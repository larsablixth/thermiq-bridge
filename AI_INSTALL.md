# Install thermiq-bridge with an AI agent

You should not have to read an install guide to get a heat pump onto a screen.
Hand this file to an AI agent instead — it does the reading, you answer at most
two questions.

Works with any agent that can run shell commands: Claude Code, Codex CLI,
Gemini CLI, Copilot agent mode.

---

## The 30-second version

Point your agent at this file and tell it where your MQTT broker is:

> Install thermiq-bridge on this machine.
> Follow this runbook exactly: <https://raw.githubusercontent.com/larsablixth/thermiq-bridge/main/AI_INSTALL.md>
> My MQTT broker is at 192.168.1.10.

That is the whole thing. The agent will find your heat pump on the broker by
itself, work out which firmware it speaks, install the bridge whichever way
suits this machine, and check that real values are arriving before it tells you
it is done.

Don't know the broker's address? Say so — the runbook covers finding it.

Prefer to do it by hand? The [README](README.md) is short.

---

## What access the agent needs

| # | Access | Required? | What for |
|---|--------|-----------|----------|
| 1 | **A shell on the machine that will run the bridge** | Yes | Installing and starting it |
| 2 | **Network reach to your MQTT broker** (usually port 1883) | Yes | Finding the pump, and reading it |
| 3 | **Outbound access to `ghcr.io` or `github.com`** | Yes | Downloading the image or the binary |
| 4 | **Docker, or permission to write a systemd unit** | One of them | How it keeps running after a reboot |
| 5 | **MQTT username and password** | Only if your broker needs them | Connecting |
| 6 | **A Home Assistant admin token** | Only for the add-on route | Installing it as an add-on |

### What it does not need

- **Root**, unless installing a system-wide systemd service — and it should
  tell you when it wants it, and why.
- **Access to your heat pump.** It never talks to the pump directly. It reads
  MQTT, which the pump publishes to anyway.
- **Any cloud account**, for ThermIQ or anyone else.
- **Home Assistant**, unless you want the add-on.

### The one real risk

Once configured, the bridge can **write setpoints to your heat pump**. Nothing
in this install writes anything — discovery and verification are read-only —
but the thing you end up with has a control panel in it.

If you would rather it could not write at all, tell the agent to set
`THERMIQ_READ_ONLY=1`. Every write is then refused, and you still get the whole
UI, the API and the metrics. You can turn it off later by changing one
variable.

---

## The contract

**The agent will:** show you what it found before changing anything; tell you
which install method it chose and why; verify against real values from your
pump rather than declaring success; and tell you how to undo it.

**The agent will never:** write to your heat pump during installation; invent
an MQTT node name that it has not seen on the broker; leave the bridge running
in a state it has not verified; or store your broker password anywhere except
where you told it to.

---
---

# Agent runbook

**Everything below is addressed to the AI agent.** Follow it in order. If a
step fails, stop and report — do not improvise around a failure.

## 0. Ground rules

1. **Never write to the heat pump during installation.** Discovery and every
   verification step here are read-only. The user's setpoints are not yours to
   experiment with.
2. **Never guess the MQTT node name.** Step 2 finds it. A guessed name produces
   a bridge that connects, subscribes, and silently reports nothing — which
   looks exactly like broken hardware and wastes an evening.
3. **Verify with values, not exit codes.** "The container started" is not
   success. A temperature that matches reality is.
4. **Keep secrets out of the shell history and out of your output.** If the
   broker needs a password, put it in the compose file or the systemd unit's
   environment file, not in a `docker run` line you echo back.
5. **One question at a time, in plain language.** The user chose this route to
   avoid reading documentation. Do not paste this runbook back at them.

## 1. Work out where you are

Establish, without asking:

```bash
uname -sm                          # OS and architecture
command -v docker && docker info --format '{{.ServerVersion}}' 2>/dev/null
command -v systemctl
cat /etc/os-release 2>/dev/null | head -2
```

Also check whether this machine is running Home Assistant:

```bash
ls /config/configuration.yaml 2>/dev/null || ls ~/.homeassistant 2>/dev/null
```

Pick the install route from what you find, and **tell the user which and why**:

