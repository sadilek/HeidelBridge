# Design: monotoner Energiezähler mit persistiertem Offset

Stand 2026-09-03, umgesetzt in `src/Components/Wallbox/EnergyCounter.{h,cpp}`. Version 2 nach
Codex-Review; die Änderungen gegenüber dem ersten Entwurf stehen am Ende.

## Problem

Register 17/18 der Heidelberg Energy Control („Energy since Installation") wird nur beim
Übergang in den Standby ins EEPROM gesichert. Da die Firmware Standby verbietet, fällt der
Wert nach jedem Stromausfall auf die letzte Sicherung zurück. Home Assistant
(`state_class: total_increasing`) interpretiert einen Rückfall > 10 % als Reset und addiert
den ganzen neuen Wert; kleinere Rückfälle gehen als negative Differenz in die Summe.

## Ziel

Der publizierte Wert (`energy_meter`, Modbus-TCP-Register 28) ist **monoton steigend** über
Wallbox-Stromausfälle, HeidelBridge-Neustarts und Fehlmessungen hinweg. Verlust pro
Stromausfall höchstens ein Persistenzschritt (10 Wh). Kein Über- oder Doppelzählen.

## Grundidee

`published = raw + offset`, wobei der Offset jeden **unplausiblen Sprung des Registers in
beide Richtungen** überbrückt. Publiziert wird ausschließlich der zuletzt erfolgreich ins
NVS geschriebene Stand, dadurch ist Monotonie über Neustarts konstruktiv garantiert.

## Zustand

| Name | Ort | Typ | Bedeutung |
|---|---|---|---|
| `lastRaw`, `lastRawMs` | RAM | uint32 | zuletzt **akzeptierter** Registerwert und Zeitpunkt |
| `offset` | RAM + NVS `energy_offset` | int64 Wh | wird auf `raw` addiert; Sprünge werden hierüber überbrückt |
| `hwm` | RAM | uint32 Wh | `raw + offset` des letzten akzeptierten Reads |
| `published` | RAM + NVS `energy_hwm` | uint32 Wh | zuletzt erfolgreich persistierter `hwm`; das ist der nach außen sichtbare Wert |
| `pending` | RAM | first/last raw, Start, Samples | Bestätigungsfenster für einen abweichenden Wert |

Alle Rechnungen in `int64`, Ergebnis auf `[0, UINT32_MAX]` gesättigt.

## Regeln

1. **Ein Poller.** `HeidelbergWallbox::Update()` liest das Register alle 5 s aus der
   Hauptschleife und füttert `EnergyCounter::Update(raw, millis())`. MQTT und Modbus-TCP
   lesen nur den Cache (`GetEnergyMeterValue()`). Nur erfolgreiche Reads zählen.
2. **Plausibel** ist ein Read, wenn `raw >= lastRaw` und der Anstieg höchstens
   `11 kW × vergangene Zeit × 1,25 + 100 Wh` beträgt. Dann: `hwm = raw + offset`; sobald
   `hwm − published >= 10 Wh` **und seit dem letzten Write mindestens 30 s vergangen sind**,
   wird `hwm` ins NVS geschrieben und damit publiziert. Das Zeitlimit begrenzt den
   Flash-Verschleiß; Baseline und Überbrückungen (Regel 3, 4, 6) schreiben sofort.
3. **Unplausibel** (Rückfall oder zu großer Anstieg): der Wert wandert ins Fenster. Folge-Reads
   müssen untereinander plausibel sein, sonst startet das Fenster neu. Nach **30 s und
   mindestens 3 Samples** wird der Sprung überbrückt: `offset = hwm − rawFirst`, dann
   `hwm = rawLast + offset` (Energie aus dem Fenster zählt mit). Während des Fensters bleibt
   `published` unverändert. Kehrt das Register vorher auf einen plausiblen Wert zurück, wird
   das Fenster verworfen.
4. **Boot.** Der erste Wert durchläuft ebenfalls das Fenster. Danach: NVS leer → Baseline
   (`offset = 0`). Sonst `rawFirst + offset >= published` → Anstieg akzeptieren (Energie
   während der HeidelBridge-Auszeit zählt); andernfalls Rückfall überbrücken wie in Regel 3.
   Bis dahin wird der NVS-Stand publiziert; bei leerem NVS wird nichts publiziert
   (`HasEnergyMeterValue() == false`, MQTT schweigt, Register 28 liefert 0).
5. **NVS-Reihenfolge**: Offset vor `published`. Schlägt ein Write fehl, bleibt der Zustand
   „dirty" und wird beim nächsten Read erneut versucht; `published` bewegt sich nur nach
   erfolgreichem Write.
6. **Manuelles Setzen** (`/api/settings_write`, Feld `energy-counter-kwh`, nur gesendet wenn
   im UI geändert): atomar `offset = ziel − lastRaw`, `published = ziel`, beides persistiert
   vor der HTTP-Antwort; ohne akzeptierten Registerwert wird abgelehnt. Das UI zeigt Zähler,
   Registerwert und Offset an (`energy-counter-kwh`, `energy-raw-kwh`, `energy-offset-kwh`).
7. **Diagnose per MQTT**: `internal/energy_offset_wh`, `internal/energy_raw_wh`.
8. **Nebenläufigkeit**: Update (Hauptschleife), Cache-Reads (MQTT, Modbus-TCP-Task) und
   Setzen (Webserver-Task) laufen unter einem Mutex in `HeidelbergWallbox`.

## NVS-Belastung

Partition 16 KB = 4 Seiten à 126 Einträge (32 Byte); NVS rotiert reihum, ein Write belegt
einen Eintrag, gelöscht wird erst beim Umkopieren einer vollen Seite (~86 freie Einträge je
Seite nach Abzug der lebenden Settings). Endurance des Flash 100.000 Zyklen.

| Profil | Writes/Jahr | Löschungen je Seite/Jahr | Lebensdauer |
|---|---|---|---|
| diese Anlage, 2,3 kW, ~1.350 kWh/Jahr | ~35.000 | ~100 | > 900 Jahre |
| 11 kW rund um die Uhr, ohne Zeitlimit (nur 10-Wh-Schritt) | 6,3 Mio. (1 je 5-s-Poll) | ~18.000 | 5,5 Jahre |
| 11 kW rund um die Uhr, mit 30-s-Limit | 1,05 Mio. | ~3.000 | 33 Jahre |

Preis des Zeitlimits: Verlust je Stromausfall bis zu 30 s Ladung (19 Wh bei 2,3 kW, 92 Wh bei
11 kW) statt 10 Wh. Eine Seitenlöschung hält den Chip 45 bis 400 ms an (Flash-Cache aus); mit
Zeitlimit passiert das im Dauerlauf alle 45 Minuten statt alle 7.

## Verlaufstabelle

`R` = Registerwert, `O` = Offset, `P` = published, alle in kWh. Ausgangslage, wenn nicht
anders angegeben: `R = 776`, `O = 0`, `P = 776`, NVS aktuell. Jede Zeile hat einen Unit-Test
in `test/test_energy_counter/test_main.cpp` (`pio test -e native`).

| Nr | Verlauf | Verhalten | Ergebnis `P` | korrekt / Verlust |
|---|---|---|---|---|
| 1 | Normalbetrieb, Laden, R steigt 776 → 780 | plausibel, alle 10 Wh NVS-Write | 776 → 780 in 10-Wh-Schritten | ja / 0 |
| 2 | Erster Start nach Firmware-Update, NVS leer | 30 s nichts publiziert, dann Baseline | 776, kein Sprung | ja / 0 |
| 3 | Nur HeidelBridge-Neustart, Wallbox läuft, NVS aktuell | Boot publiziert 776, Fenster bestätigt 776 | 776 | ja / 0 |
| 4 | Nur HeidelBridge-Neustart, während der Auszeit 80 Wh geladen | Boot publiziert 776,00; Fenster bestätigt 776,08 ≥ P → akzeptiert | 776,00 → 776,08 | ja / 0 |
| 5 | Stromausfall beide, Wallbox fällt auf 700, seit letztem Write nicht geladen | Boot publiziert 776; 700 < P → nach 30 s O = 76 | 776 durchgehend | ja / 0 |
| 6 | Stromausfall beide mitten im Laden, R war 776,024, P = 776,017 | Boot publiziert 776,017; O = 76,017 | 776,024 → 776,017 im RAM, nach außen nie sichtbar | ja / 7 Wh, mit 30-s-Limit bis zu 30 s Ladung |
| 6a | 22 kW Dauerladen, Polls alle 5 s | nur alle 30 s ein NVS-Write, P hinkt ≤ 30 s hinterher | monoton in ~180-Wh-Schritten | ja / ≤ 180 Wh je Ausfall |
| 6b | Überbrückung kurz nach einem Write | Offset und P sofort geschrieben, Zeitlimit greift nicht | konsistent | ja / 0 |
| 7 | Nur Wallbox stromlos, HeidelBridge läuft, R danach 700 und ladend | Reads scheitern (ignoriert); Fenster mit RAM-hwm, Energie im Fenster zählt | 776,09 nach 700,00 → 700,09 | ja / 0 |
| 8 | Zweiter Stromausfall, bevor die Wallbox erneut gesichert hat | O wächst auf 80 | 780 | ja / 0 |
| 9 | Einzelner Modbus-Timeout, Standby-Aussetzer | kein Read, kein Einfluss | unverändert | ja / 0 |
| 10 | Wallbox liefert nach Boot kurz 0, dann 776 | 0 landet im Fenster, 776 ist plausibel zu lastRaw → Fenster verworfen | 776 | ja / 0 |
| 11 | Kurzer Ausreißer nach unten (700, 700, dann 776,01) | Fenster verworfen | 776,01 | ja / 0 |
| 12 | Alter Wert 700 hält > 30 s, danach echter Wert 776 | erst O = 76, dann Sprung nach oben unplausibel → O = 0 | 776 durchgehend, keine Doppelzählung | ja / 0 |
| 13 | Auto direkt nach Stromausfall angesteckt, im Fenster 700 → 700,09 | O aus rawFirst, Energie aus dem Fenster zählt | 776,09 | ja / 0 |
| 14 | Stromausfall während eines Fensters | Fenster (RAM) verloren, nach Boot erneut erkannt | 776 | ja / 0 |
| 15 | Wallbox getauscht, neues Gerät 0 → 0,01 | 0 hält > 30 s → O = 776 | 776,01 | ja / 0 |
| 16 | Wallbox getauscht, neues Gerät zeigt 5.000 | Anstieg unplausibel → O = −4.224 | 776, dann weiter | ja / 0 (Design v1: manuell) |
| 17 | NVS gelöscht, R steht nach früherem Rückfall bei 700 | Baseline 700, HA sieht 776 → 700 | 700 | **nein**, Zähler im UI auf 776 setzen und HA korrigieren |
| 18 | Zähler manuell gesetzt | O = ziel − lastRaw, P = ziel, persistiert | ziel | ja, Bedienerverantwortung |
| 19 | HA oder Broker neu gestartet | nicht betroffen | unverändert | ja / 0 |
| 20 | Einmalige Fehlmessung nach oben (900) | unplausibel, nächster Read plausibel → verworfen | 776 | ja / 0 |
| 21 | Fehlmessung nach oben hält > 30 s | O = −124, danach weiter | 776 | ja / 0 (Design v1: dauerhaft +124) |
| 22 | Unplausible Folge im Fenster (700, 700,05, 1200) | Fenster startet bei 1200 neu | 776, nach 30 s Überbrückung | ja / 0 |
| 23 | Eine Stunde keine erfolgreichen Reads, dann +10 kWh | plausibel für 1 h → akzeptiert | 786 | ja / 0 |
| 24 | NVS-Write schlägt fehl | RAM korrekt, P bleibt stehen, Retry beim nächsten Read | monoton | ja / bis zum Neustart 0, danach wie 6 |
| 25 | Negativer Offset, Register fällt unter das Absorbierbare | Sättigung auf 0, P bleibt | unverändert | ja |
| 26 | `millis()` läuft über | unsigned Differenz | unverändert | ja |
| 27 | Modbus-TCP-Client liest Register 28 | Cache, kein Hardware-Read, kein Einfluss aufs Fenster | konsistent | ja |

Ohne automatische Korrektur bleibt nur 17 (NVS-Verlust); dafür gibt es das manuelle Setzen
im UI.

## Änderungen gegenüber Version 1 (Codex-Review)

- Publiziert wird nur der persistierte Stand, Schrittweite 10 Wh statt 100 Wh; damit ist der
  Wert auch über einen gemeinsamen Stromausfall monoton (Fall 6 zeigte vorher einen Rückgang).
- Ein einziger Poller in der Hauptschleife statt Reads in jedem Getter; MQTT und Modbus-TCP
  lesen den Cache. Das Fenster ist zeitbasiert (30 s), nicht aufrufbasiert.
- Sprünge nach oben werden wie Rückfälle überbrückt. Das löst den verspätet korrigierten
  Startwert (Fall 12), den Wallbox-Tausch nach oben (16) und die Ratsche bei Fehlmessungen
  (21). Die Sonderregel `raw == 0` entfällt.
- Offset `int64`, alle Rechnungen `int64` mit Sättigung.
- Manuelles Setzen als atomare, sofort persistierte Operation mit Zielwert statt Offset.
- Persistenz über dedizierte `Settings`-Methoden außerhalb von `WriteToPersistentMemory()`;
  Write-Rückgabewerte werden geprüft, Dirty-Zustand mit Retry.
- Mutex für Cache-Reads und Setzen aus anderen Tasks.
- Nachträglich (Flash-Wear-Analyse): höchstens ein `published`-Write je 30 s beim Laden.
