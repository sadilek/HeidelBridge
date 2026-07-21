# CLAUDE.md

Project-specific notes for this fork of HeidelBridge (ESP32 firmware for Heidelberg wallboxes).

## Fork- und Branch-Strategie

- `origin` = `sadilek/HeidelBridge` (dieser Fork). `upstream` = `BorisBrock/Heidelbridge` (Original).
- `main` wird als **sauberer Spiegel von `upstream/main`** gehalten — keine eigenen Commits, nur Fast-Forward via `git fetch upstream && git merge --ff-only upstream/main`.
- **`kupa5`** ist der Arbeits-Branch mit allen eigenen Anpassungen. Upstream wird per **Merge** (nicht Rebase) eingezogen, da `kupa5` gepusht/geteilt ist (kein force-push).
- Eigene Anpassungen möglichst **additiv** halten (neue Dateien statt Upstream-Dateien editieren) → minimiert Merge-Konflikte.

## Kupa5-Hardware-Anpassungen

- **`src/Boards/Kupa5/BoardKupa5.{h,cpp}`**: RS485-Pins RX=GPIO16, TX=GPIO17, RTS=-1 (Auto-Mode). Registriert in `src/Boards/BoardFactory.cpp`.
- Standard-Board ist auf **`kupa5`** gesetzt (`Settings.h` Default + `Settings.cpp` `getString`-Fallback + UI-Default in `data/index.js`). Auswahl im Web-UI unter „Hardware" (`data/index.html`).
- `platformio.ini`: `[env:esp32]` nutzt `board = wemos_d1_mini32` (nicht das Upstream-`esp32dev`).
- 2. LED auf GPIO2 als WLAN-Feedback in `src/Components/WiFi/WifiConnection.cpp`.

## Web-Oberfläche (eingebettete Header)

- HTML/JS/CSS in `data/` werden als PROGMEM-Byte-Arrays nach `data/headers/*.h` kompiliert.
- Nach jeder Änderung an `data/*` **`./build-resources.sh`** ausführen (nutzt `xxd`), sonst wirkt die Änderung nicht.

## Bauen & Flashen

- Bauen: `pio run -e esp32` (echte Hardware) bzw. `pio run -e dummy` (ohne Wallbox).
- **USB-Upload (macOS)**: `pio run -e esp32 -t upload --upload-port /dev/cu.usbmodem101` — Port muss explizit angegeben werden, da `platformio.ini` das Linux-`/dev/ttyUSB*` nutzt.
- **OTA-Update**: `http://{IP}/update` im Browser, `.pio/build/esp32/firmware.bin` hochladen. OTA lässt NVS-Einstellungen (WLAN/MQTT/board_type) unangetastet.

## Push-Auth (GitHub)

- `origin` nutzt **SSH** (`git@github.com:sadilek/HeidelBridge.git`). Damit funktioniert `git push` unabhängig davon, welcher Account in `gh auth` aktiv ist — kein HTTPS-Token/osxkeychain-Problem.
