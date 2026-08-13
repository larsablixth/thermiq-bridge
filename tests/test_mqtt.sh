#!/bin/sh
# End-to-end over real MQTT, against tests/fake_broker.py.
#
# The C unit tests feed payloads straight to the decoder, which leaves the
# whole MQTT client - the handshake, the packet framing, the subscription -
# exercised by nothing at all. This drives the real binary against a real
# socket speaking real MQTT, and checks both what it decodes and what
# --discover reports.
set -eu

BIN=${1:-build/thermiq-bridge}
PORT=${PORT:-18860}
HTTP_PORT=${HTTP_PORT:-18861}
here=$(dirname "$0")
failures=0

cleanup() {
    [ -n "${BRIDGE_PID:-}" ] && kill "$BRIDGE_PID" 2>/dev/null || true
    [ -n "${BROKER_PID:-}" ] && kill "$BROKER_PID" 2>/dev/null || true
}
trap cleanup EXIT

check() {
    if [ "$2" = "$3" ]; then
        printf 'ok   %s\n' "$1"
    else
        printf 'FAIL %s: expected "%s", got "%s"\n' "$1" "$3" "$2" >&2
        failures=$((failures + 1))
    fi
}

# ---- discovery finds the pump, and says which firmware it speaks ----------

python3 "$here/fake_broker.py" "$PORT" --node "House/HeatPump" --messages 4 \
    > /tmp/thermiq-broker.log 2>&1 &
BROKER_PID=$!
sleep 1

out=$(THERMIQ_MQTT_HOST=127.0.0.1 THERMIQ_MQTT_PORT="$PORT" \
      THERMIQ_DISCOVER_SECONDS=3 "$BIN" --discover 2>/dev/null)
node=$(printf '%s' "$out" | sed -n 's/^ *THERMIQ_NODE=//p')
hex=$(printf '%s' "$out" | sed -n 's/^ *THERMIQ_HEXFORMAT=//p')
check "discover finds the node" "$node" "House/HeatPump"
check "discover reads the register format" "$hex" "false"
wait "$BROKER_PID" 2>/dev/null || true

# An old-firmware pump publishes hex keys and must be reported as such.
python3 "$here/fake_broker.py" "$PORT" --node "attic/vp" --hex --messages 4 \
    > /tmp/thermiq-broker-hex.log 2>&1 &
BROKER_PID=$!
sleep 1
out=$(THERMIQ_MQTT_HOST=127.0.0.1 THERMIQ_MQTT_PORT="$PORT" \
      THERMIQ_DISCOVER_SECONDS=3 "$BIN" --discover 2>/dev/null)
check "discover reads a hex-firmware pump" \
    "$(printf '%s' "$out" | sed -n 's/^ *THERMIQ_HEXFORMAT=//p')" "true"
check "discover reads its node" \
    "$(printf '%s' "$out" | sed -n 's/^ *THERMIQ_NODE=//p')" "attic/vp"
wait "$BROKER_PID" 2>/dev/null || true

# ---- the bridge decodes what arrives -------------------------------------

python3 "$here/fake_broker.py" "$PORT" --node "House/HeatPump" --messages 3 \
    > /tmp/thermiq-broker2.log 2>&1 &
BROKER_PID=$!
sleep 1

THERMIQ_MQTT_HOST=127.0.0.1 THERMIQ_MQTT_PORT="$PORT" \
    THERMIQ_NODE=House/HeatPump THERMIQ_HTTP_PORT="$HTTP_PORT" \
    "$BIN" > /tmp/thermiq-bridge.log 2>&1 &
BRIDGE_PID=$!

# Wait for it to be serving before asking it anything.
i=0
while [ "$i" -lt 30 ]; do
    curl -fsS "http://127.0.0.1:$HTTP_PORT/healthz" > /dev/null 2>&1 && break
    i=$((i + 1))
    sleep 1
done

state=$(curl -fsS "http://127.0.0.1:$HTTP_PORT/api/state")
value() {
    printf '%s' "$state" | python3 -c "import json,sys;print(json.load(sys.stdin)['values']['$1']['state'])"
}

check "an integer register decodes" "$(value outdoor_t)" "-3"
# 21 and 4 arrive in separate registers; Home Assistant shows 21.4, so do we.
check "split decimals recombine" "$(value indoor_t)" "21.4"
check "the pump is reported available" \
    "$(printf '%s' "$state" | python3 -c "import json,sys;print(json.load(sys.stdin)['status']['available'])")" \
    "True"

# ---- HTTP behaviour that only shows up over a real socket -----------------

# HEAD is routed as GET, so the headers must be the GET's - and the body must
# not be there. It used to send the whole page.
head_out=$(curl -fsS -I "http://127.0.0.1:$HTTP_PORT/api/state")
head_len=$(printf '%s' "$head_out" | tr -d '\r' | sed -n 's/^[Cc]ontent-[Ll]ength: //p')
get_len=$(curl -fsS "http://127.0.0.1:$HTTP_PORT/api/state" | wc -c | tr -d ' ')
check "HEAD reports the length the GET would send" "$head_len" "$get_len"
# -I prints headers only whatever the server sends, so count the bytes on the
# wire instead: headers, the blank line, and nothing after it.
raw=$(curl -fsS -o /dev/null -w '%{size_download}' --head \
      "http://127.0.0.1:$HTTP_PORT/api/state")
check "HEAD sends no body" "$raw" "0"

# The error body is JSON built through the escaper, not by hand. No -f here:
# these are the failure responses, and -f makes curl exit non-zero on them.
err=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/nope")
check "an unknown path is 404" "$err" "404"
parsed=$(curl -sS "http://127.0.0.1:$HTTP_PORT/nope" \
         | python3 -c "import json,sys;print(json.load(sys.stdin)['error'])")
check "the error body is parseable JSON" "$parsed" "no such path"

if [ "$failures" -ne 0 ]; then
    echo "$failures mqtt check(s) failed" >&2
    exit 1
fi
echo "all mqtt end-to-end checks passed"
