# CLAUDE.md

Project-specific notes for this fork of HeidelBridge (ESP32 firmware for Heidelberg wallboxes).

## Fork- und Branch-Strategie

- `origin` = `sadilek/HeidelBridge` (dieser Fork). `upstream` = `BorisBrock/Heidelbridge` (Original).
- `main` wird als **sauberer Spiegel von `upstream/main`** gehalten — keine eigenen Commits, nur Fast-Forward via `git fetch upstream && git merge --ff-only upstream/main`.
- **`kupa5`** ist der Arbeits-Branch mit allen eigenen Anpassungen. Upstream wird per **Merge** (nicht Rebase) eingezogen, da `kupa5` gepusht/geteilt ist (kein force-push).
- Eigene Anpassungen möglichst **additiv** halten (neue Dateien statt Upstream-Dateien editieren) → minimiert Merge-Konflikte.

### Working through Obeya

- Obeya's main branch for this repository is `kupa5` (set in Obeya's configuration on the
  owner's machine, not in `.obeya/settings.json`). Cards start from `kupa5`, and approved work
  lands there locally, without a pull request (`land: "main"`). Nothing is pushed: the owner pushes
  `kupa5` to `origin` themselves. Do not push `kupa5`.
- When a card's task asks for the change upstream as well ("auch als PR an upstream"), the
  upstream PR comes in addition, after the owner approved and the work landed on `kupa5`. Create
  a branch from `upstream/main` in a separate worktree (do not switch the card's branch), carry
  the change over without fork-specific parts (Kupa5 board, `last_command` echo, the `unique_id`
  underscore, CLAUDE.md), build it with `pio run -e esp32`, push the branch to `origin` and open
  the PR against `BorisBrock/HeidelBridge` `main` with `gh pr create -R BorisBrock/HeidelBridge
  --head sadilek:<branch>`. Report it with Obeya's `pr_opened`. Obeya then watches it and
  forwards Boris' review comments, and the card stays open until he merges. Example:
  BorisBrock/HeidelBridge#82 (`feature/monotonic-energy-counter`).

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

## Firmware-Fixes — alle upstream gemergt (2026-08-04)

Nichts davon ist noch fork-spezifisch. Historie für den Fall, dass die Symptome wieder auftauchen:

- **#73** `default_entity_id` ohne Domain-Präfix → alle 15 Entities landeten als `unnamed_device`,
  und „Recreate entity IDs" verweigerte die Arbeit. Dazu: Discovery muss `retain=true` sein, sonst
  bleibt nach einem HA-Neustart alles mit `command_topic` `unavailable`.
- **#74** Ladezustand wurde erfunden statt gelesen. `Init()` las Register 261 nie, und
  `GetChargingCurrentLimit()` (Telemetrie) schrieb sein Messergebnis in dasselbe Member wie den
  Sollwert — ein einziger 0-A-Read zerstörte ihn. Soll- und Messwert sind jetzt getrennt.
- **#75** `Init()` schrieb Register 258, merkte sich den Wert aber nicht → `mStandbyEnabled` blieb auf
  dem Default. Fiel nur auf, wenn Reads fehlschlugen (Symptom in Issue #72).

Boris' Edits beim Mergen: `ClampToWallboxRange` und `WriteCurrentLimitRegister` sind jetzt **private
Member** statt Free Functions in einer anonymen Namespace, und `InitialChargingCurrentLimitA` ist als
`MaxChargingCurrentA` definiert statt 16.0f zu wiederholen. Er kürzt außerdem Kommentare im Code
konsequent — Commit-Messages lässt er unangetastet.

## Was in `kupa5` bewusst von upstream abweicht

Beim nächsten Upstream-Merge nicht wegwerfen:

- **Seit Upstream 5.0.0 (Merge 2026-11-12) gibt es keinen Enable-Charging-Switch mehr.** Laden wird
  nur noch über `control/charging_current_limit` gesteuert (0 = gesperrt, 6–16 A erlaubt, 0,1-A-Schritte).
  Die frühere Fork-Abweichung (`unique_id` `%_control_enable_charging` mit Unterstrich) ist damit
  hinfällig. Retained Commands ignoriert die Firmware seit 5.0.0, HA muss ohne Retain senden.
- **Diagnose-Echo** nach `{DeviceName}/internal/last_command`. Bewusst nicht upstream vorgeschlagen,
  weil es öffentliche Topic-Fläche hinzufügt. Seit 5.0.0 (`EchoCommand()` in `MQTTManager.cpp`):
  `topic=… raw='…' len=… match=current_limit|standby|none parsed='…' result=queued|invalid|ignored_retained|ignored_fragmented|ignored_unknown_topic`.
  `queued` heißt nur „an die Hauptschleife übergeben"; das Feld `enabled=` gibt es nicht mehr.

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
- **Geklärt (2026-08-04): der geteilte Topic-Puffer war ein echter Race.** `gMqttTopic` wurde aus zwei
  Tasks beschrieben — `PublishStatusMessages()` im Main-Loop, die AsyncMqttClient-Callbacks im
  AsyncTCP-Task. `SetString()` und das `publish()`, das den Puffer liest, sind nicht atomar. Landete
  ein Callback dazwischen, ging der Publish auf das Topic, das der Callback gerade gebaut hatte.
  Konkret: der **retained** State `enable_charging` = „OFF" landete auf
  `HeidelBridge/control/enable_charging` — ein Topic, das das Gerät selbst abonniert. Es las seinen
  eigenen State als Kommando und schaltete das Laden ab, alle ~30 s und zusätzlich bei jedem
  Reconnect (retained). Das erklärt auch das am 2026-08-03 als ungeklärt notierte „`turn_on` bewirkt
  nichts". Fix: eigener `PrefixedString` für die Callbacks, damit jeder Puffer genau einen Schreiber
  hat. **Achtung:** beide brauchen `SetPrefix()` — ohne das gehen die Subscriptions still auf
  `/control/...` statt `HeidelBridge/control/...` und es kommt gar kein Kommando mehr an.
  Noch offen (bisher nicht auffällig): `TopicBuffer`/`PayloadBuffer` in
  `PublishHomeAssistantDiscoveryTopic` werden weiterhin aus beiden Tasks benutzt.

## Retained Nachrichten auf Command-Topics prüfen

Symptom „Laden geht nach jedem Reconnect sofort wieder aus": auf `{DeviceName}/control/*` liegt eine
retained Nachricht. Löschen mit leerem Payload und `retain: true` — **Achtung**, die Firmware liest
das leere Payload als Kommando (`""` ≠ `"ON"` → aus), also danach einen Zyklus abwarten.

Was tatsächlich auf den Topics liegt, sieht man nicht am Diagnose-Echo der Firmware, sondern nur mit
einem eigenen MQTT-Sensor auf dem Command-Topic — der Diagnose-String wird erst *nach* dem
Modbus-Write gebaut, seine `topic`/`payload`-Zeiger können dann schon überschrieben sein.

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

Deckt ab (seit 5.0.0): Sollwert landet im Register, 0 A sperrt, Telemetrie-Reads von 0 A beeinflussen
den nächsten Request nicht, Clamping, retained und ungültige Commands werden ignoriert. Pausiert die HA-Automation und stellt sie danach
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

## Energiezähler geht nach Stromausfall zurück (Test 2026-09-03) — gelöst per Offset

Register 17/18 („Energy since Installation") sichert die Wallbox **nur beim Übergang in den
Standby** ins EEPROM. Da die Firmware Standby verbietet (Register 258 = 4, `AllowStandby = false`),
fällt das Register nach jedem Stromausfall auf den Stand der letzten Standby-Sicherung zurück
(2026-08-31: 874,63 → 775,88 kWh, der Wert vom 2026-08-03). Mit Sicherungs-Tests bestätigt:
Standby aus → Rückfall; Standby 15 min erlaubt, dann Stromausfall → Stand bleibt.

Standby erlauben ist keine Option: ~13 min nach dem Einschalten antwortet die schlafende Box nicht
mehr auf Modbus, die Hauptschleife hängt in Timeouts/Retries, der MQTT-Keepalive reißt und HA sieht
alle 30–60 s `unavailable`. Nach einem HeidelBridge-Neustart steht der Standby-Schalter wieder auf
aus, weil `Init()` 258 = 4 schreibt.

**Lösung (2026-09-03):** `src/Components/Wallbox/EnergyCounter.{h,cpp}` hält im NVS einen Offset
und den zuletzt publizierten Stand (`energy_offset`, `energy_hwm`). Publiziert wird `raw + offset`,
nur in persistierten 10-Wh-Schritten und beim Laden höchstens alle 30 s (Flash-Wear: im 11-kW-
Dauerlauf 33 statt 5,5 Jahre, Rechnung in `docs/EnergyCounterDesign.md`), und jeder unplausible Sprung des Registers (Rückfall oder
Anstieg schneller als 11 kW) wird nach 30 s Bestätigung per Offset überbrückt. Nur
`HeidelbergWallbox::Update()` (Hauptschleife, alle 5 s) liest das Register; MQTT und Modbus-TCP
lesen den Cache. Design und Verlaufstabelle: `docs/EnergyCounterDesign.md`. Unit-Tests:
`pio test -e native` (hardwarefrei, `test/test_energy_counter/`). Diagnose: MQTT
`internal/energy_offset_wh` / `internal/energy_raw_wh`, Web-UI Abschnitt „Energy Meter" (Zähler
dort setzen = Rebase, z. B. nach NVS-Verlust). Nach dem ersten Boot mit leerem NVS publiziert
`energy_meter` 30 s lang nichts, danach den Registerwert als Baseline (kein Sprung in HA).
