/* `thermiq-bridge --discover`: find the pump on the broker.
 *
 * The MQTT node name is the one setting nobody can look up and nearly
 * everybody gets wrong: the values are on `<node>/data`, the default is only
 * a default, and a wrong one looks exactly like a broken pump - the bridge
 * connects, subscribes, and simply never hears anything.
 *
 * So rather than sending people to MQTT Explorer, the binary listens to the
 * whole broker for a while and reports what it found: the node name to
 * configure, and whether the firmware speaks decimal or hex register keys,
 * which is the other setting that cannot be guessed. It reads only; it
 * publishes nothing.
 */
#include "discover.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "mqtt.h"
#include "util.h"

#define MAX_CANDIDATES 8
/* Long enough to catch a pump that publishes every ~30 s. Overridable, which
 * is how the tests get a two-second run out of it. */
#define LISTEN_SECONDS_DEFAULT 45.0

static double listen_seconds(void)
{
    const char *override = getenv("THERMIQ_DISCOVER_SECONDS");
    if (override && *override) {
        char *end = NULL;
        double value = strtod(override, &end);
        if (end != override && *end == '\0' && value >= 1.0 && value <= 600.0)
            return value;
    }
    return LISTEN_SECONDS_DEFAULT;
}

struct candidate {
    char node[MQTT_TOPIC_MAX];
    char client_name[64];
    unsigned messages;
    bool hex_keys;
    bool decimal_keys;
    unsigned registers;
};

struct discovery {
    struct candidate found[MAX_CANDIDATES];
    int count;
    unsigned foreign;
    unsigned total;
};

/* A ThermIQ data message identifies itself, and its register keys reveal
 * which format the firmware speaks. */
static bool inspect(char *payload, size_t len, struct candidate *out)
{
    static struct json_member members[JSON_MAX_MEMBERS];
    int count = json_parse_object(payload, len, members, JSON_MAX_MEMBERS);
    if (count < 0)
        return false;

    const struct json_member *client = json_get(members, count, "Client_Name");
    if (!client || client->kind != JSON_STRING ||
        strncmp(client->string, "ThermIQ_", 8) != 0)
        return false;

    snprintf(out->client_name, sizeof(out->client_name), "%s", client->string);
    for (int i = 0; i < count; i++) {
        const char *key = members[i].key;
        size_t length = strlen(key);
        if (key[0] == 'd' && length == 4) {
            out->decimal_keys = true;
            out->registers++;
        } else if (key[0] == 'r' && length == 3) {
            out->hex_keys = true;
            out->registers++;
        }
    }
    return true;
}

static void on_message(void *user, const char *topic, char *payload, size_t len)
{
    struct discovery *state = user;
    state->total++;

    /* Only the data topic carries the register payload, and its prefix is
     * exactly the node name we are looking for. */
    const char *suffix = strstr(topic, "/data");
    if (!suffix || suffix[5] != '\0') {
        return;
    }

    struct candidate probe = {{0}, {0}, 0, false, false, 0};
    if (!inspect(payload, len, &probe)) {
        state->foreign++;
        return;
    }

    size_t node_len = (size_t)(suffix - topic);
    for (int i = 0; i < state->count; i++) {
        if (strncmp(state->found[i].node, topic, node_len) == 0 &&
            state->found[i].node[node_len] == '\0') {
            state->found[i].messages++;
            state->found[i].hex_keys |= probe.hex_keys;
            state->found[i].decimal_keys |= probe.decimal_keys;
            state->found[i].registers = probe.registers;
            return;
        }
    }
    if (state->count >= MAX_CANDIDATES)
        return;

    struct candidate *found = &state->found[state->count++];
    snprintf(found->node, sizeof(found->node), "%.*s", (int)node_len, topic);
    snprintf(found->client_name, sizeof(found->client_name), "%s", probe.client_name);
    found->messages = 1;
    found->hex_keys = probe.hex_keys;
    found->decimal_keys = probe.decimal_keys;
    found->registers = probe.registers;
    printf("  found %s (%s), %u registers\n", found->node, found->client_name,
           found->registers);
    fflush(stdout);
}

int discover_run(const struct config *config)
{
    struct discovery state = {{{{0}, {0}, 0, false, false, 0}}, 0, 0, 0};

    struct mqtt_config mqtt_config = {
        .host = config->mqtt_host,
        .port = config->mqtt_port,
        .client_id = config->mqtt_client_id,
        .username = config->mqtt_username,
        .password = config->mqtt_password,
        /* Everything, because the point is that we do not know the node. */
        .subscribe_topic = "#",
        .keepalive = 60,
    };

    struct mqtt_client client;
    mqtt_init(&client, &mqtt_config, on_message, &state);

    double seconds = listen_seconds();
    printf("Listening to %s:%u for %.0f seconds...\n", config->mqtt_host,
           config->mqtt_port, seconds);
    fflush(stdout);

    double deadline = now_monotonic() + seconds;
    while (now_monotonic() < deadline) {
        mqtt_service(&client, false, false);
        int fd = mqtt_fd(&client);
        if (fd < 0) {
            /* Backing off between connection attempts. */
            struct pollfd nothing = {-1, 0, 0};
            poll(&nothing, 0, 200);
            continue;
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        if (mqtt_wants_write(&client))
            pfd.events |= POLLOUT;
        int ready = poll(&pfd, 1, 500);
        if (ready < 0)
            break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            mqtt_close(&client, "socket error");
        else if (pfd.revents)
            mqtt_service(&client, (pfd.revents & POLLIN) != 0,
                         (pfd.revents & POLLOUT) != 0);
    }
    bool was_connected = mqtt_connected(&client);
    mqtt_close(&client, NULL);

    printf("\n");
    if (!was_connected && state.total == 0) {
        printf("Could not talk to the broker at %s:%u.\n", config->mqtt_host,
               config->mqtt_port);
        if (client.last_error[0])
            printf("  %s\n", client.last_error);
        printf("Check the address, and the username and password if it needs "
               "them.\n");
        return 2;
    }

    if (state.count == 0) {
        printf("No ThermIQ device found. Saw %u message(s) on the broker", state.total);
        if (state.foreign)
            printf(", %u of them on a /data topic but not from a ThermIQ device",
                   state.foreign);
        printf(".\n\n");
        printf("The pump publishes about every 30 seconds, so either it is not\n"
               "connected to this broker, or it is not powered on. Check it with\n"
               "MQTT Explorer, or try again.\n");
        return 1;
    }

    printf("Found %d heat pump%s:\n\n", state.count, state.count == 1 ? "" : "s");
    for (int i = 0; i < state.count; i++) {
        const struct candidate *found = &state.found[i];
        /* Old 1.xx firmware sends hex register keys and expects them back. */
        bool hex = found->hex_keys && !found->decimal_keys;
        printf("  THERMIQ_NODE=%s\n", found->node);
        printf("  THERMIQ_HEXFORMAT=%s\n", hex ? "true" : "false");
        printf("    device %s, %u messages in %.0fs, %u registers, %s keys\n\n",
               found->client_name, found->messages, seconds, found->registers,
               hex ? "hex (rXX)" : "decimal (dNNN)");
    }
    if (state.count > 1)
        printf("More than one: run an instance per pump, each with its own\n"
               "THERMIQ_ID so their values do not collide.\n");
    return 0;
}
