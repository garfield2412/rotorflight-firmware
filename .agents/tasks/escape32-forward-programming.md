# Agentenauftrag: ESCape32-Bridge in Rotorflight-Firmware

Status: experimenteller Integrationsdraft mit implementiertem portablem
Sessioncore, CLI-Parser und optionaler MSP-Anbindung. Contract-Version1 und
Testvektoren liegen vor; ein validierter physischer Hardwareadapter fehlt noch.
Die Aufgabenbeschreibung liegt unter .agents/tasks/, da AGENTS.md neue technische
Dokumente im lokalen docs/-Verzeichnis ausschließt.

## Ziel und Arbeitsbereich

Implementiere die FC-Seite des
[ESCape32-Drafts](https://github.com/garfield2412/rotorflight-lua-ethos-suite/blob/draft/escape32-forward-programming/docs/reference/escape32-forward-programming-draft.md).
Gegenstück: [Suite-Auftrag](https://github.com/garfield2412/rotorflight-lua-ethos-suite/blob/draft/escape32-forward-programming/docs/reference/escape32/suite-agent.md).

Zielrepository ist garfield2412/rotorflight-firmware.
Dieser Auftrag liegt im Branch draft/escape32-forward-programming, ausgehend von
master (7b54df9e1f34f0bbc825cbfe7df17a0a59cbd032). Verwende für die Umsetzung diesen Branch oder
einen davon abgeleiteten Arbeitsbranch. Lies dessen AGENTS.md und
pfadspezifische Regeln vor Änderungen. Der Entwurf ist kein Auftrag, direkt
auf upstream master zu schreiben.

Liefere eine begrenzte, asynchrone Bridge zwischen MSP und ESCape32-CLI auf einer
ausgewählten Motor-Signalleitung. Der erste Meilenstein ist ein bidirektionaler
Transportnachweis, anschließend Lesen, erst danach Schreiben und Speichern.
Firmware-Flashing und Änderungen an ESCape32 selbst gehören nicht zum Umfang.

## 1. Machbarkeit und aktuelle Quellen prüfen

Lies die aktuellen Implementierungen von:
- src/main/drivers/serial_escserial.c und zugehörigem Header;
- Motor-Pin-/Timer-/DMA-Ownership und DShot-Treiber;
- src/main/sensors/esc_sensor.c und esc_sensor.h;
- src/main/msp/msp.c und MSP-Protokollheader;
- Arming-Inhibits, Motor-Overrides und Scheduler;
- ESCape32 src/io.c und src/prog.c beim unterstützten Revisionstag.

Prüfe insbesondere, ob das konkrete FC-Motorpad 38400 Baud, 8N1,
bidirektionalen Halbduplexbetrieb einschließlich RX ermöglicht. Vorhandenes
KISS-Passthrough ist kein Nachweis für einen drahtlosen MSP-Rückkanal.
Bestehende AM32-Erkennung oder EEPROM-Adressen nicht auf ESCape32 übertragen.

Baue einen Transportadapter mit Test-Double, falls Hardware fehlt. Kennzeichne
den Hardwarepfad dann als unbestätigt; behaupte keinen erfolgreichen Bench-Test.
Eine FC-/Board-/ESC-Kombination ohne nachgewiesenen RX-Pfad darf keine
Schreibfähigkeit als unterstützt melden.

## 2. Gemeinsamen MSP-Vertrag zuerst liefern

Erstelle vor der Suite-Geräteanbindung einen versionierten Vertragsentwurf,
übergebe ihn dem Suite-Agenten und prüfe ihn gegen dessen Queue-/Funktransport.
Er muss mindestens spezifizieren:
- CAPS ohne Motorpin-Änderung; unterstützte Ziele/Backends/Schreibfähigkeiten;
- OPEN, STATUS, GET, SET, SAVE, CLOSE mit exakten Request-/Reply-Layouts;
- IDs, Little-/Big-Endian je Feld, Längen und maximale Transfergrößen;
- Sessiontoken, Generation, Request-IDs, Owner-/Busy-Verhalten;
- Parametertypen, skalierte Ganzzahlen, Grenzen und Schema-/ESC-Version;
- Zustandsübergänge, Ergebnisabruf, Zeitlimits und Retry-/Deduplizierungsregeln;
- unsupported, busy, invalid-session, power-cycle-needed, timeout,
  protocol-error, normalized-value, outcome-unknown, verification-failed;
- Verhalten bei unbekannten Operationen, Versionen und fehlerhaften Längen.

Ergänze bytegenaue gemeinsame Testvektoren für normale und fehlerhafte Pakete.
Halte den Vertrag im Reviewmaterial versioniert; die Suite bekommt dieselben
Vektoren. Beachte die Regel des Firmware-Repositories: neue Internals-Dokumente
gehören in rotorflight-documentation, nicht in ein neues lokales docs/-Dokument.

Prüfe belegte und in offenen Änderungen beanspruchte MSP-IDs. Benenne lokale
experimentelle IDs ausdrücklich als solche; keine Kollision und keine
unabgestimmte produktive Zuteilung. Die Anwendungs-Contract-Version ist vom
globalen MSP-API-Versionsstand getrennt. Gemäß AGENTS.md keine MSP-API- oder
Firmware-Versionsnummer durch den Agenten erhöhen.

## 3. Exklusive Session und Transport

Implementiere eine nicht blockierende Zustandsmaschine:
IDLE, ACQUIRE, WAIT_ESC_POWER, IDENTIFY, READY, READ/SET/SAVE,
VERIFY, ERROR, CLOSING, RELEASE und gegebenenfalls RESTART_REQUIRED.

1. OPEN nur disarmed und ohne aktive Motor-Overrides akzeptieren. Ziel gegen
   reale Motorressourcen prüfen; exklusiven Besitzer und Arming-Inhibit setzen.
   Mit Vierweg-Programmierung und anderen Pinbesitzern kollisionsfrei arbeiten.
2. Relevante Timer-, Pin-, DMA- und Telemetriezustände kontrolliert sichern und
   stoppen. Nur das ausgewählte ESC ansprechen; Auswirkungen auf gemeinsam
   genutzte Timer berücksichtigen und gegebenenfalls die Kombination ablehnen.
3. CLI-Eintritt gemäß dokumentiertem Zwei-Sekunden-High-Fenster beim
   ESC-Einschalten ermöglichen. FC muss währenddessen versorgt bleiben.
   WAIT_ESC_POWER an die Suite melden; keine automatische Stromschaltung
   unterstellen. Elektrische Pegel und Halbduplex-Turnaround am Bench prüfen.
4. Mit info die ESCape32-Identität und Revision prüfen. Unbekannte Identitäten
   nicht beschreiben; unbekannte Revisionen höchstens begrenzt lesend behandeln.
5. MSP-Handler starten oder pollen Arbeit. Kein Busy-Wait für Sekunden,
   kein blockierendes Lesen im MSP-Callback. Fristen im Scheduler abarbeiten.
   Begrenzte statische Puffer und explizite Längen verwenden.

## 4. Parser, Einstellungen und Schreibsemantik

CLI-Kommandos enden mit LF. Antworten erst nach vollständigem OK oder ERROR als
abgeschlossen werten. Split-Reads, mehrere Zeilen, fehlende Terminatoren,
Overflow, falsche Keys und ungültige Zahlen behandeln. Kein unbeschränktes
show in den bestehenden Parameterblock kopieren.

Erlaube ausschließlich strukturierte Whitelist-Operationen. Es darf keine
freie Textkonsole und keinen durchschleifbaren throt-Befehl geben.
Erste Kandidaten: revdir, timing, freq_min, freq_max, duty_spup, duty_ramp,
duty_rate, prot_temp, prot_curr. Schema, Wertebereiche, Einheiten und
Abhängigkeiten anhand der tatsächlich unterstützten ESC-Revision definieren.
Zusätzliche Identitäts-/Metadatenausgaben strikt begrenzen.

SET verändert zunächst ESC-RAM. Danach betroffenen Key und bekannte abhängige
Werte lesen; tatsächliche Normalisierung durch checkcfg zurückmelden.
SAVE ist ein expliziter separater Auftrag. Ein verlorenes ACK darf keinen
zweiten Flash-Save auslösen: Token und Request-ID deduplizieren und Ergebnisse
für ein definiertes begrenztes Fenster halten. Gleiche ID mit anderer Nutzlast
ablehnen. Bei Timeout nach Versand ist der Ausgang unbekannt.

Readback unmittelbar nach SAVE belegt den aktuellen Wert, nicht zwingend
Persistenz über einen Neustart. Das erst nach einem echten Power-Cycle und
erneutem Identitäts-/Wertevergleich bestätigen.

## 5. Abbruch und Wiederherstellung

Ein FC-seitiger Owner-Watchdog muss auch bei Funkverlust oder abgestürzter Suite
wirken. Explizites CLOSE, Fristablauf und sämtliche Fehler laufen durch denselben
Cleanup-Pfad. Keine Callback-Nutzung nach Freigabe; keine alte Session darf ein
neu gewähltes Ziel verändern.

Pin-/Timer-/DMA-/Telemetrie-Ownership geordnet zurückgeben. Bei einem im CLI
verbleibenden ESC ist Freigabe der Ressourcen nicht mit Motorbereitschaft
gleichzusetzen. Arming-Inhibit bis zu belegter sicherer Reinitialisierung bzw.
erforderlichem Neustart erhalten. ESC-Neustart nicht anhand eines bloßen
MSP-ACK oder eines festen Delays als erfolgt erklären. Alte Overrides niemals
automatisch wieder aktivieren.

Experimentelle Unterstützung standardmäßig aus lassen, solange Transport und
Recovery auf Zielhardware nicht nachgewiesen sind. Bestehende ESC-Backends und
MSP-Clients müssen unverändert weiterarbeiten.

## 6. Prüfen und liefern

Gezielte Tests mit Fake-Uhr und Fake-Serial für:
- MSP-Längenkontrolle, Versionen, Typen, Grenzen und Vertragsvektoren;
- fragmentierte CLI-Antworten, ERROR, Timeout, Overflow und Zahlenüberlauf;
- Arming/Overrides, Busy, ungültiges Ziel, widersprüchliche Pin-Ownership;
- doppelte Aufträge, andere Nutzlast bei gleicher ID und veraltete Tokens;
- Normalisierung, Readback-Abweichung und unbekannten Schreibausgang;
- Cleanup aus jedem aktiven Zustand, Owner-Timeout und Neustartpflicht;
- fehlende Build-Features sowie Regression bestehender ESC-Programmierung.

Baue gemäß aktueller AGENTS.md bevorzugt STM32F7X2 sowie die relevante Ziel-
MCU-Familie mit aktivierter und deaktivierter Funktion. Prüfe Flash-/RAM-Bedarf.
Ändere keine globalen Releaseversionen. Falls persistente PG-Strukturen geändert
werden müssen, befolge deren PG-Versionierungsregel; vermeide unnötige neue PGs.

Bench-Abnahme, soweit Hardware verfügbar: info/get lesen, einen Parameter
ändern und lesen, genau einmal speichern, ESC power-cyclen, Persistenz prüfen.
Mit zwei ESCs nachweisen, dass das andere unverändert bleibt. Linkverlust in
Eintritt, SET und SAVE sowie Rückkehr zur normalen DShot-/Telemetriefunktion
prüfen. FC-Target, Boardkonfiguration, ESC-Target/-Revision, Pegel, Verkabelung
und separate FC-Versorgung protokollieren.

Liefere reviewfähige Commits und Draft-PR bzw. Vergleich mit What,
Compatibility und Testing; verlinke Suite-Companion, Vertragsversion und
Testvektoren. Liste reale Builds/Tests und offene Bench-Nachweise getrennt.
Melde erforderliche Benutzer-/Internals-Dokumentation im dafür vorgesehenen
Dokumentationsrepository. Keine Hardwarefreigabe allein aus Parser-Unit-Tests.

## Implementierungsstand dieses Drafts (2026-10-29)

- Implementiert: portabler Sessioncore und begrenzter CLI-Parser unter
  src/main/io/escape32.{c,h}; FakeSerial-/FakeTime-Tests, gemeinsames JSON-Fixture.
- MSP-Anbindung: fork-lokales experimentelles Envelope 0x5FE0, Contract-Version1.
  Nur mit USE_ESCAPE32_FORWARD_PROGRAMMING aktiv; kein Target definiert dieses
  Flag standardmäßig. Keine globale API-/Releaseversion geändert.
- Scheduler: taskMain tickt den Core unabhängig vom MSP-Heartbeat.
- Physischer Adapter fehlt absichtlich. Auch mit aktiviertem Flag meldet CAPS
  nur corePresent, backend0, targetMask0 und keine Read-/Write-Capabilities.
  OPEN wird ohne Pinänderung als unsupported beantwortet.
- Bekannte ESC17.2-Identität erhält Schema1; unbekannte Revisionen sind nur
  identifizierbar, GET/SET/SAVE werden abgewiesen.
- SET liest alle neun freigegebenen Keys erneut und prüft die Frequenzrelation.
  SAVE bestätigt nur CLI-Abschluss, keine Persistenz über Stromunterbrechung.

### Konkrete Grenze des vorhandenen Hardwarepfads

src/main/drivers/serial_escserial.c ist kein direkt nutzbarer Adapter:
PROTOCOL_KISS initialisiert nur TX, escEnablePassthrough liest in mode2 kein RX,
und processTxStateBL schaltet RX nur bei BLHELI/CASTLE um. openEscSerial und
closeEscSerial sind statisch; der bestehende Passthrough ist blockierend.
Es wird zusätzlich ein ESCSERIAL-Timer benötigt; N-Channel-Motorpads werden für
RX abgewiesen. HAL TIM_DeInit ist ein Stub. Ein geprüfter Wiederherstellungspfad
für Motor-Timer/DMA und Rückkehr aus der ESC-CLI ist damit nicht belegt.

Nächster Hardwareauftrag: einen nicht blockierenden, bidirektionalen 38400-8N1-
Adapter mit expliziter Timer-/DMA-Ownership, dokumentierter Stromsequenz, Flush
alter RX-/TX-Daten und nachgewiesener Recovery entwickeln. acquire/release
müssen alte Bytes und Callbacks entfernen. Bei fehlender Recovery muss der
Arming-Inhibit bestehen bleiben. CAPS erst nach Board-/ESC-Bench-Nachweis für
Ziele erweitern; keine Aktivierung durch bloßes Setzen eines Build-Flags.

Aktuelle Restart-Latch-Einschränkung: liefert release false, bleibt inhibited
gesetzt und die Session wird inaktiv. Der Core bietet danach keinen erneuten
Recovery-Poll; Init lehnt den Reset ab und OPEN bleibt gesperrt. Ein alleiniger
ESC-Power-Cycle bei weiter versorgtem FC hebt diese Sperre deshalb nicht auf.
Im aktuellen Draft ist ein FC-Neustart erforderlich. Der Hardware-Folgeauftrag
muss einen explizit geprüften Rückweg aus RESTART_REQUIRED ergänzen; ein Delay
oder die bloße Annahme eines ESC-Neustarts darf den Inhibit nicht entfernen.

### Ausgeführte Prüfung / offene Abnahme

Host: sh src/test/run_escape32_tests.sh mit GCC C11, -Wall -Wextra -Werror und
-pedantic erfolgreich; zusätzlich AddressSanitizer und UndefinedBehaviorSanitizer
erfolgreich mit ASAN_OPTIONS=detect_leaks=0. LeakSanitizer ist wegen ptrace in der
Arbeitsumgebung nicht ausführbar. Der Core verwendet keine Heap-Allokation.
Eine separate GitHub-Actions-Workflowdatei führt den portablen Harness aus.
Sie enthält außerdem eine STM32F7X2-ARM-Buildmatrix mit deaktivierter und
aktivierter USE_ESCAPE32_FORWARD_PROGRAMMING-Option. Der Workflow ist vorbereitet;
ein tatsächlicher CI-Erfolg muss nach dem Branch-Push separat geprüft werden.

STM32F7X2-Builds mit Feature aus/an sind inzwischen in GitHub Actions bestanden
(siehe nachfolgenden Nachweis). Echte Motorpin-Kommunikation, CLI-Eintritt/Recovery,
weitere MCU-Familien und Funk-/ESC-Benchtests stehen aus. Shell-Netzwerk scheitert
am Proxy-Verbindungsaufbau; ARM-Toolchain und vollständiger Checkout fehlen.
Der native GCC-Test ersetzt keinen STM32F7X2-Build. Dieser Branch ist ein
experimenteller Integrationsdraft, noch keine funktionierende ESC-Programmierung.


### Unabhängige Koordinator-Prüfung

Code-Commit: [9c6273aa0afc61277a9cfeca96bb2b82740a1267](https://github.com/garfield2412/rotorflight-firmware/commit/9c6273aa0afc61277a9cfeca96bb2b82740a1267).
[GitHub Actions Run 40829033784](https://github.com/garfield2412/rotorflight-firmware/actions/runs/40829033784):
alle drei Jobs erfolgreich: nativer Core mit Sanitizern sowie vollständiger
STM32F7X2-Build mit deaktiviertem und aktiviertem USE_ESCAPE32_FORWARD_PROGRAMMING.
Keine globale MSP-API-/Firmware-Releaseversion wurde dafür geändert.

GNU size für diesen STM32F7X2-Build (Bytes):

| Feature | text | data | bss |
| --- | ---: | ---: | ---: |
| aus | 444762 | 11152 | 121444 |
| an | 448634 | 11152 | 121668 |

Differenz in diesem Build: +3872 Bytes text und +224 Bytes bss. Dies ist eine
Linker-/Buildmessung, keine Laufzeit-Stack- oder Hardware-Sicherheitsfreigabe.

Zusätzlich besteht ein echter Cross-Repository-Test des C-Kerns gegen den
Lua-Codec und die Lua-Sessionsteuerung, mit injiziertem Fake-Serial-Adapter,
Normalisierung, einmaligem SAVE, Keepalive und verlorenen SAVE-/CLOSE-Antworten.
Der Test liegt in der Suite unter bin/escape32/verify_bridge.py.
