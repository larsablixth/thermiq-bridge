/* Decode and encode parity with the Home Assistant integration.
 *
 * Every case here mirrors one in the repository's tests/test_heatpump.py, so
 * the bridge and the integration agree on what a payload means and on what
 * may be written back. The expected values are the Python test's, not
 * re-derived.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "registers.h"
#include "state.h"
#include "util.h"
#include "widget.h"

static int failures;
static int checks;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(condition)) {                                                              \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
        }                                                                                \
    } while (0)

#define CHECK_STR(actual, expected)                                                      \
    do {                                                                                 \
        checks++;                                                                        \
        if (strcmp((actual), (expected)) != 0) {                                         \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", __FILE__,       \
                    __LINE__, (expected), (actual));                                     \
        }                                                                                \
    } while (0)

/* state_ingest mutates its input and needs a spare byte for a terminator. */
static int ingest(struct pump_state *state, const char *payload)
{
    static char scratch[8192];
    size_t length = strlen(payload);
    memcpy(scratch, payload, length + 1);
    return state_ingest(state, scratch, length);
}

static double number_of(const struct pump_state *state, const char *key)
{
    const struct reg_value *value = state_value(state, reg_find(key));
    return value ? value->number : NAN;
}

static void entity(const struct pump_state *state, const char *domain, const char *key,
                   char *out, size_t cap)
{
    state_entity_string(state, domain, reg_find(key), out, cap);
}

/* ---- decoding --------------------------------------------------------- */

static void test_message_populates_state_and_combines_decimals(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    /* r01 is whole degrees, r02 tenths: 21 + 4/10 */
    CHECK(ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r00\": -3, \"r01\": 21,"
                         " \"r02\": 4}") > 0);
    CHECK(number_of(&state, "outdoor_t") == -3.0);
    CHECK(fabs(number_of(&state, "indoor_t") - 21.4) < 1e-9);
    CHECK(state.message_count == 1);
    CHECK(state_available(&state));
}

static void test_decimals_not_combined_when_absent(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r00\": 7}");
    CHECK(number_of(&state, "outdoor_t") == 7.0);
    /* A second message without r01 must not compound the tenths again */
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r01\": 20, \"r02\": 5}");
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r00\": 8}");
    CHECK(fabs(number_of(&state, "indoor_t") - 20.5) < 1e-9);
}

static void test_foreign_payload_is_ignored(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    CHECK(ingest(&state, "{\"Client_Name\": \"Other\", \"r00\": 1}") == -1);
    CHECK(state.message_count == 0);
    CHECK(!state_available(&state));
}

static void test_invalid_json_is_ignored(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    CHECK(ingest(&state, "{not valid json") == -1);
    CHECK(ingest(&state, "[1,2,3]") == -1);
    CHECK(ingest(&state, "") == -1);
    CHECK(ingest(&state, "{}") == -1);
    CHECK(state.message_count == 0);
}

static void test_unknown_key_does_not_abort_processing(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    CHECK(ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"dXX\": 1, \"r00\": 9}") > 0);
    CHECK(number_of(&state, "outdoor_t") == 9.0);
}

static void test_decimal_and_hex_keys_are_equivalent(void)
{
    struct pump_state decimal;
    struct pump_state hex;
    state_init(&decimal, 120.0, 0);
    state_init(&hex, 120.0, 0);
    /* d000 is r00, d010 is r0a - the old firmware speaks hex, the new one
     * decimal, and both must land in the same register. */
    ingest(&decimal, "{\"Client_Name\": \"ThermIQ_x\", \"d000\": -3, \"d010\": 12}");
    ingest(&hex, "{\"Client_Name\": \"ThermIQ_x\", \"r00\": -3, \"r0a\": 12}");
    CHECK(number_of(&decimal, "outdoor_t") == number_of(&hex, "outdoor_t"));
    CHECK(number_of(&decimal, "cooling_t") == number_of(&hex, "cooling_t"));
}

