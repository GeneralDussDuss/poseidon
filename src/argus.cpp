/*
 * argus.cpp — mood-portrait renderer with subtle procedural overlays.
 *
 * Renders one of ten 48x48 RGB565 sprites (argus_data.h) with these
 * effects on top:
 *   - Subtle vertical sway (breathing) on a slow sine
 *   - Mood-specific accents drawn AFTER the sprite blit:
 *       OLD_FURY    -> random lightning crackle around the head
 *       SLEEPING    -> floating "z" glyphs drifting up
 *       CALCULATING -> magenta scan-line crosses the third eye
 *       ANNOYED     -> brief glitch flashes
 *   - One-shot flash override (argus_flash(mood, ms)) — flips to that
 *     mood for ms milliseconds before falling back to the caller's
 *     base mood. Useful for "PLEASED" bursts on handshake capture.
 *
 * Sprite IS re-pushed every call. Caller is expected to invoke this
 * after its own body-clear / right-zone-redraw. Doing it any other
 * way leaves a black gap between clear and re-push (flicker).
 */
#include "argus.h"
#include "theme.h"
#include "app.h"
#include "heap_budget.h"
#include <M5Cardputer.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

static argus_mood_t s_flash_mood   = ARGUS_WATCHING;
static uint32_t     s_flash_until  = 0;
/* Cache: caller redraw cycle doesn't clear our region (partial redraw
 * pattern in triton.cpp), so we only push the sprite when state
 * actually changed. */
static argus_mood_t s_last_mood = (argus_mood_t)-1;
static int          s_last_sway = INT32_MIN;
static int          s_last_x    = INT32_MIN;
static int          s_last_y    = INT32_MIN;

/* RAM-backed copy of the current mood sprite. Pushing the 18 KB image
 * directly from flash .rodata while `esp_wifi_80211_tx` is firing
 * triggers an MMU cache stall mid-DMA and scrambles the tile (Porkchop
 * + Bruce sidestep this by rendering text-only into a Sprite canvas).
 * Caching ONE sprite in internal SRAM and pushing from there avoids
 * the flash fetch entirely. Allocated lazily on first draw. */
static uint16_t    *s_ram_sprite = nullptr;
static argus_mood_t s_ram_mood   = (argus_mood_t)-1;
static bool         s_ram_sprite_tried = false;

// Reclaimable cache: the ~18 KB DMA mascot sprite refills from flash on the
// next draw, so it yields to any RF feature that needs the internal heap.
static void heap_argus_release(void) {
    if (s_ram_sprite) { heap_caps_free(s_ram_sprite); s_ram_sprite = nullptr; }
    s_ram_sprite_tried = false;      // allow a fresh alloc attempt on next draw
    s_ram_mood = (argus_mood_t)-1;   // force the memcpy refresh when it returns
}

void argus_flash(argus_mood_t mood, uint32_t ms)
{
    s_flash_mood  = mood;
    s_flash_until = millis() + ms;
}

void argus_invalidate(void)
{
    /* Force the next draw to push. Sentinels match the initial values so
     * the mood/x/y/sway comparison in argus_draw always mismatches once. */
    s_last_mood = (argus_mood_t)-1;
    s_last_sway = INT32_MIN;
    s_last_x    = INT32_MIN;
    s_last_y    = INT32_MIN;
}

static void overlay_lightning(const uint16_t *src, int x, int y)
{
    auto &d = M5Cardputer.Display;
    /* Wipe last frame's bolts by re-pushing the top strip of the cached
     * face, then crackle DOWNWARD from the crown — kept inside the sprite
     * so the bolts never bleed into (or fail to clear from) the status bar
     * above the head. Only re-push when the source is the RAM sprite; a
     * per-frame flash push during STORM's heavy TX would scramble it. */
    const int STRIP = 20;
    if (src == s_ram_sprite && src)
        d.pushImage(x, y, ARGUS_W, STRIP, src);
    int strikes = 1 + (int)(esp_random() % 2);
    for (int s = 0; s < strikes; ++s) {
        int sx = x + (esp_random() % ARGUS_W);
        int sy = y + (int)(esp_random() % 3);
        int mx = sx + (int)(esp_random() % 7) - 3;
        int my = sy + (3 + (int)(esp_random() % 5));
        int ex = mx + (int)(esp_random() % 5) - 2;
        int ey = my + (2 + (int)(esp_random() % 4));
        if (my > y + STRIP - 1) my = y + STRIP - 1;
        if (ey > y + STRIP - 1) ey = y + STRIP - 1;
        uint16_t c = (esp_random() & 1) ? 0xFFFF : T_ACCENT;
        d.drawLine(sx, sy, mx, my, c);
        d.drawLine(mx, my, ex, ey, c);
    }
}

