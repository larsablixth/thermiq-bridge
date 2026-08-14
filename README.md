# thermiq-bridge

Your Thermia or Danfoss heat pump on a web page, with an API and metrics, in a
container small enough to forget about. No Python, no interpreter, no runtime
dependencies at all - and Home Assistant entirely optional: run it instead of
Home Assistant, alongside it, or as an add-on inside it.

It speaks the same MQTT the [Home Assistant integration](https://github.com/larsablixth/thermiq_mqtt-ha)
speaks, decodes the same registers from the same register table, and draws the
same animated widget from the same template file - and it brings its own web
UI, so none of it depends on HACS cards.

Home Assistant is not required, but it is not in the way either: if you run
it, this installs as an add-on and appears in your sidebar - see
[below](#as-a-home-assistant-add-on).

```
docker run --rm -p 8080:8080 -e THERMIQ_DEMO=1 ghcr.io/larsablixth/thermiq-bridge
```

Open <http://localhost:8080/> and you get the whole thing running on canned
values, with no broker and no heat pump. That is the fastest way to see what
this is.

---

## What you get

**A web page.** The animated heat-pump schematic - pipe colours tracking real
temperatures, flow arrows that appear only when the medium moves, the
compressor turning - plus every register the pump exposes, grouped into the
same sections as the Home Assistant dashboard card. Setpoints, the operating
mode and the EVU block are live controls, not read-outs. The page refreshes
itself and morphs the widget in place, so the animations never restart.

**A JSON API**, for anything else you want to build:

| | |
|---|---|
| `GET /api/state` | every register's current value, plus connection status |
| `GET /api/registers` | the catalogue: names, units, bounds, select options |
| `GET /api/widget` | the rendered widget, as an HTML fragment |
| `POST /api/write` | `{"key": "indoor_requested_t", "value": 21}` |
| `GET /metrics` | Prometheus exposition |
| `GET /healthz` | 200 while data is arriving, 503 when it is not |

**Clean MQTT topics**, optionally. Set `THERMIQ_PUBLISH_PREFIX=thermiq/vp1` and
every decoded register is republished as it changes - `thermiq/vp1/outdoor_t` →
`-3` - so Node-RED, openHAB or a shell script can read the pump without knowing
anything about register numbers.

**History without a recorder.** Point Prometheus at `/metrics` and graph it in
Grafana. Every numeric register is exported as
`thermiq_register{key="outdoor_t",register="r00"}`.

## Installing

**In a hurry?** [Let an AI agent do it](AI_INSTALL.md) — it finds your pump on
the broker, picks the install route that suits the machine, and checks real
values are arriving before it says it is done. That page also spells out
exactly what access it needs.

### Docker

```bash
docker run -d --name thermiq -p 8080:8080 \
  -e THERMIQ_MQTT_HOST=192.168.1.10 \
  -e THERMIQ_NODE=ThermIQ/ThermIQ-mqtt \
  --restart unless-stopped \
  ghcr.io/larsablixth/thermiq-bridge:latest
```

Published for `amd64`, `arm64` and `armv7`. To build it yourself instead,
`docker build -t thermiq-bridge .`

There is a [docker-compose.yml](docker-compose.yml) with the same thing plus
the hardening flags (`read_only`, `cap_drop: ALL`, `no-new-privileges`) - all of
which it satisfies, because it needs no filesystem, no user and no privileges.

The image is `FROM scratch`: one static binary and nothing else. No shell, no
libc, no package manager, no CVE surface that is not this program's own code.
The `HEALTHCHECK` works anyway, because the binary probes itself
(`thermiq-bridge --healthcheck`).

### As a Home Assistant add-on

Running Home Assistant OS or Supervised? Add this repository under
*Settings → Add-ons → Add-on store → ⋮ → Repositories*:

```
https://github.com/larsablixth/thermiq-bridge
```

Then install **ThermIQ Bridge**, fill in the broker address on the
Configuration tab, and start it. The UI appears in the sidebar through ingress,
so there is no port to publish and no second address to remember.

There is no shell script behind that. Home Assistant writes your settings to
`/data/options.json` and the binary reads that file itself, which is why the
add-on is the same program in the same thin image rather than a wrapper around
one. Environment variables still override the options, so an add-on install
stays debuggable from a terminal.

The add-on is the same bridge, not a Home Assistant integration: it does not
create entities in Home Assistant. If that is what you want, install
[the integration](https://github.com/larsablixth/thermiq_mqtt-ha) instead - or
both, since they read the same MQTT and only this one writes.

### A single binary, no Docker

Static binaries for `amd64`, `arm64` and `armv7` are attached to every
[release](https://github.com/larsablixth/thermiq-bridge/releases). Download
one, `chmod +x`, run it - there is nothing to install alongside it.

### From source

You need a C compiler. That is the whole list.

```bash
make
./build/thermiq-bridge
```

## Configuring

Everything is an environment variable, validated at startup. A process that
starts holds a configuration that cannot fail later.

| Variable | Default | What it does |
|---|---|---|
| `THERMIQ_MQTT_HOST` | *required* | Broker address. Not required with `THERMIQ_DEMO=1`. |
| `THERMIQ_MQTT_PORT` | `1883` | Broker port. |
| `THERMIQ_MQTT_USERNAME` | *(none)* | Set both this and the password if your broker requires a login - Mosquitto does by default under Home Assistant. |
| `THERMIQ_MQTT_PASSWORD` | *(none)* | See above. Omit both for an anonymous broker. |
| `THERMIQ_MQTT_CLIENT_ID` | `thermiq-bridge-<id>` | Worth setting only if another bridge on the same broker would otherwise share it: two clients with one id disconnect each other in a loop. |
| `THERMIQ_NODE` | `ThermIQ/ThermIQ-mqtt` | Topic prefix, **without** `/data`. The single most common thing to get wrong. |
| `THERMIQ_ID` | `vp1` | Identifies this pump. Lowercase letters, digits, underscores. |
| `THERMIQ_LANGUAGE` | `en` | Register names: `en`, `se`, `fi`, `no`, `de`. |
| `THERMIQ_HEXFORMAT` | `false` | Set only for the old 1.xx ThermIQ-MQTT firmware. See below. |
| `THERMIQ_READ_ONLY` | `false` | Refuse every write. Useful for a display-only instance. |
| `THERMIQ_DEBUG_WRITES` | `false` | Divert writes to `<node>/dbg_write` and `/dbg_set`, so the pump is never touched. |
| `THERMIQ_PUBLISH_PREFIX` | *(none)* | Republish decoded values under this prefix. |
| `THERMIQ_HTTP_HOST` | `0.0.0.0` | |
| `THERMIQ_HTTP_PORT` | `8080` | |
| `THERMIQ_AVAILABILITY_TIMEOUT` | `120` | Seconds of silence before values are shown as stale. |
| `THERMIQ_DEMO` | `false` | Serve canned values; do not connect to a broker. |
| `THERMIQ_POOL_CIRCUIT` | `false` | The pump heats a second circuit (a pool) through a heat exchanger. Draws the widget's pool branch while that circuit is actually being heated. Leave off without the expansion card - the pump reports a curve-2 target either way, so this cannot be detected. |
| `THERMIQ_DISCOVER_SECONDS` | `45` | How long `--discover` listens. Only used by that mode. |
| `THERMIQ_LOG_LEVEL` | `info` | `debug`, `info`, `warn`, `error`. |

### Finding your node name

`THERMIQ_NODE` is the one setting nobody can look up and nearly everybody gets
wrong - and a wrong one looks exactly like broken hardware, because the bridge
connects, subscribes, and simply never hears anything. So it finds it for you:

```bash
docker run --rm -e THERMIQ_MQTT_HOST=192.168.1.10 \
  ghcr.io/larsablixth/thermiq-bridge --discover
```

```
Found 1 heat pump:

  THERMIQ_NODE=ThermIQ/ThermIQ-mqtt
  THERMIQ_HEXFORMAT=false
    device ThermIQ_abc123, 2 messages in 45s, 61 registers, decimal (dNNN) keys
```

It listens to the whole broker for 45 seconds, publishes nothing, and prints
the two settings that cannot be guessed. Exit codes: `0` found a pump, `1`
reached the broker but saw none, `2` could not reach the broker.

### Decimal or hex?

`--discover` answers this too, from the register keys the pump actually sends:
`d000`-style means current firmware and `THERMIQ_HEXFORMAT=false`; `r00`-style
means the old 1.xx firmware and `true`. Reads work either way - the setting
only controls the key format used when *writing*, which is why it has to
match. If controls appear to do nothing while the sensors are fine, this is
the first thing to check.

## How it stays honest

The failure mode for a rewrite like this is quiet divergence: the container
says 21.4 °C and Home Assistant says something else, and nobody notices for a
season. So nothing here is retyped from the integration - it is generated from
it, and the agreement is tested rather than asserted.

**The register table is generated.** `codegen/gen_registers.py` reads
`vendor/thermiq_regs.py` - the integration's own file, the same 140 registers,
units, bounds, bitmasks and five languages - and emits `src/registers_gen.c`.
CI regenerates and fails if the checked-in file differs.

**The two files it generates from are vendored, not rewritten.** They live
under `vendor/` with the commit they came from, and who wrote them, recorded
in [vendor/SOURCE.md](vendor/SOURCE.md). Living in a separate repository means
those copies can fall behind, which would be the quiet kind of wrong - so a
weekly CI job fetches both from the integration and fails if they have
diverged. Drift becomes a red build rather than a surprise in February.

**The widget is generated too.** `heatpump_widget.j2` is a Jinja2 template, and
C has no Jinja2. Rather than reimplement the drawing, `codegen/gen_widget.py`
compiles the template to C at build time: static text becomes `memcpy`,
expressions become arithmetic, entity ids resolve to array indices. Nothing is
parsed at runtime.

That is only safe if it is verified, so it is: `tests/test_widget.c` renders
nine state vectors - demo, heating, hot water, pool, alarm, comms lost,
unavailable, all-unknown, and every clamp edge - and compares them **byte for
byte** against
the same states rendered through real Jinja2. The build fails on a single byte
of difference. The live server's `/api/widget` output is identical to the
Python render.

**Decoding and writing are ported case by case.** Every check in
`tests/test_state.c` mirrors one in the integration's `tests/test_heatpump.py`,
including the exact wire payloads: `{"d050": 21}` for a setpoint, `{"EVU": 1}`
for the block, `{"d055": 65533}` for -3 in 16-bit two's complement. The
integer/float distinction survives decoding, so a combined register reads
`21.0` here exactly as it does in Home Assistant.

**Writes are validated before they leave.** The pump does not range-check what
it is told. A value outside the register's bounds, a non-boolean for a switch,
a write to a read-only register, or any write before the pump has said anything
at all is refused with a reason - the same conditions the integration refuses
under.

## How it is built

One process, one thread, one `poll()` loop. The MQTT client and the HTTP server
are state machines serviced from the same loop, so pump state is never read
while it is being written: there are no threads, and therefore nothing to lock
and no race to get wrong.

Nothing is allocated after startup. Every buffer is fixed and bounded - the
register table, the connection slots, the render buffer - so there is no
fragmentation, no allocation failure path, and no growth over months of
uptime. Writing past the end of a buffer sets a flag and truncates rather than
overruns.

The MQTT client reconnects by itself with backoff, and keeps its own keepalive,
because a bridge that stops when the broker restarts is not much use.

Measured on the demo instance: **193 kB** static binary, **2.1 MB** resident.

## Limitations

- **No TLS.** The broker this is built for is a Mosquitto on the same LAN. A
  TLS stack would be, by a wide margin, the largest thing in the binary. Put a
  tunnel in front of it if your broker is remote.
- **Writes come in over HTTP, not MQTT.** The bridge republishes readings
  outward, but does not subscribe to a command topic. Use `POST /api/write`.
- **No stored history.** Use `/metrics` and Prometheus; this keeps only the
  current value of each register.
- **Publishes at QoS 1**, where the integration uses QoS 2. Every write is an
  absolute setpoint, so a duplicate delivery is indistinguishable from the
  original.
- **No authentication.** Put it on a trusted network, or behind a reverse proxy
  that authenticates. `THERMIQ_READ_ONLY=1` at least makes it harmless.
- **Registers the table does not describe are ignored.** The integration stores
  them; nothing reads them.

## Developing

```bash
make            # build
make test       # widget parity, decode/encode parity, HTTP framing, config
make generate   # after changing the register table, the template, or web/
make run-demo   # serve canned values on :8080
```

`make generate` needs Python and Jinja2; building and testing the C does not,
because the generated sources are checked in. `make check-generated` is what CI
runs to prove they are current.

Cross-compiling for a Raspberry Pi, with no cross toolchain to install:

```bash
make CC="zig cc" TARGET=aarch64-linux-musl LDFLAGS="-static -Wl,--gc-sections"
```

Use `arm-linux-musleabihf` for an older 32-bit Pi. musl rather than glibc on
purpose: a statically linked glibc still wants its NSS shared libraries at
runtime to resolve a hostname, which is the one thing a single-file binary
must not need. CI builds all three and attaches them to the run, so you can
also just download one.

## Layout

```
src/            the bridge: config, mqtt, http, json, state, registers, widget
src/*_gen.c     generated - do not edit, run make generate
codegen/        the generators, and the Jinja2 reference renderer
vendor/         the integration's register table and widget template
web/index.html  the UI, embedded into the binary at build time
tests/          parity tests against Jinja2 and against the Python integration
addon/          Home Assistant add-on manifest (pulls the published image)
```
