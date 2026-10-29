# Agentenauftrag: Wi-Fi-Link-Transport auf RF007 portieren

Status: nächster Implementierungsschritt, noch nicht umgesetzt. Ziel ist die
reale serielle Verbindung vom RF007 zum ESC; Protokollkern, Suite-Seite und
Fake-Serial-Tests existieren bereits.

## Quellen und Arbeitsbereich

Arbeite im Fork garfield2412/rotorflight-firmware, Branch
draft/escape32-forward-programming oder einem davon abgeleiteten Arbeitsbranch.
Lies AGENTS.md sowie [Grundauftrag](escape32-forward-programming.md) und
[RF007-Profil](escape32-rf007.md). Neue technische Referenzdokumente gehören gemäß
AGENTS.md nicht in ein neues lokales docs/-Verzeichnis.

Die verbindliche [Wi-Fi-Link-Referenzauswertung](https://github.com/garfield2412/rotorflight-lua-ethos-suite/blob/draft/escape32-forward-programming/docs/reference/escape32/wifi-link-reference.md) enthält unveränderliche
Quelllinks, Funktionen und die Abgrenzung des nachgewiesenen ESC-Protokolls.
Referenzcommit: 40af21ccfafd9cb8d1c9b951d57b54f3e36bd21f.
Lies insbesondere main/main.c recvbuf, sendbuf, wshandler und UART-Setup
sowie main/root.html Parameterlesen, Parameteränderung und Speichern.

Die Referenz zeigt tatsächliche UART-RX/TX- und CLI-Befehlsverarbeitung. Unser
noch offener Teil ist die Portierung dieser Verbindung einschließlich
Motorressourcen und Recovery auf den STM32F722. Es ist kein neues
ESC-Parameterprotokoll zu erfinden.

## Feste Randbedingungen

- Nutzer: Vantac RF007, DShot am ESC-Throttle-Ausgang, ESCape32 17.3.
- Default-MOTOR1: PA09, TIM1_CH2 AF1, DMA2 Stream6 Channel0. Tatsächliche
  Laufzeit-Resources, Boardrevision und elektrischer Rückkanal sind zu prüfen.
- ESCape32 17.2 und 17.3 besitzen die belegte identische CLI-/Parameterdefinition.
  Schema 1 akzeptiert exakt diese beiden; andere Revisionen bleiben identity-only.
- MSP2 0x5FE0/Contract v1 und die neun Keys bleiben erhalten. Die gemeinsamen
  Fixtures und der Suite-Companion-Pin dürfen nicht auseinanderlaufen.
- Kein Wi-Fi-/HTTP-/WebSocket-Stack im FC und keine neue Funkstrecke zur Suite.
  Die Suite spricht weiter MSP; der FC implementiert den seriellen ESC-Adapter.
- Draft-UI bleibt auf ausdrücklichen Nutzerwunsch englisch. Keine
  Übersetzungsdateien oder globale MSP-/Firmware-Releaseversionen ändern.
- Kein Motorlauf, BEC-/Moduswechsel, Bootloaderupdate oder freier CLI-Passthrough
  als Nebenwirkung der Verbindungsherstellung.

## 1. Referenz auf den Adaptervertrag abbilden

| Adapterbestandteil | Zu implementierendes Verhalten |
| --- | --- |
| safe | Disarmed, keine aktiven Overrides und keine kollidierenden Ressourcen |
| acquire | Exklusiver Erwerb; DShot kontrolliert stoppen, alte Bytes/Callbacks entfernen, definierter Startzustand |
| ready | CLI-Eintritt und Stromsequenz tatsächlich abgeschlossen; bloßer Ablauf eines Timers beweist keine ESC-Identität |
| write | Begrenzten TX-Auftrag ohne Warten annehmen; Umschaltung nach physischem TX-Ende berücksichtigen |
| read | Verfügbare RX-Bytes liefern, andernfalls sofort -1; kein blockierender Read |
| release | Ressourcen freigeben/wiederherstellen; true nur nach positiv belegter normaler Motorbereitschaft |
| inhibit | Arming während Besitz und ungeklärter Recovery zuverlässig sperren |

Die tatsächlichen Signaturen stehen in src/main/io/escape32.h. Erweiterungen
dieser internen Schnittstelle begründen und gemeinsam mit Tests reviewbar halten.

Die ESP32-Referenz wartet in recvbuf auf FreeRTOS-Ereignisse und liest blockierend.
Das ist als Verhaltensvorlage zu verwenden, nicht unverändert in FC-Callbacks
zu kopieren. Ebenso keinen 1200-Byte-WebSocketpuffer, Browser-Requestecho oder
Bootloaderzweig in den FC-Parser übernehmen. LF-Datenzeilen und vollständige
OK/ERROR-Terminatoren inkrementell verarbeiten. Aktive Teilantworten nicht flushen.

## 2. Transportentscheidung und erste echte RX-Verbindung

1. Ressourcen-/Timer-/DMA-Map und Stromversorgung erfassen. ESC-Hardwaretarget
   und FC-Versorgung während ESC-Neustart sind noch offen; fehlende Angaben
   konkret dokumentieren, keine elektrische Durchgängigkeit unterstellen.
2. Elektrischen PA09-Steckerpfad und TX/RX-Richtungswechsel anhand Schaltplan
   oder Messung klären. Ein ESP32 mit getrennten RX-/TX-Pins ist kein Nachweis
   für einen rücklesbaren RF007-Motorausgang.
3. Transport begründet auswählen: timerbasierter Halbduplex oder geeigneter
   Hardware-UART-Pfad. Der bestehende ESCSERIAL-Pfad ist TX-only im KISS-Modus,
   benötigt einen zusätzlichen Timerpin und hat unvollständige Recovery.
   Ein UART-Kandidat auf PA9/USART1_TX kollidiert möglicherweise mit UART1
   PB6/PB7; bestehende Belegung zuerst prüfen. Keine automatische Umwidmung.
4. Nicht blockierenden Treiber hinter dem experimentellen Adaptervertrag
   implementieren und mit einem isolierten Hardwareabstraktions-Testdouble
   prüfen. Zielmasken bis zum konkreten Bench-Nachweis nicht freischalten.
5. DShot-Besitz abgeben und dokumentiertes Zwei-Sekunden-High-Fenster beim
   ESC-Einschalten erzeugen, während der FC versorgt bleibt. Keine automatische
   ESC-Stromschaltung oder CLI-Eintritt aus laufendem DShot behaupten.
6. Über den tatsächlichen RF007-Stecker info und einen whitelisted GET lesen.
   Roh-TX/RX, Baudrate, Pegel, Fristen, Turnaround und Identität protokollieren.
   Optional zuvor dieselben statischen Werte per Wi-Fi-Link lesen; Adapter
   getrennt betreiben, keine gleichzeitig aktiven Signal-Treiber.

## 3. Schreiben und Recovery

Nach erfolgreichem echten Lesen erst SET mit normalisiertem Wert und
abhängigem Readback, anschließend explizites einmaliges SAVE prüfen.
Ein fehlendes Radio-ACK darf keinen zweiten Flash-Save erzeugen. Nach einem
echten ESC-Neustart Identität und Persistenz erneut prüfen.

DShot-/Timer-/DMA-/Pin- und Telemetriezustände geordnet wiederherstellen;
Servoausgänge und andere Timerbenutzer berücksichtigen. release true darf
weder auf einem CLI-OK noch auf einer festen Wartezeit beruhen. Alte
Motor-Overrides nicht automatisch wieder aktivieren.

Aktuell führt release false zu einem FC-seitigen Restart-Latch ohne späteren
Recovery-Poll. ESC-only Power-Cycle löst ihn nicht. Für eine Rückkehr ohne
FC-Neustart ist ein expliziter, überprüfbarer Recoverypfad im Adapter/Core nötig;
dessen Zustands- und Arming-Verhalten vor Implementierung festhalten.

## 4. Abnahme und Übergabe

- Hosttests: getrennte/verspätete Zeilen und Terminatoren, RX-Overflow, Timeout,
  Fehler nach SET/SAVE, Flush-Grenzen und Cleanup aus jeder Phase.
- Ownershiptests: ungeeignetes Ziel, aktive Overrides, Timer/UART belegt,
  fehlgeschlagener Erwerb ohne Pinänderung, genau einmalige Freigabe.
- Bestehende Coretests und realer C/Lua-Cross-Test bleiben grün.
- STM32F7X2 mit Feature aus/an bauen; keine globale Versionsanhebung.
- Bench: tatsächlicher RF007-RX, SET/Readback, SAVE/Persistenz sowie Rückkehr
  zu DShot/RPM/Telemetrie, Funkverlust und Owner-Timeout nachweisen.
- Capability-Flags nur für nachgewiesene Kombinationen setzen; Kompilierbarkeit
  oder Fake-Serial-Erfolg bleibt ausdrücklich von Hardwareabnahme getrennt.

Liefere den Treiberdiff, begründete Transportentscheidung, reproduzierbare
Test-/Messschritte und den Companion-Stand. Ein weiterer reiner Mock oder ein
bloßes CAPS-Flag ersetzt diesen physischen Implementierungsschritt nicht.
