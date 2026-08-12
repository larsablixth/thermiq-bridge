# Home Assistant add-on image.
#
# The Supervisor passes BUILD_FROM as the architecture's base image and builds
# this with the repository root as context, so the sources are one level up.
#
# The binary is compiled in a throwaway Alpine stage and copied into the HA
# base; the base contributes only its own init and labels, because the program
# needs no shell, no bashio and no wrapper - it reads /data/options.json
# itself.
ARG BUILD_FROM

FROM alpine:3.22 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make LDFLAGS="-static -Wl,--gc-sections" \
    && strip build/thermiq-bridge

FROM ${BUILD_FROM}
COPY --from=build /src/build/thermiq-bridge /usr/bin/thermiq-bridge

# Ingress proxies to this port inside the container; nothing is published.
ENV THERMIQ_HTTP_PORT=8080

LABEL io.hass.name="ThermIQ Bridge" \
      io.hass.description="Thermia/Danfoss heat pump: web UI, JSON API and metrics" \
      io.hass.type="addon" \
      io.hass.version="0.1.0"

CMD ["/usr/bin/thermiq-bridge"]
