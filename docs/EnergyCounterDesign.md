# Design: monotoner Energiezähler mit persistiertem Offset

Stand 2026-09-03, noch nicht implementiert. Hintergrund in `CLAUDE.md`
(„Energiezähler geht nach Stromausfall zurück").

## Problem

Register 17/18 der Heidelberg Energy Control („Energy since Installation") wird nur beim
Übergang in den Standby ins EEPROM gesichert. Da die Firmware Standby verbietet, fällt der
Wert nach jedem Stromausfall auf die letzte Sicherung zurück. Home Assistant
(`state_class: total_increasing`) interpretiert einen Rückfall > 10 % als Reset und addiert
den ganzen neuen Wert; kleinere Rückfälle gehen als negative Differenz in die Summe.

## Ziel

Der publizierte Wert (`energy_meter`, Modbus-TCP-Register 28) ist **monoton steigend** über
Wallbox-Stromausfälle und HeidelBridge-Neustarts hinweg. Der Verlust pro Stromausfall ist
auf die Sicherungsgranularität begrenzt (≤ 100 Wh, nur wenn im Moment des Ausfalls geladen
wurde). Kein Über- oder Doppelzählen, insbesondere nicht durch Lesefehler.

## Zustand

| Name | Ort | Typ | Bedeutung |
|---|---|---|---|
| `raw` | RAM | uint32 Wh | zuletzt **erfolgreich** gelesener Registerwert |
| `offset` | RAM + NVS `energy_offset` | uint32 Wh | wird auf `raw` addiert |
| `hwm` | RAM + NVS `energy_hwm` | uint32 Wh | höchster je publizierter Wert (high-water mark) |
| `hwmPersisted` | RAM | uint32 Wh | zuletzt ins NVS geschriebener `hwm` |
| `pending` | RAM | Zähler + `rawFirst` | laufendes Bestätigungsfenster für einen Rückfall |

Publizierter Wert: `published = max(raw + offset, hwm)`.

## Regeln

1. **Nur erfolgreiche Reads** gehen in die Logik ein. Modbus-Fehler ändern nichts,
   es wird der letzte `published` weiter gemeldet (wie heute).
2. **`raw == 0` gilt als ungültig** und wird verworfen (Warnung ins Log). Ein echtes
   Installations-Total von exakt 0 gibt es nur bei einer fabrikneuen Box und ist nach
   der ersten 10-Wh-Ladung wieder erfasst.
3. **Normalfall** `raw + offset >= hwm`: Fenster löschen, `hwm = raw + offset`.
   Wenn `hwm - hwmPersisted >= 100 Wh`: `hwm` ins NVS schreiben, `hwmPersisted = hwm`.
4. **Rückfall** `raw + offset < hwm`: Fenster starten bzw. fortführen.
   - Beim Start: `rawFirst = raw`, Zähler = 1.
   - Folge-Reads müssen plausibel sein: `raw >= rawLetzter` und
     `raw - rawLetzter <= 200 Wh` (11 kW × 5 s Lesezyklus ≈ 15 Wh, großzügig). Sonst
     Fenster neu starten.
   - Nach **5 aufeinanderfolgenden** Rückfall-Reads (≈ 25 s): `offset = hwm - rawFirst`,
     `offset` sofort ins NVS, Fenster löschen. Damit zählt Energie, die während des
     Fensters geladen wurde, korrekt mit (`raw - rawFirst`).
   - Während des Fensters wird `hwm` publiziert (durch `max(...)`), nach außen passiert
     nichts.
5. **Vor dem ersten erfolgreichen Read** nach dem Booten wird `hwm` aus dem NVS publiziert
   (monoton, höchstens 100 Wh zu niedrig), nicht 0 und nicht „nan".
6. **Reihenfolge im NVS**: erst `offset`, dann `hwm`. Bricht der Strom dazwischen weg, wird
   der Rückfall beim nächsten Boot erneut erkannt und identisch berechnet.
7. **Diagnose**: `internal/energy_offset` und `internal/energy_hwm` werden mit den anderen
   Internals publiziert. `/api/settings_read` zeigt beide; `/api/settings_write` darf
   `energy_offset` setzen (Reparatur nach NVS-Verlust oder Wallbox-Tausch). Nach manuellem
   Setzen gilt `hwm = raw + offset` beim nächsten Read.

## NVS-Belastung

`offset` ändert sich nur bei einem Rückfall (selten). `hwm` wird alle 100 Wh geschrieben:
bei ~1.500 kWh/Jahr ≈ 15.000 Einträge à 32 Byte, verteilt über die NVS-Seiten ≈ wenige
hundert Löschzyklen pro Seite und Jahr bei 100.000 Zyklen Lebensdauer. Unkritisch.

## Verlaufstabelle

`R` = Registerwert, `O` = Offset, `H` = hwm, alle in kWh. Ausgangslage, wenn nicht
anders angegeben: `R = 776`, `O = 0`, `H = 776`, NVS aktuell.

| Nr | Verlauf | Verhalten | Ergebnis `published` | korrekt / Verlust |
|---|---|---|---|---|
| 1 | Normalbetrieb, Laden, R steigt 776 → 780 | Regel 3, H folgt, alle 100 Wh NVS-Write | 776 → 780 | ja / 0 |
| 2 | Erster Start nach Firmware-Update, NVS leer | O = 0, H = 0; erster Read setzt H = R | 776, kein Sprung | ja / 0 (vor dem ersten NVS-Write ist noch nichts geschützt) |
| 3 | Nur HeidelBridge-Neustart (OTA, Crash), Wallbox läuft, NVS aktuell | Boot publiziert H = 776, erster Read R + O = 776 ≥ H | 776 | ja / 0 |
| 4 | Nur HeidelBridge-Neustart, seit letztem NVS-Write 80 Wh geladen (R = 776,08, H_NVS = 776,00) | Boot publiziert 776,00, erster Read 776,08 ≥ H → H = 776,08 | 776,00 → 776,08 | ja / 0 (kurzzeitig 80 Wh zu niedrig, monoton) |
| 5 | Stromausfall beide, Wallbox fällt auf 700 zurück, nicht geladen seit letztem Write | Boot publiziert 776; Reads 700 < 776, 5× bestätigt → O = 76 | 776 durchgehend | ja / 0 |
| 6 | Stromausfall beide mitten im Laden, R war 780,07, H_NVS = 780,00, Wallbox fällt auf 700 | Boot publiziert 780,00; O = 80,00 | vorher 780,07, nachher 780,00 | ja / 70 Wh, HA sieht −0,07 kWh (< 10 %, keine Reset-Erkennung) |
| 7 | Nur Wallbox stromlos, HeidelBridge läuft (getrennte Versorgung) | Reads schlagen fehl (Regel 1), H exakt im RAM; danach R = 700 < H → O = H − 700 | unverändert | ja / 0 |
| 8 | Zweiter Stromausfall, bevor die Wallbox erneut gesichert hat (Fall 5, dann 4 kWh geladen, dann wieder Ausfall) | H = 780, R fällt erneut auf 700 → O = 80 | 780 | ja / 0 |
| 9 | Einzelner Modbus-Timeout | Regel 1, kein Einfluss | letzter Wert | ja / 0 |
| 10 | Wallbox liefert nach eigenem Boot kurz 0, dann 776 | 0 verworfen (Regel 2), 776 ≥ H | 776 | ja / 0 |
| 11 | Wallbox liefert nach Boot kurz einen alten Wert 700, dann echten 776 (Ausreißer nach unten) | Fenster startet, 3. Read 776 ≥ H → Fenster gelöscht, kein Offset | 776 | ja / 0 |
| 12 | Wallbox liefert kurz 700, dann 700,05, dann 1200 (unplausibler Sprung) | Fenster wird neu gestartet (Regel 4), erst 5 plausible Reads bestätigen | 776 bzw. später 1200 + O | ja, sofern 1200 echt; Fehlmessung nach oben ist nicht abgesichert (siehe 20) |
| 13 | Auto direkt nach Stromausfall angesteckt, während des Fensters wird geladen (700 → 700,02) | rawFirst = 700, O = 76; published = 700,02 + 76 | 776,02 | ja / 0 |
| 14 | Stromausfall während eines laufenden Bestätigungsfensters | Fenster (RAM) verloren; nach Boot erneut erkannt, gleiche Rechnung | 776 | ja / 0 |
| 15 | Wallbox getauscht, neues Gerät zeigt 0 → nach erster Ladung 0,01 | 0 verworfen; 0,01 < H → O = 776 − 0,01 | 776,01 | ja / 0 |
| 16 | Wallbox getauscht, neues Gerät zeigt 5.000 | 5.000 ≥ H → H = 5.000 | Sprung 776 → 5.000, HA zählt 4.224 kWh | **nein**, Offset manuell auf −4.224 setzen (Regel 7, Offset muss daher signed int32 sein) und HA-Statistik korrigieren |
| 17 | NVS gelöscht (Flash-Erase), Register weiterhin bei 700 nach früherem Rückfall | O = 0, H = 0 → published 700, HA sieht 776 → 700 (−9,8 %: negative Differenz; > 10 % wäre Reset) | 700 | **nein**, Offset manuell nachtragen (Regel 7) und HA-Statistik korrigieren |
| 18 | Offset manuell gesetzt | H = R + O beim nächsten Read, kein Fenster | R + O | ja, Verantwortung beim Bediener |
| 19 | HA oder Broker neu gestartet, HeidelBridge läuft | keine Logik betroffen, Wert kommt mit dem nächsten Publish | unverändert | ja / 0 |
| 20 | Fehlmessung nach oben (Register liefert einmalig 900) | H springt auf 900 und bleibt (Ratsche) | 900 | **nein**, dauerhaft +124 kWh. Modbus-CRC macht das unwahrscheinlich; optional Regel „Anstieg > 200 Wh pro Read nur nach 5-facher Bestätigung", wobei Fall 4 (Anstieg nach Boot) ausgenommen bleiben muss |
| 21 | NVS-Write schlägt fehl | Log-Fehler, RAM-Werte bleiben korrekt; nächster Write versucht es erneut | korrekt bis zum nächsten Neustart | ja / nach Neustart wie Fall 4 bis 6 |
| 22 | Überlauf | uint32 Wh = 4,29 GWh; float32 im Publish-Pfad hat bei 16,7 MWh noch 1 Wh Auflösung | – | ja für Jahrzehnte |
| 23 | Standby versehentlich erlaubt, Box schläft, Reads scheitern minutenlang | wie Fall 9, kein Einfluss auf den Zähler | letzter Wert | ja / 0 |
| 24 | Rückfall exakt auf H − 0,01 (Wallbox hat unmittelbar vor dem Ausfall gesichert) | Rückfall wie jeder andere, O = 0,01 | 776 | ja / 0 |
| 25 | Modbus-TCP-Client (Daheimladen-Emulation) liest Register 28 | bekommt `published`, gleiche Quelle wie MQTT | konsistent | ja |

Fälle 16, 17 und 20 sind die einzigen ohne automatische Korrektur; alle drei sind Bedien-
oder Hardware-Ereignisse, die ohnehin einen manuellen Eingriff in HA erfordern, und Regel 7
macht die Reparatur ohne Reflash möglich.

## Umsetzung

- Neue Klasse `EnergyCounter` (`src/Components/Wallbox/EnergyCounter.{h,cpp}`) mit
  `uint32_t Update(uint32_t rawWh)`, `uint32_t Get()`, `void SetOffset(int32_t)`,
  Persistenz über ein kleines Interface, damit die Logik ohne Hardware testbar ist.
- `HeidelbergWallbox::GetEnergyMeterValue()` ruft `Update()` nur im Erfolgszweig auf und
  gibt sonst `Get()` zurück. `DummyWallbox` bleibt unverändert.
- `Settings`: `energy_offset` (int32) und `energy_hwm` (uint32) im bestehenden
  Preferences-Namespace, Lesen beim Boot, Schreiben aus dem `EnergyCounter` über das
  Interface.
- `MQTTManager`: zwei Internals-Topics; Web-Settings: Anzeige beider Werte, Schreiben
  des Offsets.
- Abnahme: Unit-Tests für Fälle 1–15 gegen `EnergyCounter`; danach Sicherungstest wie am
  2026-09-03 (0,1 kWh laden, Strom weg, Wert muss bleiben) und Kontrolle der HA-Statistik.