| What you found | Route |
|---|---|
| Docker available | **A — container.** The default. Survives reboots, easy to update. |
| No Docker, systemd present | **B — binary + systemd unit.** Nothing to install but one file. |
| Home Assistant OS / Supervised | **C — add-on.** It lands in their sidebar. |
| Neither, or a one-off look | **D — just run it.** Foreground, no persistence. |

If both Docker and Home Assistant are present, ask which they would prefer;
the add-on is usually nicer for an HA user because of the sidebar.

## 2. Find the heat pump

You need two settings that cannot be guessed: the **node name** (the MQTT topic
prefix the pump publishes under) and whether its firmware speaks **decimal or
hex** register keys. The bridge finds both itself.

```bash
# amd64; use -aarch64 or -armv7 to match `uname -m`
curl -fsSL -o /tmp/thermiq-bridge \
  https://github.com/larsablixth/thermiq-bridge/releases/latest/download/thermiq-bridge-amd64
chmod +x /tmp/thermiq-bridge

THERMIQ_MQTT_HOST=192.168.1.10 /tmp/thermiq-bridge --discover
```

It listens to the whole broker for 45 seconds and prints what it found:

```
Found 1 heat pump:

  THERMIQ_NODE=ThermIQ/ThermIQ-mqtt
  THERMIQ_HEXFORMAT=false
    device ThermIQ_abc123, 2 messages in 45s, 61 registers, decimal (dNNN) keys
```

Use those two values verbatim. Exit codes: `0` found something, `1` reached the
broker but saw no pump, `2` could not reach the broker.

If you are using the Docker route you can run discovery through Docker instead,
and skip the download:

```bash
docker run --rm -e THERMIQ_MQTT_HOST=192.168.1.10 \
  ghcr.io/larsablixth/thermiq-bridge --discover
```

**If it reports no pump** (exit 1): the pump publishes every ~30 seconds, so
either it is not connected to this broker or it is off. Do not proceed with a
guess. Ask the user to check the pump, or to confirm the broker address.

**If it cannot reach the broker** (exit 2): check the address, and ask whether
the broker needs a username and password — pass them as
`THERMIQ_MQTT_USERNAME` and `THERMIQ_MQTT_PASSWORD` and retry.

**If the user does not know the broker address**, it is usually the machine
running Mosquitto or Home Assistant. `ip route | grep default` gives the
gateway; the broker is typically another host on that subnet. Home Assistant
users can find it under *Settings → Devices & Services → MQTT → Configure*.
Ask rather than scan the network.

**If it finds more than one pump**, ask which, or set up one bridge per pump
with a different `THERMIQ_ID` and HTTP port each.

## 3. Install

