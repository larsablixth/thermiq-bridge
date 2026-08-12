#!/usr/bin/env python3
"""A throwaway MQTT 3.1.1 broker, just enough to exercise the client.

The bridge speaks MQTT to a real broker and nothing in the test suite touches
that code - so this stands in for one: it accepts a connection, answers the
handshake, and publishes a ThermIQ data message. Enough to prove the client
connects, subscribes and decodes, and that --discover reports the right node.

Usage: fake_broker.py <port> [--node NODE] [--hex] [--messages N]
"""

import json
import socket
import struct
import sys
import threading
import time


def remaining_length(data):
    """Decode MQTT's variable-length integer; returns (value, bytes_used)."""
    value = 0
    multiplier = 1
    for index, byte in enumerate(data[:4]):
        value += (byte & 0x7F) * multiplier
        if not byte & 0x80:
            return value, index + 1
        multiplier *= 128
    return None, 0


def encode_remaining_length(length):
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length:
            byte |= 0x80
        out.append(byte)
        if not length:
            return bytes(out)


def publish(topic, payload):
    body = struct.pack(">H", len(topic)) + topic.encode() + payload.encode()
    return bytes([0x30]) + encode_remaining_length(len(body)) + body


def read_packet(conn):
    header = conn.recv(1)
    if not header:
        return None, None
    length_bytes = b""
    while True:
        byte = conn.recv(1)
        if not byte:
            return None, None
        length_bytes += byte
        if not byte[0] & 0x80:
            break
    length, _ = remaining_length(length_bytes)
    body = b""
    while len(body) < length:
        chunk = conn.recv(length - len(body))
        if not chunk:
            break
        body += chunk
    return header[0], body


def serve(port, node, hexformat, messages):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    print(f"fake broker on 127.0.0.1:{port}", flush=True)

    conn, _ = listener.accept()
    subscribed = False
    sent = 0
    deadline = time.time() + 20

    conn.settimeout(0.5)
    while time.time() < deadline and sent < messages:
        try:
            packet_type, body = read_packet(conn)
        except socket.timeout:
            packet_type, body = 0, b""
        if packet_type is None:
            break
        kind = packet_type & 0xF0
        if kind == 0x10:  # CONNECT
            conn.sendall(bytes([0x20, 0x02, 0x00, 0x00]))  # CONNACK, accepted
        elif kind == 0x80:  # SUBSCRIBE
            packet_id = body[:2]
            conn.sendall(bytes([0x90, 0x03]) + packet_id + bytes([0x00]))
            subscribed = True
        elif kind == 0xC0:  # PINGREQ
            conn.sendall(bytes([0xD0, 0x00]))

        if subscribed and sent < messages:
            # Something from another device, to prove it is filtered out.
            conn.sendall(publish("shellies/kitchen/status", '{"ison": true}'))
            keys = (
                {"r00": -3, "r01": 21, "r02": 4}
                if hexformat
                else {"d000": -3, "d001": 21, "d002": 4}
            )
            conn.sendall(
                publish(
                    f"{node}/data",
                    json.dumps({"Client_Name": "ThermIQ_test", **keys, "time": "12:00"}),
                )
            )
            sent += 1
            time.sleep(0.2)

    conn.close()
    listener.close()


if __name__ == "__main__":
    args = sys.argv[1:]
    port = int(args[0])
    node = "ThermIQ/ThermIQ-mqtt"
    hexformat = "--hex" in args
    messages = 2
    if "--node" in args:
        node = args[args.index("--node") + 1]
    if "--messages" in args:
        messages = int(args[args.index("--messages") + 1])
    threading.Thread(target=serve, args=(port, node, hexformat, messages)).start()
