#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "../../src/features/ble_clone_profile.cpp"

static clone_profile_t g_p;

void setUp(void)    { memset(&g_p, 0, sizeof(g_p)); }
void tearDown(void) {}

static void make_synthetic(clone_profile_t *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "Acme Lock");
    uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    memcpy(p->mac, mac, 6);
    uint8_t adv[] = { 0x02, 0x01, 0x06, 0x03, 0x03, 0x2C, 0xFE };
    memcpy(p->adv, adv, sizeof(adv));
    p->adv_len = sizeof(adv);

    clone_entry_t *e = &p->entries[p->n_entries++];
    e->type = CE_SERVICE;
    snprintf(e->uuid, sizeof(e->uuid), "0000fe2c-0000-1000-8000-00805f9b34fb");

    e = &p->entries[p->n_entries++];
    e->type  = CE_CHAR;
    snprintf(e->uuid, sizeof(e->uuid), "2a00");
    e->props = 0x0A;                 // read + write
    uint8_t v[] = { 'D', 'o', 'o', 'r' };
    memcpy(e->val, v, sizeof(v));
    e->val_len = sizeof(v);

    e = &p->entries[p->n_entries++];
    e->type = CE_DESC;
    snprintf(e->uuid, sizeof(e->uuid), "2902");
    e->val[0] = 0x00; e->val[1] = 0x00; e->val_len = 2;
}

void test_serialize_nonempty(void) {
    make_synthetic(&g_p);
    char out[1024];
    int n = clone_profile_serialize(&g_p, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE(strstr(out, "Acme Lock") != NULL);
    TEST_ASSERT_TRUE(strstr(out, "AABBCC112233") != NULL);
}

void test_round_trip_equal(void) {
    make_synthetic(&g_p);
    char out[1024];
    int n = clone_profile_serialize(&g_p, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_INT(0, n);

    clone_profile_t back;
    TEST_ASSERT_TRUE(clone_profile_deserialize(out, &back));

    TEST_ASSERT_EQUAL_STRING(g_p.name, back.name);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(g_p.mac, back.mac, 6);
    TEST_ASSERT_EQUAL_INT(g_p.adv_len, back.adv_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(g_p.adv, back.adv, g_p.adv_len);
    TEST_ASSERT_EQUAL_INT(g_p.n_entries, back.n_entries);
    for (int i = 0; i < g_p.n_entries; ++i) {
        TEST_ASSERT_EQUAL_INT(g_p.entries[i].type, back.entries[i].type);
        TEST_ASSERT_EQUAL_STRING(g_p.entries[i].uuid, back.entries[i].uuid);
        TEST_ASSERT_EQUAL_INT(g_p.entries[i].props, back.entries[i].props);
        TEST_ASSERT_EQUAL_INT(g_p.entries[i].val_len, back.entries[i].val_len);
        if (g_p.entries[i].val_len)
            TEST_ASSERT_EQUAL_UINT8_ARRAY(g_p.entries[i].val, back.entries[i].val,
                                          g_p.entries[i].val_len);
    }
}

void test_empty_value_char_round_trips(void) {
    memset(&g_p, 0, sizeof(g_p));
    snprintf(g_p.name, sizeof(g_p.name), "X");
    clone_entry_t *e = &g_p.entries[g_p.n_entries++];
    e->type = CE_CHAR; snprintf(e->uuid, sizeof(e->uuid), "2a4d");
    e->props = 0x10; e->val_len = 0;                 // notify-only, no value

    char out[256];
    TEST_ASSERT_GREATER_THAN_INT(0, clone_profile_serialize(&g_p, out, sizeof(out)));
    clone_profile_t back;
    TEST_ASSERT_TRUE(clone_profile_deserialize(out, &back));
    TEST_ASSERT_EQUAL_INT(1, back.n_entries);
    TEST_ASSERT_EQUAL_INT(0, back.entries[0].val_len);
    TEST_ASSERT_EQUAL_INT(0x10, back.entries[0].props);
}

void test_deserialize_rejects_garbage(void) {
    clone_profile_t back;
    TEST_ASSERT_FALSE(clone_profile_deserialize("not a profile\n", &back));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_serialize_nonempty);
    RUN_TEST(test_round_trip_equal);
    RUN_TEST(test_empty_value_char_round_trips);
    RUN_TEST(test_deserialize_rejects_garbage);
    return UNITY_END();
}
