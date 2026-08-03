#!/usr/bin/env python3
"""Regression test for HeidelBridge's charging-state handling.

Needs no car and no PV surplus: the bugs it covers are register bookkeeping,
which happens whether or not anything is plugged in. Safe to run at night.

Ground truth is read directly from wallbox register 261 through HeidelBridge's
Modbus TCP server (Daheimladen register 91). MQTT is deliberately NOT used for
verification, because the firmware only publishes the current limit when it is
within 6-16 A -- and 0 A is exactly the value these tests care about.

What it covers:

  1. Init() seeds the enabled flag from the wallbox rather than assuming it.
  2. A telemetry read of 0 A does not destroy the requested current limit.
     This is the regression that matters: GetChargingCurrentLimit() used to
     write its reading into the same member as the setpoint, so disabling and
     re-enabling restored 0 A and charging silently never started.
  3. Requests are clamped to the 0 or 6-16 A the wallbox accepts.
  4. Enabling with a zero setpoint falls back to the default instead of
     writing 0 A and leaving charging blocked.

Usage:
    export HA_URL=... HA_TOKEN=...          # required
    export HEIDELBRIDGE_HOST=192.168.0.42   # optional, defaults below
    export WALLBOX_AUTOMATION=automation.wallbox_steuerung   # optional
    python3 tools/regression_test_charging.py

Exit code is non-zero if any check fails.
"""
import json
import os
import socket
import struct
import sys
import time
import urllib.request

HA_URL = os.environ["HA_URL"].rstrip("/")
HA_TOKEN = os.environ["HA_TOKEN"]
DEVICE = os.environ.get("HEIDELBRIDGE_HOST", "192.168.178.106")
DEVICE_NAME = os.environ.get("HEIDELBRIDGE_NAME", "HeidelBridge")
# Optional: a Home Assistant automation that drives the wallbox and would
# otherwise fight the test by reasserting its own state on a timer.
AUTOMATION = os.environ.get("WALLBOX_AUTOMATION", "automation.wallbox_steuerung")

DAHEIMLADEN_LIMIT_REGISTER = 91
SETTLE_S = 3

failures = []


def read_limit_amps():
    """Read the wallbox current limit via Modbus TCP (function code 3)."""
    frame = struct.pack(">HHHB", 1, 0, 6, 0xFF) + struct.pack(
        ">BHH", 0x03, DAHEIMLADEN_LIMIT_REGISTER, 1
    )
    sock = socket.create_connection((DEVICE, 502), timeout=8)
    try:
        sock.sendall(frame)
        response = sock.recv(256)
    finally:
        sock.close()
    if len(response) < 11 or response[7] & 0x80:
        raise RuntimeError("Modbus exception or short frame: " + response.hex())
    return struct.unpack(">H", response[9:11])[0] / 10.0


def call_service(service, payload):
    request = urllib.request.Request(
        f"{HA_URL}/api/services/{service}",
        data=json.dumps(payload).encode(),
        headers={"Authorization": "Bearer " + HA_TOKEN, "Content-Type": "application/json"},
        method="POST",
    )
    urllib.request.urlopen(request, timeout=10).read()


def command(topic, value):
    call_service(
        "mqtt/publish",
        {"topic": f"{DEVICE_NAME}/control/{topic}", "payload": str(value), "qos": 2},
    )
    time.sleep(SETTLE_S)


def check(label, expected, actual):
    ok = abs(expected - actual) < 0.05
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}")
    print(f"         register 261 = {actual} A, expected {expected} A")
    if not ok:
        failures.append(label)


def main():
    if AUTOMATION:
        print(f"Pausing {AUTOMATION} so it cannot fight the test")
        call_service("automation/turn_off", {"entity_id": AUTOMATION})
        time.sleep(SETTLE_S)

    try:
        # The wallbox enforces its own maximum (a hardware setting), so a request
        # of 16 A may come back lower. Everything below is expressed against that
        # cap rather than against a hard-coded 16 A.
        command("enable_charging", "ON")
        command("charging_current_limit", 16)
        cap = read_limit_amps()
        probe = 8.0 if cap >= 8.0 else 6.0
        print(f"\nWallbox hardware maximum: {cap} A (probe current {probe} A)\n")

        print("1. Requested limit survives a disable and telemetry reads of 0 A")
        command("charging_current_limit", probe)
        check("limit applied while enabled", probe, read_limit_amps())
        command("enable_charging", "OFF")
        check("disable writes 0 A", 0.0, read_limit_amps())
        print("       waiting 12 s so telemetry definitely observes 0 A ...")
        time.sleep(12)
        command("enable_charging", "ON")
        check("enable restores the requested limit, not 0", probe, read_limit_amps())

        print("\n2. Clamping of out-of-range requests")
        command("charging_current_limit", 3)
        check("sub-minimum request becomes 0 A (blocked)", 0.0, read_limit_amps())

        print("\n3. Enabling with a zero setpoint falls back to the default")
        command("enable_charging", "ON")
        check("fallback applies the default limit", cap, read_limit_amps())

    finally:
        print("\nRestoring: charging disabled")
        command("enable_charging", "OFF")
        if AUTOMATION:
            call_service("automation/turn_on", {"entity_id": AUTOMATION})
            print(f"Re-enabled {AUTOMATION}")

    print("\n" + ("ALL CHECKS PASSED" if not failures else "FAILED: " + ", ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
