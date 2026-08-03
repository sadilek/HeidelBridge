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

## Einsatz: Steuerung per Home Assistant (kein evcc mehr)

Seit 2026-08-03 wird die Wallbox **ausschließlich über MQTT von Home Assistant** gesteuert
(PV-Überschuss + Schnellladen). Die Daheimladen-Modbus-TCP-Emulation läuft weiter, wird aber
von nichts mehr benutzt. Steuerlogik: `~/dev/homeassistant/ha_config/wallbox.yaml`.

Hintergrund: evcc prüft seit 2026-07 in `charger/daheimladen.go` per `checkStation()` einen
22-Register-Block ab Reg. 32 und lehnt zusätzlich die Seriennummer `heidelbridge` ab
(→ `sponsorship required`). Upstream 4.0.0 meldet diese Seriennummer bewusst. Diese Firmware
beantwortet nur exakte Startadressen, nicht den Block — deshalb war evcc nicht mehr nutzbar.

## Bekannte Firmware-Fallstricke (hier gefixt, upstream noch offen)

Kandidaten für einen PR an Boris — reine Bugs, unabhängig von der evcc-Diskussion:

- **`default_entity_id` ohne Domain-Präfix** (`MQTTManager.cpp`): HA verlangt `domain.object_id`.
  Ohne den Punkt scheitert `cv.entity_id`, alle 15 Entities landen als `unnamed_device`, und
  „Recreate entity IDs" verweigert die Arbeit (HA hält die IDs für explizit gesetzt).
  Das ist auch die Ursache von Upstream-Issue #58.
- **Discovery muss `retain=true` sein** (`MQTTManager.cpp`): Discovery wird nur beim MQTT-Connect
  gesendet. Ohne Retain findet ein neu startendes HA nichts unter `homeassistant/+/+/config`;
  alles mit `command_topic` (beide Switches + Number) bleibt `unavailable`, bis der ESP zufällig
  reconnected. Sensoren überleben, weil sie laufend State publishen.
- **MQTT-Payloads sind nicht nullterminiert** (`MQTTManager.cpp`): `String(payload, len)` reichte
  nicht — `"ON"` wurde nie erkannt, jedes Kommando fiel in den Off-Zweig. `OFF` funktionierte nur
  scheinbar, weil `false` das gewünschte Ergebnis war. Jetzt wird explizit in einen
  nullterminierten Puffer kopiert.
- **`SetChargingEnabled` war auf einen Zustandswechsel gegated** (`HeidelbergWallbox.cpp`):
  `mChargingEnabled` liegt nur im RAM und steht nach jedem Reboot auf `true`, während Register 261
  noch 0 hält → der Zweig, der beides synchronisieren würde, feuert nie. Schreibt jetzt bei **jedem**
  Aufruf; damit ist die Operation idempotent und selbstheilend (die HA-Automation reasserted alle 30 s).

**Wichtig:** `unique_id`s nicht mehr ändern — HA legt sonst eine neue Entity an und die Historie geht
verloren.

## Debugging ohne serielle Konsole

- `[env:esp32]` baut mit `-D LOGGING_LEVEL_ERROR`, d. h. `Logger::Info/Debug/Trace` sind wegkompiliert.
- Stattdessen echot die Firmware jedes empfangene Kommando nach **`{DeviceName}/internal/last_command`**
  (in HA: Einstellungen → Geräte → MQTT → „Auf ein Thema hören").
- REST-API des Geräts: `GET /api/version`, `GET /api/settings_read`, `POST /api/settings_write`,
  `POST /api/reboot`. **Kein** State/Control darüber — nur Konfiguration.
  `build_date` aus `/api/version` ist unzuverlässig: `__DATE__` stammt aus der jeweiligen
  Objektdatei, die bei inkrementellen Builds nicht neu übersetzt wird.

## Bauen & Flashen

- Bauen: `pio run -e esp32` (echte Hardware) bzw. `pio run -e dummy` (ohne Wallbox).
- **USB-Upload (macOS)**: `pio run -e esp32 -t upload --upload-port /dev/cu.usbmodem101` — Port muss explizit angegeben werden, da `platformio.ini` das Linux-`/dev/ttyUSB*` nutzt.
- **OTA-Update**: `http://{IP}/update` im Browser, `.pio/build/esp32/firmware.bin` hochladen. OTA lässt NVS-Einstellungen (WLAN/MQTT/board_type) unangetastet.
- **PlatformIO steckt in einem pipx-Venv** (`~/.local/pipx/venvs/platformio`). Fehlt `esptool` ein Modul
  (z. B. `intelhex` → `firmware.bin` schlägt fehl, obwohl `firmware.elf` schon gebaut ist), gehört es
  dorthin: `pipx inject platformio intelhex`. Nicht ins Homebrew-Python (PEP 668) und nicht ins
  `~/.platformio/penv` — esptool läuft unter dem pipx-Interpreter.

## Push-Auth (GitHub)

- `origin` nutzt **SSH** (`git@github.com:sadilek/HeidelBridge.git`). Damit funktioniert `git push` unabhängig davon, welcher Account in `gh auth` aktiv ist — kein HTTPS-Token/osxkeychain-Problem.