static void test_generated_fields(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"time\": \"12:34\", \"r00\": 1}");
    char text[STATE_STRING_MAX];
    entity(&state, "sensor", "time_str", text, sizeof(text));
    CHECK_STR(text, "12:34");
    /* No vp_read in the message means communication is fine */
    entity(&state, "sensor", "communication_status", text, sizeof(text));
    CHECK_STR(text, "Ok");
    entity(&state, "sensor", "mqtt_counter", text, sizeof(text));
    CHECK_STR(text, "1");

    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"vp_read\": \"Timeout\"}");
    entity(&state, "sensor", "communication_status", text, sizeof(text));
    CHECK_STR(text, "Timeout");
    entity(&state, "sensor", "mqtt_counter", text, sizeof(text));
    CHECK_STR(text, "2");
}

static void test_availability_reflects_last_message(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    CHECK(!state_available(&state));
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r00\": 1}");
    CHECK(state_available(&state));
    /* Pretend the last message was long ago */
    state.last_message = now_monotonic() - 121.0;
    CHECK(!state_available(&state));

    char text[STATE_STRING_MAX];
    entity(&state, "sensor", "outdoor_t", text, sizeof(text));
    CHECK_STR(text, "unavailable");
}

/* ---- entity strings --------------------------------------------------- */

static void test_entity_strings(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    /* r0d bit 0 is boiler_3kw_on, bit 1 is boiler_6kw_on; r33 is the mode */
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r00\": -3, \"r01\": 21,"
                   " \"r02\": 0, \"r0d\": 1, \"r33\": 2, \"r70\": 0}");

    char text[STATE_STRING_MAX];
    /* An integer register renders as an integer */
    entity(&state, "sensor", "outdoor_t", text, sizeof(text));
    CHECK_STR(text, "-3");
    /* A combined one is a float even when the tenths are zero */
    entity(&state, "sensor", "indoor_t", text, sizeof(text));
    CHECK_STR(text, "21.0");
    entity(&state, "binary_sensor", "boiler_3kw_on", text, sizeof(text));
    CHECK_STR(text, "on");
    entity(&state, "binary_sensor", "boiler_6kw_on", text, sizeof(text));
    CHECK_STR(text, "off");
    entity(&state, "select", "main_mode", text, sizeof(text));
    CHECK_STR(text, "2 - Heatpump only");
    /* A register that never arrived is unavailable, not zero and not
     * unknown: messages are flowing and this one has never been in any of
     * them, so the hardware does not have it. "unknown" would suggest a value
     * is still coming. */
    entity(&state, "sensor", "hgw_water_t", text, sizeof(text));
    CHECK_STR(text, "unavailable");
}

static void test_select_options_translate(void)
{
    char label[STATE_STRING_MAX];
    CHECK(state_mode_option(0, 2, label, sizeof(label)));
    CHECK_STR(label, "2 - Heatpump only");
    CHECK(state_mode_option(1, 2, label, sizeof(label)));
    CHECK_STR(label, "2 - Bara värmepump");
    CHECK(!state_mode_option(0, 9, label, sizeof(label)));

    double value = -1;
    CHECK(state_mode_value(0, "2 - Heatpump only", &value) && value == 2);
    CHECK(!state_mode_value(0, "2 - Bara värmepump", &value));
    CHECK(!state_mode_value(0, "nonsense", &value));
}

/* ---- writes ----------------------------------------------------------- */

static struct write_topics topics(bool hexformat)
{
    struct write_topics t = {"ThermIQ/ThermIQ-mqtt/write", "ThermIQ/ThermIQ-mqtt/set",
                             hexformat};
    return t;
}