Whichever route, the same settings apply. Only `THERMIQ_MQTT_HOST` and
`THERMIQ_NODE` are needed; the rest have sane defaults, and the full list is in
the [README](README.md#configuring).

### Route A — container

```bash
docker run -d --name thermiq \
  -p 8080:8080 \
  -e THERMIQ_MQTT_HOST=192.168.1.10 \
  -e THERMIQ_NODE=ThermIQ/ThermIQ-mqtt \
  --restart unless-stopped \
  --read-only --cap-drop ALL --security-opt no-new-privileges \
  ghcr.io/larsablixth/thermiq-bridge:latest
```

The hardening flags are not decoration — the process needs no filesystem, no
user and no privileges, so it satisfies all of them. Keep them.

If the broker needs a password, write a `docker-compose.yml` instead so the
credential is in a file rather than in the process list. There is one in the
repository to start from.

### Route B — binary and systemd

```bash
sudo install -m 755 /tmp/thermiq-bridge /usr/local/bin/thermiq-bridge
sudo tee /etc/thermiq-bridge.env >/dev/null <<'EOF'
THERMIQ_MQTT_HOST=192.168.1.10
THERMIQ_NODE=ThermIQ/ThermIQ-mqtt
EOF
sudo chmod 600 /etc/thermiq-bridge.env   # it may hold a password
sudo tee /etc/systemd/system/thermiq-bridge.service >/dev/null <<'EOF'
[Unit]
Description=ThermIQ heat pump bridge
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/thermiq-bridge
EnvironmentFile=/etc/thermiq-bridge.env
DynamicUser=yes
Restart=always
RestartSec=5
# It needs no filesystem at all.
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes
CapabilityBoundingSet=
MemoryMax=64M

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now thermiq-bridge
```

### Route C — Home Assistant add-on

This one needs the user to click, and that is fine — it is three clicks.

1. *Settings → Add-ons → Add-on store → ⋮ → Repositories*
2. Add `https://github.com/larsablixth/thermiq-bridge`
3. Install **ThermIQ Bridge**, put the broker address and the node name from
   step 2 on the Configuration tab, and start it.

The UI then appears in the sidebar. Wait for the user to confirm before
verifying.

If you have a Home Assistant admin token and want to do it without clicks, the
Supervisor is reachable through `POST /api/hassio/store/repositories` and
`POST /api/hassio/addons/{slug}/install`. Verify each call's response rather
than assuming — and fall back to asking the user to click if anything answers
unexpectedly.

### Route D — just run it

```bash
THERMIQ_MQTT_HOST=192.168.1.10 THERMIQ_NODE=ThermIQ/ThermIQ-mqtt /tmp/thermiq-bridge
```

Tell the user plainly that this stops when they close the terminal, and offer
route A or B for something permanent.

## 4. Verify with real values

Not "it started". Real readings from their actual heat pump.

```bash
curl -fsS http://127.0.0.1:8080/healthz
```

`200` and `"available": true` means data is arriving. `503` means it is not —
wait a minute, since the pump publishes every ~30 seconds, then look again.

Then read something a person can sanity-check:

```bash
curl -fsS http://127.0.0.1:8080/api/state | python3 -c '
import json, sys
data = json.load(sys.stdin)
print("connected:", data["status"]["connected"], "available:", data["status"]["available"])
for key in ("outdoor_t", "indoor_t", "supplyline_t", "boiler_t", "compressor_on"):
    print(f"  {key:16}", data["values"][key]["state"])
'
```

Report those numbers to the user and **ask whether the outdoor temperature
looks right**. That is the one check that proves the whole chain — broker, node
name, decode — and it is a check only they can make.

- `unavailable` everywhere: no messages arriving. The node name is wrong, or
  the pump is off. Re-run `--discover`.
- Plausible numbers: done.
- Wildly wrong numbers: rare, but would suggest `THERMIQ_HEXFORMAT` is
  inverted. Re-run `--discover` and compare.

Finally, open the UI: `http://<host>:8080/`. Tell them the address.

## 5. Report

Short, and in their terms:

- where it is running and how it restarts
- the URL of the UI
- the node name and register format you detected
- one real reading, with its value
- whether writes are enabled, and how to turn them off (`THERMIQ_READ_ONLY=1`)
- how to undo the whole thing

No secrets. No wall of text.

---

## Updating

```bash
docker pull ghcr.io/larsablixth/thermiq-bridge:latest && docker restart thermiq
```

For the binary route, download the new release over the old one and
`systemctl restart thermiq-bridge`. For the add-on, Home Assistant offers the
update itself.

## Undoing it

| Route | |
|---|---|
| A | `docker rm -f thermiq` |
| B | `sudo systemctl disable --now thermiq-bridge`, then remove the unit, the env file and `/usr/local/bin/thermiq-bridge` |
| C | Uninstall the add-on in Home Assistant |

Nothing is written outside those. The bridge keeps no database and no state on
disk: stop it and it is gone.

## When it does not work

| Symptom | Cause | Fix |
|---|---|---|
| `--discover` exit 2 | Broker unreachable, or wants credentials | Check the address; add `THERMIQ_MQTT_USERNAME` / `THERMIQ_MQTT_PASSWORD` |
| `--discover` exit 1 | Reached the broker, no pump publishing | The pump is off or on another broker. Do not guess a node name |
| `/healthz` 503, everything `unavailable` | Wrong node name | Re-run `--discover` and compare with `THERMIQ_NODE` |
| Some registers `unknown` | Not present on this pump model | Normal |
| Controls refuse with "read-only mode" | `THERMIQ_READ_ONLY` is set | Remove it and restart |
| Controls refuse with "no data received yet" | No message from the pump yet | Wait a minute |
| Writes appear to do nothing | `THERMIQ_HEXFORMAT` inverted | Compare with what `--discover` reported |
| Container exits immediately | Bad configuration | `docker logs thermiq` — it names the variable and what was wrong with it |
| Add-on won't start | Same | Check the add-on log tab |
