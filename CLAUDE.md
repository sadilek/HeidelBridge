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
- **Ladezustand wurde erfunden statt gelesen** (`HeidelbergWallbox.cpp`, `.h`, `Constants.h`).
  Zwei unabhängige Ursachen:
  1. `Init()` schrieb 262/258/257, las aber **nie** Register 261 → `mChargingEnabled` startete auf dem
     Header-Default `true`, egal was die Wallbox tat. Liest und seedet jetzt.
  2. `GetChargingCurrentLimit()` (Telemetrie, läuft im Publish-Zyklus **und** über Modbus TCP) schrieb
     sein Messergebnis in dasselbe Member wie der Sollwert. Ein einziger 0-A-Read zerstörte den Sollwert.
  Sollwert und Messwert sind jetzt getrennt (`mRequested…` / `mObserved…`); `mPrevious…` entfällt.
  `SetChargingEnabled` schreibt bei jedem Aufruf. Clamping auf {0, 6..16 A} schließt außerdem UB beim
  `float`→`uint16_t`-Cast negativer Werte.

## Widerlegt — nicht nochmal „reparieren"

- **MQTT-Payload-Parsing ist in Ordnung.** Kurzzeitig verdächtigt, `String(payload, len)` würde die
  nicht nullterminierte Payload falsch behandeln und `"ON"` nie erkennen. Am 2026-08-03 mit einem
  Diagnose-Build gemessen: `raw='ON' len=2 match=enable_charging parsed='ON' enabled=1`. Der
  Konstruktor begrenzt korrekt, `setLen()` nullterminiert. Upstream-Code ist hier korrekt.
  (Einzige echte Unsauberkeit: `String::copy` macht `memmove(cstr, length + 1)`, liest also ein Byte
  über die Payload hinaus. `setLen()` überschreibt es — praktisch folgenlos.)
- **Erster Anlauf des Ladezustand-Fixes war selbst falsch.** „Register bei jedem `SetChargingEnabled`
  schreiben" allein reicht **nicht**: der Getter vergiftet `mChargingCurrentLimitA` weiterhin mit 0,
  und der idempotente Write schreibt diese 0 dann zuverlässig zurück. Zwei unabhängige adversariale
  Reviews (Codex, Fable) haben das gekillt. Ohne die Trennung von Soll- und Messwert ist der Fix wertlos.
- **Ungeklärt:** Beim ursprünglichen Debugging schaltete `turn_on` den Switch gar nicht um, obwohl
  der Code das tun müsste. Nicht reproduziert. Verdächtig bleibt, dass `gMqttTopic`, `TopicBuffer`
  und `PayloadBuffer` ohne Lock zwischen Publish-Timer-Task und MQTT-Callback-Task geteilt werden.
  Bei `match=none` im Diagnose-Echo genau dort suchen.

**Wichtig:** `unique_id`s nicht mehr ändern — HA legt sonst eine neue Entity an und die Historie geht
verloren.

## Hardware-Grenzen der Wallbox (gemessen 2026-08-03)

- Die Wallbox **kappt selbst bei 10 A** (Hardware-Einstellung, passend zur 1-phasigen
  Installation): Requests von 11/12/16 A landen alle als 10 A in Register 261, 6/8/9/10 A gehen
  unverändert durch. `InitialChargingCurrentLimitA = 16` wirkt hier also faktisch als 10 A.
- Nutzbares Band damit **6–10 A ≈ 1,38–2,30 kW** (1 Phase, 230 V).
- Konsequenz für den Fallback in `SetChargingEnabled`: hier harmlos, weil die Wallbox ohnehin kappt.
  Auf einer 16-A-Installation würde er echte 16 A starten — genau die im PR dokumentierte
  Verhaltensänderung.

## Regressionstest

`tools/regression_test_charging.py` — braucht **kein Auto und keine Sonne**, läuft also auch nachts.
Liest die Wahrheit per Modbus TCP direkt aus Register 261 (Daheimladen-Register 91), nicht über MQTT:
Die Firmware publiziert den Stromwert nur im Bereich 6–16 A, und genau die 0 A sind hier interessant.

```bash
set -a; . ~/dev/homeassistant/.env; set +a
python3 tools/regression_test_charging.py
```

Deckt ab: Seeding aus der Hardware beim Boot, Sollwert überlebt Disable + Telemetrie-Reads von 0 A,
Clamping, Fallback beim Enable mit Null-Sollwert. Pausiert die HA-Automation und stellt sie danach
wieder her. **Hätte den ersten (falschen) Fix sofort gefangen.**

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