static struct pump_state writable_state(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    /* A ThermIQ-Room2 reporting everything. Writes are only accepted for
     * registers the pump actually sends, so a fixture that reports one
     * register can only ever test the refusal path. */
    ingest(&state,
           "{\"Client_Name\": \"ThermIQ_x\", \"EVU\": 0, \"INDR_T\": 21.0, "
           "\"d0\": 1, \"d1\": 1, \"d2\": 1, \"d3\": 1, \"d4\": 1, \"d5\": 1, \"d6\": 1, \"d7\": 1, \"d8\": 1, \"d9\": 1, \"d10\": 1, \"d11\": 1, \"d12\": 1, \"d13\": 1, \"d14\": 1, \"d15\": 1, \"d16\": 1, \"d17\": 1, \"d18\": 1, \"d19\": 1, \"d20\": 1, \"d21\": 1, \"d22\": 1, \"d23\": 1, \"d24\": 1, \"d25\": 1, \"d26\": 1, \"d27\": 1, \"d28\": 1, \"d29\": 1, \"d30\": 1, \"d31\": 1, \"d32\": 1, \"d33\": 1, \"d34\": 1, \"d35\": 1, \"d36\": 1, \"d37\": 1, \"d38\": 1, \"d39\": 1, \"d40\": 1, \"d41\": 1, \"d42\": 1, \"d43\": 1, \"d44\": 1, \"d45\": 1, \"d46\": 1, \"d47\": 1, \"d48\": 1, \"d49\": 1, \"d50\": 1, \"d51\": 1, \"d52\": 1, \"d53\": 1, \"d54\": 1, \"d55\": 1, \"d56\": 1, \"d57\": 1, \"d58\": 1, \"d59\": 1, \"d60\": 1, \"d61\": 1, \"d62\": 1, \"d63\": 1, \"d64\": 1, \"d65\": 1, \"d66\": 1, \"d67\": 1, \"d68\": 1, \"d69\": 1, \"d70\": 1, \"d71\": 1, \"d72\": 1, \"d73\": 1, \"d74\": 1, \"d75\": 1, \"d76\": 1, \"d77\": 1, \"d78\": 1, \"d79\": 1, \"d80\": 1, \"d81\": 1, \"d82\": 1, \"d83\": 1, \"d84\": 1, \"d85\": 1, \"d86\": 1, \"d87\": 1, \"d88\": 1, \"d89\": 1, \"d90\": 1, \"d91\": 1, \"d92\": 1, \"d93\": 1, \"d94\": 1, \"d95\": 1, \"d96\": 1, \"d97\": 1, \"d98\": 1, \"d99\": 1, \"d100\": 1, \"d101\": 1, \"d102\": 1, \"d103\": 1, \"d104\": 1, \"d105\": 1, \"d106\": 1, \"d107\": 1, \"d108\": 1, \"d109\": 1, \"d110\": 1, \"d111\": 1, \"d112\": 1, \"d113\": 1, \"d114\": 1, \"d115\": 1, \"d116\": 1, \"d117\": 1, \"d118\": 1, \"d119\": 1, \"d120\": 1, \"d121\": 1, \"d122\": 1, \"d123\": 1, \"d124\": 1, \"d125\": 1, \"d126\": 1, \"d127\": 1}");
    return state;
}

static void test_write_publishes_value_in_range(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    /* indoor_requested_t (r32) allows 0..50 and is published as d050 */
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", 21, &message) == WRITE_OK);
    CHECK_STR(message.topic, "ThermIQ/ThermIQ-mqtt/write");
    CHECK_STR(message.payload, "{\"d050\": 21}");
}

static void test_write_rejects_value_out_of_range(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", 95, &message) ==
          WRITE_OUT_OF_RANGE);
}

static void test_write_rejects_non_boolean_for_switch_register(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "heatpump_evu_block", 5, &message) ==
          WRITE_NOT_BOOLEAN);
}

static void test_write_accepts_boolean_for_switch_register(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "heatpump_evu_block", 1, &message) == WRITE_OK);
    CHECK_STR(message.topic, "ThermIQ/ThermIQ-mqtt/set");
    CHECK_STR(message.payload, "{\"EVU\": 1}");
}

static void test_write_accepts_negative_value_in_range(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    /* integral1_curve_p5 (r37) allows -5..5; the wire format is 16-bit two's
     * complement, so -3 must be published as 65533 */
    CHECK(state_encode_write(&state, &t, "integral1_curve_p5", -3, &message) == WRITE_OK);
    CHECK_STR(message.payload, "{\"d055\": 65533}");
}

static void test_write_rejects_negative_value_below_min(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "integral1_curve_p5", -6, &message) ==
          WRITE_OUT_OF_RANGE);
}

static void test_write_rejects_read_only_and_unknown_registers(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "outdoor_t", 5, &message) == WRITE_NOT_WRITABLE);
    CHECK(state_encode_write(&state, &t, "compressor_on", 1, &message) ==
          WRITE_NOT_WRITABLE);
    CHECK(state_encode_write(&state, &t, "no_such_register", 1, &message) ==
          WRITE_UNKNOWN_KEY);
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", NAN, &message) ==
          WRITE_NOT_NUMERIC);
}

