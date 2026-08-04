#!/usr/bin/env bash
# OTA-flash a built firmware image to HeidelBridge over HTTP.
#
#   ./tools/ota_upload.sh                        # flashes .pio/build/esp32/firmware.bin
#   ./tools/ota_upload.sh path/to/firmware.bin
#   HEIDELBRIDGE_HOST=192.168.0.42 ./tools/ota_upload.sh
#
# Safe to run while a car is charging: the wallbox holds its current limit in
# register 261 across a reboot of this bridge, and the Heidelberg watchdog is
# disabled (Constants::HeidelbergWallbox::WatchdogTimeoutS = 0), so charging
# continues. Since 4.1 the bridge also seeds its state from that register on
# boot instead of assuming charging is enabled.
set -euo pipefail

HOST="${HEIDELBRIDGE_HOST:-heidelbridge}"
BIN="${1:-.pio/build/esp32/firmware.bin}"

if [ ! -f "$BIN" ]; then
    echo "No firmware image at $BIN -- run 'pio run -e esp32' first." >&2
    exit 1
fi

echo "Uploading $BIN ($(wc -c <"$BIN" | tr -d ' ') bytes) to http://$HOST/api/update"
curl -sS --max-time 180 -F "firmware=@$BIN" "http://$HOST/api/update" -w '\nHTTP %{http_code}\n'

echo "Waiting for the device to come back ..."
for _ in $(seq 1 30); do
    sleep 3
    if version=$(curl -sS --max-time 5 "http://$HOST/api/version" 2>/dev/null); then
        echo "Back online: $version"
        exit 0
    fi
done

echo "Device did not respond within 90 s -- check $HOST manually." >&2
exit 1
