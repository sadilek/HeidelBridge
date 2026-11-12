#!/usr/bin/env python3
"""Regression test for HeidelBridge's charging-state handling.

Needs no car and no PV surplus: the bugs it covers are register bookkeeping,
which happens whether or not anything is plugged in. Safe to run at night.

Ground truth is read directly from wallbox register 261 through HeidelBridge's
Modbus TCP server (Daheimladen register 91). MQTT is deliberately NOT used for
verification, because the firmware only publishes the current limit when it is
within 6-16 A -- and 0 A is exactly the value these tests care about.

What it covers (firmware >= 5.0.0, where the current limit is the only
charging control; 0 A blocks charging):

  1. A requested limit is written to register 261, and 0 A blocks charging.
  2. A telemetry read of 0 A does not change what the next request writes.
  3. Requests are clamped to the 0 or 6-16 A the wallbox accepts, and the
     wallbox's own hardware maximum applies on top.
  4. Retained and malformed commands are ignored instead of changing the
     charging state.

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
        # The wallbox enforces its own maximum (a hardware setting), so a request
        # of 16 A may come back lower. Everything below is expressed against that
        # cap rather than against a hard-coded 16 A.
        command("charging_current_limit", 16)
        cap = read_limit_amps()
        probe = 8.0 if cap >= 8.0 else 6.0
        print(f"\nWallbox hardware maximum: {cap} A (probe current {probe} A)\n")

        print("1. Requested limits reach the register, 0 A blocks charging")
        command("charging_current_limit", probe)
        check("limit applied", probe, read_limit_amps())
        command("charging_current_limit", 0)
        check("0 A blocks charging", 0.0, read_limit_amps())

        print("\n2. Telemetry reads of 0 A do not leak into the next request")
        print("       waiting 12 s so telemetry definitely observes 0 A ...")
        time.sleep(12)
        command("charging_current_limit", probe)
        check("next request writes its own value", probe, read_limit_amps())

        print("\n3. Clamping of out-of-range requests")
        command("charging_current_limit", 3)
        check("sub-minimum request becomes 0 A (blocked)", 0.0, read_limit_amps())
        command("charging_current_limit", 40)
        check("over-maximum request is capped", cap, read_limit_amps())

        print("\n4. Retained and malformed commands are ignored")
        command("charging_current_limit", 0)
        command("charging_current_limit", probe, retain=True)
        clear_retained("charging_current_limit")
        check("retained command ignored", 0.0, read_limit_amps())
        command("charging_current_limit", "abc")
        check("malformed command ignored", 0.0, read_limit_amps())

    finally:
        print("\nRestoring: charging blocked (0 A)")
        command("charging_current_limit", 0)
        if AUTOMATION:
            call_service("automation/turn_on", {"entity_id": AUTOMATION})
            print(f"Re-enabled {AUTOMATION}")

    print("\n" + ("ALL CHECKS PASSED" if not failures else "FAILED: " + ", ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
