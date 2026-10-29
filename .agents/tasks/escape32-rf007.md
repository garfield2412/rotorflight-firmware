# Hardware-Folgeauftrag: Vantac RF007 und ESCape32 17.3

Stand: 2026-10-29. Recherche und Quellkompatibilität belegt, Hardwareadapter noch
nicht implementiert oder auf einem realen FC/ESC geprüft. Dieser Auftrag ergänzt
escape32-forward-programming.md. Keine Ziel-Capability allein wegen dieses
Boardprofils oder eines Build-Flags freischalten.

## Bekannte Nutzerkonfiguration

- FC: Vantac RF007; MCU STM32F722RET6, Unified-Firmwarefamilie STM32F7X2.
- Verbindung: DShot-Signalleitung vom Motor-/ESC-Throttle-Ausgang zum Regler.
- ESC-Firmware: ESCape32 Revision17, Patch3. Konkreter ESC-Hardwaretarget und
  Sensor-/Treiberbestückung sind noch nicht benannt.

Die Ressourcen unten sind Defaults des offiziellen Boardtargets. Sie ersetzen
keinen read-only Ressourcen-/Timer-/DMA-Abzug des tatsächlich betriebenen FC.

## Nachgewiesene Ressourcen

Primärquelle: [FRSK-VANTAC_RF007.config](https://github.com/rotorflight/rotorflight-targets/blob/1d9cb4dc6f85c7895c2b089f4b8aa776ca3d3bd3/configs/FRSK-VANTAC_RF007.config),
Blob58fe80e13a4fae9f662377cbf165e1d936862dd5.
MCU-Angabe: [Rotorflight RF007-Dokumentation](https://rotorflight.org/docs/2.1.0/controllers/frsky-007).

| Ressource | Pin | Timer / DMA-Default |
| --- | --- | --- |
| MOTOR1 / ESC-Throttle | PA09 | TIM1_CH2, AF1; DMA2 Stream6 Channel0 |
| SERVO1 | PB04 | TIM3_CH1, AF2 |
| SERVO2 | PB05 | TIM3_CH2, AF2 |
| SERVO3 | PB00 | TIM3_CH3, AF2 |
| SERVO4 / Tail | PA15 | TIM2_CH1, AF1 |
| Frequenzeingang / RPM | PA02 | TIM5_CH3, AF2 |
| SERIAL_RX2 / TLM | PA03 | Boardresource ist RX2; nicht mit RPM verwechseln |

PA09 ist ein normales CH2, kein CH2N. Der MCU-Timer kann grundsätzlich für
Capture/RX verwendet werden. Das beweist weder die elektrische Rückrichtung
am ESC-Stecker noch korrekten Halbduplex-Turnaround.

Im Defaulttarget steht noch PWM und dshot_bidir=OFF. Daraus nicht ableiten,
welche DShot-/Bidirectional-Einstellungen beim Nutzer aktuell aktiv sind.
Keine automatische Konfigurationsänderung zur Programmierung durchführen.

Firmware-Primärquellen, jeweils Rotorflight-Commit
7b54df9e1f34f0bbc825cbfe7df17a0a59cbd032:

- [timer_stm32f7xx.c](https://github.com/rotorflight/rotorflight-firmware/blob/7b54df9e1f34f0bbc825cbfe7df17a0a59cbd032/src/main/drivers/timer_stm32f7xx.c)
  bestätigt DEF_TIM(TIM1, CH2, PA9, ...); Blob32055ad3e0ea3bfca801fe46a030efe699a200b3.
- [pg/esc_serial.c](https://github.com/rotorflight/rotorflight-firmware/blob/7b54df9e1f34f0bbc825cbfe7df17a0a59cbd032/src/main/pg/esc_serial.c)
  setzt ESCSERIAL_TIMER_TX_PIN standardmäßig auf NONE;
  Blobfd256de6cbe559620abd124147832201440a3a43.
- [serial_escserial.c](https://github.com/rotorflight/rotorflight-firmware/blob/7b54df9e1f34f0bbc825cbfe7df17a0a59cbd032/src/main/drivers/serial_escserial.c)
  verlangt einen konfigurierten zusätzlichen ESCSERIAL-Timerpin und lehnt
  N-Channel-RX ab; Blob48a4e93133815bf735ff0f001a5962b1a37e743a.

Das RF007-Target weist keinen ESCSERIAL-Timerpin zu. Nicht ungeprüft einen
Servo-, RPM-, Receiver- oder Port-A/C-Timer dafür umwidmen. Der bestehende
KISS-Pfad ist TX-only und der Passthrough blockierend; er ist kein verwendbarer
bidirektionaler MSP-Adapter. TIM1-/DMA-Abschaltung und Wiederherstellung müssen
mit dem tatsächlichen DShot-Ausgabepfad abgestimmt werden.

## ESCape32-Schema 17.2 / 17.3

- 17.2: [734097d2825deb49328342335e931ec18ce5823d](https://github.com/neoxic/ESCape32/tree/734097d2825deb49328342335e931ec18ce5823d).
- 17.3, offizieller rel17-Tag:
  [1d718c143380c3eb7e6581478990e1496632e5b8](https://github.com/neoxic/ESCape32/tree/1d718c143380c3eb7e6581478990e1496632e5b8).
- src/prog.c mit CFG_MAP und execcmd ist byteidentisch:
  Blob21885a08a0d0d1e6c022e8db91d1685a24589127.
- src/util.c mit checkcfg ist byteidentisch:
  Blobd450d1f4a9a569701914ec327666b05111d38d43.
- common.h und defs.h sind identisch. main.c ändert nur REVPATCH2 nach3.
  io.c ändert DShot-Framingschutz, keine CLI-Kommandos.

Schema1 unterstützt deshalb genau17.2 und17.3. Alle neun Parameter bleiben
identisch; keine Wirelayout-, Command-ID- oder Contractversionsänderung nötig:

| Key | Rohbereich | Anzeige / Besonderheit |
| --- | --- | --- |
| revdir | 0..1 | aus/an |
| timing | 1..31 | Rohwert mal15/16 Grad |
| freq_min | 16..48 | kHz |
| freq_max | 16..96 | kHz; checkcfg hebt auf mindestens freq_min an |
| duty_spup | 1..100 | Prozent |
| duty_ramp | 0..100 | kERPM |
| duty_rate | 1..100 | Rohwert mal0,1 Prozent/ms |
| prot_temp | 0..140 | Grad Celsius; 0 aus, sonst auf60..140 normalisiert |
| prot_curr | 0..999 | Ampere; ohne passenden Stromsensor auf0 normalisiert |

Unbekannte Revisionen, ausdrücklich17.4 und16.3, bleiben identity-only Schema0;
GET, SET und SAVE werden abgelehnt. Vollständige Identität vor jeder Session
lesen. Die frühere Beschreibung des master72004eff-Snapshots als17.2 war falsch:
dieser Stand liegt bereits nach17.3; die Parameterdefinitionen waren identisch.

## Implementierungsfolge für den physischen Adapter

1. Tatsächliche FC-Resources, Timer/DMA-Belegung, DShot-Modus und Boardrevision
   erfassen. Elektrischen Signalweg PA09 zum ESC-Stecker über Schaltplan oder
   Messung prüfen: Richtungspuffer, Pegel, Pulls und Eingangsschutz.
2. Einen nicht blockierenden 38400-8N1-Halbduplex-Adapter entwerfen. MCU-seitige
   Capture-Fähigkeit reicht allein nicht. Vorhandene Timer-/DMA-Ownership und
   Interrupt-Prioritäten prüfen; konfliktfreien Zeitgeber begründet auswählen.
3. Exklusiven Arming-Inhibit setzen, aktive Overrides ablehnen, DShot kontrolliert
   stoppen. Keine alten Overrides nach Verlassen wieder aktivieren. Erwerb muss
   bei ungeeigneter Konfiguration ohne Pinänderung scheitern.
4. CLI-Eintritt mit dokumentiertem Zwei-Sekunden-High-Fenster beim ESC-Startup
   implementieren. FC muss dabei versorgt bleiben; erforderliche Versorgung und
   gemeinsames GND am Bench dokumentieren. Keine automatische Stromschaltung
   oder Rückspeisungsfreiheit unterstellen.
5. Begrenzte RX/TX-Verarbeitung, Flush alter Bytes und Abmeldung alter Callbacks
   bei acquire/release nachweisen. Erst info und GET, dann SET mit Readback,
   erst zuletzt explizites SAVE prüfen.
6. Timer, DMA, Pins und Telemetrie geordnet wiederherstellen. Aktueller Core hält
   nach release(false) den Inhibit bis zum FC-Neustart; ein alleiniger ESC-
   Power-Cycle genügt derzeit nicht. Vor Hardwarefreigabe einen ausdrücklich
   verifizierten Rückweg aus RESTART_REQUIRED ergänzen, ohne Delay-Heuristik.
7. Validated-target-Maske und Schreibfähigkeit erst nach konkretem Bench-Nachweis
   erweitern. Das Boardprofil selbst aktiviert keine Capability und keinen Port.

## Erforderliche Bench-Abnahme

- FC-/ESC-Target, Revisionen, Pinbelegung, Stromsequenz und Messaufbau festhalten.
- CLI-info und mindestens einen Parameter über PA09 bidirektional lesen.
- ESC17.3 ändern, normalisierten Wert lesen, genau einmal speichern und nach
  echtem ESC-Neustart auf Persistenz prüfen. prot_temp-/Frequenzabhängigkeiten
  und gegebenenfalls fehlenden Stromsensor berücksichtigen.
- Linkverlust/Owner-Timeout während Eintritt, SET und SAVE, beschädigte Replies,
  späte Retries, Abbruch und gesperrtes Arming prüfen.
- Rückkehr zur normalen DShot-/RPM-/Telemetriefunktion messen; Servoausgänge und
  andere Timerbenutzer dürfen nicht unerwartet beeinflusst werden.
- STM32F7X2 mit Feature aus/an bauen. Host-/Simulatorerfolg ist keine
  Hardwarefreigabe; ungetestete Capability bleibt0.