static void overlay_zzz(int x, int y, uint32_t now)
{
    auto &d = M5Cardputer.Display;
    for (int i = 0; i < 2; ++i) {
        uint32_t phase = (now / 40 + i * 30) % 80;
        if (phase < 5) continue;
        int zx = x + ARGUS_W + 1 + i * 3;
        int zy = y + 2 - (int)(phase / 2);
        uint16_t col = (phase < 30) ? T_ACCENT : (phase < 55) ? T_DIM : 0x18C3;
        d.setTextColor(col, T_BG);
        d.setCursor(zx, zy);
        d.print("z");
    }
}

static void overlay_scan_line(int x, int y, uint32_t now)
{
    auto &d = M5Cardputer.Display;
    /* Magenta scan-line crosses the third-eye region every ~2 s. */
    uint32_t phase = now % 2000;
    if (phase > 500) return;
    int sline_y = y + 6 + (int)((phase * 5) / 500);
    d.drawFastHLine(x + 18, sline_y, 12, T_ACCENT2);
}

static void overlay_glitch(int x, int y, uint32_t now)
{
    auto &d = M5Cardputer.Display;
    if ((now / 200) % 5 != 0) return;
    for (int i = 0; i < 3; ++i) {
        int sy = y + (int)(esp_random() % ARGUS_H);
        int sx = x + (int)(esp_random() % (ARGUS_W - 6));
        int len = 3 + (int)(esp_random() % 6);
        uint16_t c = (esp_random() & 1) ? T_ACCENT2 : T_ACCENT;
        d.drawFastHLine(sx, sy, len, c);
    }
}

void argus_draw(argus_mood_t mood, int x, int y)
{
    auto &d = M5Cardputer.Display;
    static bool s_reg = false;
    if (!s_reg) { s_reg = true; heap_reclaim_register(heap_argus_release); }
    uint32_t now = millis();

    argus_mood_t cur = mood;
    if (now < s_flash_until) cur = s_flash_mood;

    /* Sway disabled at 96x96 — even ±1 px per-frame pushImage of 18 KB
     * fragmented WiFi DMA and visibly scrambled the sprite. With sway=0
     * the cache only fires on actual mood change, which is rare. */
    const int sway = 0;

    int idx = (int)cur;
    if (idx < 0 || idx >= ARGUS_MOOD_COUNT) idx = ARGUS_WATCHING;

    /* Lazy-allocate the RAM sprite ONCE. Old code only checked
     * `if (!s_ram_sprite)` which means a failed alloc (returns NULL,
     * leaving the pointer null) re-tries on EVERY redraw call.
     * In low-heap situations that turns into 18 KB malloc spam every
     * frame — fragmented the heap and starved WiFi capture, found
     * via Triton serial trace (internal RAM was 384 BYTES free,
     * Argus spammed malloc unable to fit). The s_ram_sprite_tried
     * flag pins it: one attempt, then fall back to direct-from-flash
     * pushImage (already wired below). */
    if (!s_ram_sprite && !s_ram_sprite_tried) {
        s_ram_sprite_tried = true;
        size_t bytes = (size_t)ARGUS_W * ARGUS_H * sizeof(uint16_t);
        s_ram_sprite = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        Serial.printf("[argus] ram sprite alloc %u bytes -> %p (free internal=%u)\n",
                      (unsigned)bytes, s_ram_sprite,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (!s_ram_sprite) {
            Serial.println("[argus] fallback: direct-from-flash pushImage");
        }
    }

    /* Refresh the RAM copy on actual mood change only — this is the only
     * remaining flash read, and it's a single memcpy with no DMA. */
    if (s_ram_sprite && cur != s_ram_mood) {
        size_t bytes = (size_t)ARGUS_W * ARGUS_H * sizeof(uint16_t);
        memcpy(s_ram_sprite, ARGUS_SPRITES[idx], bytes);
        s_ram_mood = cur;
    }

    /* Only push when something visible changed. Source is RAM (or flash
     * fallback if alloc failed), not flash directly — no MMU cache
     * contention with WiFi TX. */
    const uint16_t *src = s_ram_sprite ? s_ram_sprite : ARGUS_SPRITES[idx];
    if (cur != s_last_mood || sway != s_last_sway
        || x != s_last_x || y != s_last_y) {
        d.pushImage(x, y + sway, ARGUS_W, ARGUS_H, src);
        s_last_mood = cur;
        s_last_sway = sway;
        s_last_x    = x;
        s_last_y    = y;
    }

    /* ALL face overlays removed. The sprite is cached and only re-pushes
     * on a mood change (sway=0), so ANYTHING drawn over it persists as a
     * trail — lightning, glitch, the scan-line, and the floating zzz all
     * bled over him. Mood now reads entirely from the per-mood sprite. */
    (void)src; (void)now;
}
