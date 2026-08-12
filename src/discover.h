/* Broker discovery: find the pump's MQTT node name and register format. */
#ifndef THERMIQ_DISCOVER_H
#define THERMIQ_DISCOVER_H

#include "config.h"

/* Listens to the whole broker for a while and prints what to configure.
 * Returns 0 when a pump was found, 1 when none was, 2 when the broker could
 * not be reached. Read-only: it never publishes. */
int discover_run(const struct config *config);

#endif /* THERMIQ_DISCOVER_H */