static void test_write_refused_before_any_data(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    struct write_topics t = topics(false);
    struct write_message message;
    /* The integration refuses to write before the pump has spoken */
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", 21, &message) ==
          WRITE_NO_DATA);
}

static void test_write_hexformat_uses_hex_register_keys(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(true);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", 21, &message) == WRITE_OK);
    CHECK_STR(message.payload, "{\"r32\": 21}");
}

static void test_write_room_sensor_keeps_its_fraction(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    /* room_sensor_set_t is the one register sent as a float, on the set topic */
    CHECK(state_encode_write(&state, &t, "room_sensor_set_t", 21.5, &message) == WRITE_OK);
    CHECK_STR(message.topic, "ThermIQ/ThermIQ-mqtt/set");
    CHECK_STR(message.payload, "{\"INDR_T\": 21.5}");
}

static void test_debug_writes_divert_to_dbg_topics(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = {"ThermIQ/ThermIQ-mqtt/dbg_write",
                             "ThermIQ/ThermIQ-mqtt/dbg_set", false};
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", 21, &message) == WRITE_OK);
    CHECK_STR(message.topic, "ThermIQ/ThermIQ-mqtt/dbg_write");
    CHECK(state_encode_write(&state, &t, "heatpump_evu_block", 0, &message) == WRITE_OK);
    CHECK_STR(message.topic, "ThermIQ/ThermIQ-mqtt/dbg_set");
}

/* ---- number formatting ------------------------------------------------ */

static void test_number_formatting_matches_python(void)
{
    char out[40];
    fmt_double(out, sizeof(out), 274.0);
    CHECK_STR(out, "274.0");
    fmt_double(out, sizeof(out), 0.85);
    CHECK_STR(out, "0.85");
    fmt_double(out, sizeof(out), -3.5);
    CHECK_STR(out, "-3.5");
    fmt_double(out, sizeof(out), 21.4);
    CHECK_STR(out, "21.4");
    fmt_double(out, sizeof(out), 0.0);
    CHECK_STR(out, "0.0");
}

/* ---- capability detection ------------------------------------------- */
/* EVU is ThermIQ-Room2 only; the room-sensor setpoint needs a Room or a
 * Room2. The pump says which it is by what it puts in /data - confirmed by
 * ThermIQ: "the mqtt message will only contain EVU or INDR_T if it's
 * supported by the hw". */

static struct pump_state plain_mqtt_state(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    /* Plain ThermIQ-MQTT: registers, but never EVU or INDR_T. */
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"d0\": 21, \"d50\": 15}");
    return state;
}

static void test_plain_hardware_refuses_writes_it_cannot_perform(void)
{
    struct pump_state state = plain_mqtt_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "heatpump_evu_block", 1, &message)
          == WRITE_UNSUPPORTED);
    CHECK(state_encode_write(&state, &t, "room_sensor_set_t", 21.0, &message)
          == WRITE_UNSUPPORTED);
    /* ...while a register it does report is writable as before */
    CHECK(state_encode_write(&state, &t, "indoor_requested_t", 21, &message) == WRITE_OK);
}

static void test_room2_accepts_the_writes_plain_hardware_cannot(void)
{
    struct pump_state state = writable_state();
    struct write_topics t = topics(false);
    struct write_message message;
    CHECK(state_encode_write(&state, &t, "heatpump_evu_block", 1, &message) == WRITE_OK);
    CHECK(state_encode_write(&state, &t, "room_sensor_set_t", 21.0, &message) == WRITE_OK);
}

static void test_unsupported_register_reads_as_unavailable(void)
{
    struct pump_state state = plain_mqtt_state();
    char text[STATE_STRING_MAX];
    entity(&state, "switch", "heatpump_evu_block", text, sizeof(text));
    CHECK_STR(text, "unavailable");
    entity(&state, "number", "room_sensor_set_t", text, sizeof(text));
    CHECK_STR(text, "unavailable");
}

/* ---- the pool branch --------------------------------------------------- */

/* What the widget would read for binary_sensor.pool_heating_active. In Home
 * Assistant that is a template sensor the user writes; here it is derived. */
