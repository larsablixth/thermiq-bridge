/* Configuration, including Home Assistant add-on options.
 *
 * As an add-on the Supervisor writes settings to /data/options.json instead of
 * passing environment variables, and the binary reads that file itself rather
 * than relying on a shell wrapper. These check the translation both ways, and
 * that an environment variable still wins - which is what makes an add-on
 * install debuggable from a terminal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

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

static const char *OPTIONS_PATH = "/tmp/thermiq-test-options.json";

static void write_options(const char *json)
{
    FILE *file = fopen(OPTIONS_PATH, "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", OPTIONS_PATH);
        exit(2);
    }
    fputs(json, file);
    fclose(file);
    setenv("THERMIQ_OPTIONS_FILE", OPTIONS_PATH, 1);
}

static void clear_environment(void)
{
    static const char *const names[] = {
        "THERMIQ_MQTT_HOST",   "THERMIQ_MQTT_PORT", "THERMIQ_MQTT_USERNAME",
        "THERMIQ_MQTT_PASSWORD", "THERMIQ_NODE",    "THERMIQ_ID",
        "THERMIQ_LANGUAGE",    "THERMIQ_HEXFORMAT", "THERMIQ_READ_ONLY",
        "THERMIQ_DEBUG_WRITES", "THERMIQ_PUBLISH_PREFIX", "THERMIQ_DEMO",
        "THERMIQ_LOG_LEVEL",   "THERMIQ_HTTP_PORT", "THERMIQ_AVAILABILITY_TIMEOUT",
        "THERMIQ_OPTIONS_FILE",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unsetenv(names[i]);
    remove(OPTIONS_PATH);
}

static void test_defaults(void)
{
    clear_environment();
    setenv("THERMIQ_DEMO", "1", 1);
    struct config config;
    char error[256] = "";
    CHECK(config_load(&config, error, sizeof(error)));
    CHECK_STR(config.id_name, "vp1");
    CHECK_STR(config.node, "ThermIQ/ThermIQ-mqtt");
    CHECK_STR(config.data_topic, "ThermIQ/ThermIQ-mqtt/data");
    CHECK_STR(config.write_topic, "ThermIQ/ThermIQ-mqtt/write");
    CHECK(config.mqtt_port == 1883);
    CHECK(config.http_port == 8080);
}

static void test_broker_is_required_unless_demo(void)
{
    clear_environment();
    struct config config;
    char error[256] = "";
    CHECK(!config_load(&config, error, sizeof(error)));
    CHECK(strstr(error, "THERMIQ_MQTT_HOST") != NULL);
}

static void test_addon_options_are_read(void)
{
    clear_environment();
    write_options("{\"mqtt_host\": \"192.168.1.10\", \"mqtt_port\": 8883,"
                  " \"node\": \"House/Pump\", \"id\": \"vp2\", \"language\": \"se\","
                  " \"hexformat\": true, \"read_only\": true,"
                  " \"availability_timeout\": 300, \"log_level\": \"debug\"}");
    struct config config;
    char error[256] = "";
    CHECK(config_load(&config, error, sizeof(error)));
    CHECK_STR(config.mqtt_host, "192.168.1.10");
    CHECK(config.mqtt_port == 8883);
    CHECK_STR(config.node, "House/Pump");
    CHECK_STR(config.id_name, "vp2");
    CHECK_STR(config.language_code, "se");
    CHECK(config.language == 1);
    CHECK(config.hexformat);
    CHECK(config.read_only);
    CHECK(config.availability_timeout == 300.0);
    /* Derived topics must follow the option, not the default */
    CHECK_STR(config.data_topic, "House/Pump/data");
}

static void test_debug_writes_option_diverts_topics(void)
{
    clear_environment();
    write_options("{\"mqtt_host\": \"broker\", \"debug_writes\": true}");
    struct config config;
    char error[256] = "";
    CHECK(config_load(&config, error, sizeof(error)));
    CHECK_STR(config.write_topic, "ThermIQ/ThermIQ-mqtt/dbg_write");
    CHECK_STR(config.set_topic, "ThermIQ/ThermIQ-mqtt/dbg_set");
}

static void test_environment_beats_options(void)
{
    clear_environment();
    write_options("{\"mqtt_host\": \"from-options\", \"id\": \"vp2\"}");
    setenv("THERMIQ_MQTT_HOST", "from-environment", 1);
    struct config config;
    char error[256] = "";
    CHECK(config_load(&config, error, sizeof(error)));
    /* An add-on install stays debuggable from a terminal */
    CHECK_STR(config.mqtt_host, "from-environment");
    /* ...without discarding the options it did not override */
    CHECK_STR(config.id_name, "vp2");
}

static void test_empty_option_means_unset(void)
{
    clear_environment();
    /* The add-on schema defaults these to "", which must not be read as a
     * configured empty value - mqtt_host="" has to fail like a missing one. */
    write_options("{\"mqtt_host\": \"\", \"publish_prefix\": \"\"}");
    struct config config;
    char error[256] = "";
    CHECK(!config_load(&config, error, sizeof(error)));
    CHECK(strstr(error, "THERMIQ_MQTT_HOST") != NULL);
}

static void test_bad_option_is_reported(void)
{
    clear_environment();
    write_options("{\"mqtt_host\": \"broker\", \"id\": \"Not A Slug\"}");
    struct config config;
    char error[256] = "";
    CHECK(!config_load(&config, error, sizeof(error)));
    CHECK(strstr(error, "THERMIQ_ID") != NULL);

    clear_environment();
    write_options("{\"mqtt_host\": \"broker\", \"language\": \"kl\"}");
    CHECK(!config_load(&config, error, sizeof(error)));
    CHECK(strstr(error, "THERMIQ_LANGUAGE") != NULL);

    clear_environment();
    write_options("{\"mqtt_host\": \"broker\", \"node\": \"House/#\"}");
    CHECK(!config_load(&config, error, sizeof(error)));
    CHECK(strstr(error, "THERMIQ_NODE") != NULL);
}

static void test_missing_or_broken_options_file_is_not_fatal(void)
{
    clear_environment();
    setenv("THERMIQ_OPTIONS_FILE", "/tmp/thermiq-does-not-exist.json", 1);
    setenv("THERMIQ_MQTT_HOST", "broker", 1);
    struct config config;
    char error[256] = "";
    /* Not running as an add-on is the normal case, not an error */
    CHECK(config_load(&config, error, sizeof(error)));

    clear_environment();
    write_options("this is not json");
    setenv("THERMIQ_MQTT_HOST", "broker", 1);
    CHECK(config_load(&config, error, sizeof(error)));
    CHECK_STR(config.mqtt_host, "broker");
}

static void test_publish_prefix_must_differ_from_node(void)
{
    clear_environment();
    write_options("{\"mqtt_host\": \"broker\", \"node\": \"A/B\","
                  " \"publish_prefix\": \"A/B\"}");
    struct config config;
    char error[256] = "";
    CHECK(!config_load(&config, error, sizeof(error)));
    CHECK(strstr(error, "PUBLISH_PREFIX") != NULL);
}

int main(void)
{
    test_defaults();
    test_broker_is_required_unless_demo();
    test_addon_options_are_read();
    test_debug_writes_option_diverts_topics();
    test_environment_beats_options();
    test_empty_option_means_unset();
    test_bad_option_is_reported();
    test_missing_or_broken_options_file_is_not_fatal();
    test_publish_prefix_must_differ_from_node();
    clear_environment();

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d config checks passed\n", checks);
    return 0;
}