static void pool_state(const struct pump_state *state, char *out, size_t cap)
{
    static struct widget_states states;
    state_widget_states(state, &states);
    for (int i = 0; i < WIDGET_ENTITY_COUNT; i++) {
        if (strcmp(WIDGET_ENTITIES[i].id, "binary_sensor.pool_heating_active") == 0) {
            snprintf(out, cap, "%s", states.entries[i]);
            return;
        }
    }
    snprintf(out, cap, "<no such entity>");
}

/* r10 carries all three flags: compressor 0x02, supply pump 0x04, DHW 0x08. */
static struct pump_state pool_running_state(bool pool_circuit)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    state.pool_circuit = pool_circuit;
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r10\": 6, \"r40\": 27}");
    return state;
}

static void test_pool_branch_is_off_unless_configured(void)
{
    struct pump_state state = pool_running_state(false);
    char text[STATE_STRING_MAX];
    pool_state(&state, text, sizeof(text));
    /* Every condition holds; only the opt-in is missing. */
    CHECK_STR(text, "off");
}

static void test_pool_branch_on_when_the_circuit_is_being_heated(void)
{
    struct pump_state state = pool_running_state(true);
    char text[STATE_STRING_MAX];
    pool_state(&state, text, sizeof(text));
    CHECK_STR(text, "on");
}

static void test_pool_branch_off_below_the_curve_minimum(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    state.pool_circuit = true;
    char text[STATE_STRING_MAX];
    /* 10 is the pump's "auto", i.e. the off value - not 0. */
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r10\": 6, \"r40\": 10}");
    pool_state(&state, text, sizeof(text));
    CHECK_STR(text, "off");
}

static void test_pool_branch_off_while_making_hot_water(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    state.pool_circuit = true;
    char text[STATE_STRING_MAX];
    /* 0x0E = compressor + supply pump + hot water. The diverter valve sends
     * the heat to the tank, so the circuit gets none of it. */
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r10\": 14, \"r40\": 27}");
    pool_state(&state, text, sizeof(text));
    CHECK_STR(text, "off");
}

static void test_pool_branch_off_when_the_compressor_is_idle(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    state.pool_circuit = true;
    char text[STATE_STRING_MAX];
    /* 0x04 = supply pump only. Circulating, but producing nothing. */
    ingest(&state, "{\"Client_Name\": \"ThermIQ_x\", \"r10\": 4, \"r40\": 27}");
    pool_state(&state, text, sizeof(text));
    CHECK_STR(text, "off");
}

static void test_pool_branch_off_before_any_data(void)
{
    struct pump_state state;
    state_init(&state, 120.0, 0);
    state.pool_circuit = true;
    char text[STATE_STRING_MAX];
    pool_state(&state, text, sizeof(text));
    CHECK_STR(text, "off");
}

int main(void)
{
    test_message_populates_state_and_combines_decimals();
    test_decimals_not_combined_when_absent();
    test_foreign_payload_is_ignored();
    test_invalid_json_is_ignored();
    test_unknown_key_does_not_abort_processing();
    test_decimal_and_hex_keys_are_equivalent();
    test_generated_fields();
    test_availability_reflects_last_message();
    test_entity_strings();
    test_select_options_translate();
    test_write_publishes_value_in_range();
    test_write_rejects_value_out_of_range();
    test_write_rejects_non_boolean_for_switch_register();
    test_write_accepts_boolean_for_switch_register();
    test_write_accepts_negative_value_in_range();
    test_write_rejects_negative_value_below_min();
    test_write_rejects_read_only_and_unknown_registers();
    test_write_refused_before_any_data();
    test_write_hexformat_uses_hex_register_keys();
    test_write_room_sensor_keeps_its_fraction();
    test_debug_writes_divert_to_dbg_topics();
    test_number_formatting_matches_python();
    test_plain_hardware_refuses_writes_it_cannot_perform();
    test_room2_accepts_the_writes_plain_hardware_cannot();
    test_unsupported_register_reads_as_unavailable();
    test_pool_branch_is_off_unless_configured();
    test_pool_branch_on_when_the_circuit_is_being_heated();
    test_pool_branch_off_below_the_curve_minimum();
    test_pool_branch_off_while_making_hot_water();
    test_pool_branch_off_when_the_compressor_is_idle();
    test_pool_branch_off_before_any_data();

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d state checks passed\n", checks);
    return 0;
}
