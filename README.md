![GitHub Logo](http://www.heise.de/make/icons/make_logo.png)

Maker Media GmbH

***

# LED-Achterbahn

**Aus einem LED-Streifen, etwas Physik und einem ESP32 entsteht eine LED-Achterbahn, bei der das Licht wie ein echter Wagen durch Loopings und Kurven rauscht. Und das zugehörige Baukastensystem macht individuelle Streckendesigns zum Kinderspiel – ganz ohne Kleben, Schrauben oder manuelle Code-Anpassungen.**

![Aufmacherbild aus dem Heft](./doc/ledAchterbahn_github.JPG)

Hier findet ihr die Daten für den 3D-Druck und den Arduino-Code.

Der vollständige Artikel zum Projekt steht in der **[Make-Ausgabe 2/26](https://www.heise.de/select/make/2026/2)**.

---
# Infos aus dem FORK

## geplante Features
- Anzeige von G-Käften unter Telemetrie
- Anbindung von DFPlayer für AudioEffekte

# Rollercoaster LED‑Simulation  
*Interaktive Achterbahn‑Physik auf einem ESP32 mit OLED‑Display und bis zu 1000 LEDs.*

![Platform](https://img.shields.io/badge/Platform-ESP32-blue)
![Display](https://img.shields.io/badge/Display-SH1106-lightgrey)
![LEDs](https://img.shields.io/badge/LEDs-WS2812B-green)
![Status](https://img.shields.io/badge/Status-Active-success)

---

# 📖 Inhalt
1. [Überblick](#überblick)
2. [FPS‑Hinweise & Performance](#fps-hinweise--performance-tabelle)
3. [Features](#features)
4. [Hardware](#hardware)
5. [Installation](#installation)
6. [Bedienung & Menüsteuerung](#bedienung--menüsteuerung)
7. [Streckeneditor](#streckeneditor)
8. [Telemetrie & Kirmes-Modus](#telemetrie--kirmes-modus)
9. [Physik‑System](#physik-system)
10. [Dateisystem & Slots](#dateisystem--slots)
11. [Code‑Architektur](#code-architektur)
12. [Detaillierte Code‑Erklärung](#-detaillierte-code-erklärung)
13. [Lizenz](#lizenz)

---

# Überblick
- **rollercoaster_finale** ist **unverändert**.
- **Meine Änderungen** sind vollständig in **rollercoaster_extra** eingeflossen. Dieses Fork erweitert die Achterbahn um detailliertere Einstellungen, eine saubere Telemetrie mit Ziel-Logik, einen Kirmes-Modus mit dynamischen Sprüchen und eine optimierte Menüführung.

---

# FPS‑Hinweise & Performance‑Tabelle
Da der ESP32 gleichzeitig die Physik für bis zu 1000 LEDs berechnet und ein OLED-Display via I2C ansteuert, ist das richtige Timing entscheidend. Die folgende Tabelle zeigt, wie sich die Anzahl der LEDs auf die Datenraten, die maximal sinnvollen OLED‑FPS und das subjektive Fahrgefühl auswirken.

| **Anzahl LEDs** | **Zeit LED‑Daten** | **Zeit OLED‑Daten** | **Max. empfohlene OLED‑FPS** | **Effekt auf die LED‑Bahn** |
|----------------:|-------------------:|---------------------:|------------------------------:|------------------------------|
| 100  | 3 ms  | ~25 ms | 20–30 | Absolut flüssig, Ruckler nicht wahrnehmbar |
| 200  | 6 ms  | ~25 ms | 15–20 | Absolut flüssig |
| 300  | 9 ms  | ~25 ms | 15–20 | Sehr flüssig |
| 400  | 12 ms | ~25 ms | 12–15 | Sehr flüssig |
| 500  | 15 ms | ~25 ms | 10–15 | Flüssig |
| 600  | 18 ms | ~25 ms | 10–12 | Minimales Mikroruckeln messbar, kaum sichtbar |
| 700  | 21 ms | ~25 ms | 8–10  | Sehr stabiles Fahrgefühl |
| 800  | 24 ms | ~25 ms | 8–10  | Stabiles Fahrgefühl |
| 900  | 27 ms | ~25 ms | 6–8   | Leichte Ruckler beim Display‑Update möglich |
| 1000 | 30 ms | ~25 ms | 5–8   | Guter Kompromiss aus flüssiger Bahn und Telemetrie |

*Tipp: Die Display-FPS lassen sich im "Optik"-Menü jederzeit anpassen. Standardwert ist 4 FPS.*

---

# Features
- LED‑Achterbahn mit realistisch berechneter Physik
- Bis zu **1000 LEDs**
- OLED‑Display mit Telemetrie (Aktiv/Ziel-Logik & Top-Speed Tracking)
- Vollständiger **Streckeneditor** direkt am Gerät mit Scroll-Menüs
- Lift, Brake, Booster, Loop, Helix, Steilkurve uvm.
- Zonen lassen sich temporär deaktivieren (Enabled/Disabled)
- Kirmes-Modus mit passenden Sprüchen zum jeweiligen Fahrabschnitt
- 5 Speicher‑Slots (LittleFS) inkl. Abwärtskompatibilität
- Farbcodierung nach Geschwindigkeit (Speed-Map)
- Bezier‑Interpolation für realistische Höhenprofile

---

# Hardware
- ESP32 DevKit (Empfohlen: ESP32-S3 für bessere FPU-Performance)
- SH1106 OLED 128×64 (I2C)
- WS2812B LED‑Strip
- Rotary Encoder mit Button (KY-040)
- 5V Netzteil (je nach LED‑Anzahl 5–20A)

---

# Installation
1. Repository klonen
2. Arduino IDE öffnen (ESP32 Boardverwalter muss installiert sein)
3. Bibliotheken installieren:
   - Adafruit GFX Library
   - Adafruit SH110X
   - Adafruit NeoPixel
   - RotaryEncoder (von Matthias Hertel)
   - LittleFS (ESP32)
4. Taktfrequenz in der IDE auf 240 MHz stellen (für maximale Physik-Performance).
5. Sketch hochladen.

---

# Bedienung & Menüsteuerung
Die komplette Steuerung erfolgt intuitiv über den **Drehencoder**:
- **Drehen:** Navigiert durch Listen oder ändert Werte.
- **Drücken:** Bestätigt eine Auswahl oder wechselt in die nächste Menüebene.

### Das Hauptmenü
- **ABSPIELEN:** Startet die Echtzeit-Simulation der aktuell geladenen Strecke.
- **OPTIK:** Hier lassen sich visuelle Parameter anpassen:
  - Wagen- und Bahnfarbe (inkl. Speed-Map)
  - Helligkeit für Wagen, Bahn und Zonen (Lift, Brake, Booster)
  - Wagenlänge (Anzahl der LEDs)
  - Zonen-Effekte (Aus, Statisch, Lauflicht/Pulsieren)
  - OLED FPS und Display-Modus (Telemetrie vs. Kirmes)
- **ELEMENTE:** Zugang zum Streckeneditor (Bearbeiten, Neu hinzufügen).
- **STRECKEN [Sx]:** Dateimanager. Zeigt den aktuell geladenen Slot (S1-S5) an.

*Hinweis: Längere Menüs (wie z.B. bei der Element-Bearbeitung oder Optik) verfügen über einen **dynamischen Scroll-Viewport**, sodass immer 6-7 Zeilen sichtbar sind und der Cursor flüssig mitläuft.*

---

# Streckeneditor
Der Editor ermöglicht es, die physikalischen Eigenschaften der Strecke direkt am LED-Band zu "zeichnen". Die Strecke besteht aus "Nodes" (Knotenpunkten).

1. **Startpunkt setzen:** Definiert die Position (LED) und die Starthöhe.
2. **Elemente hinzufügen:** Du navigierst die "grüne LED" an die gewünschte Position auf dem Streifen, stellst die Höhe ein und wählst die Art des Elements (Hill, Loop, Helix, etc.).
3. **Funktions-Zonen (Lift, Brake, Booster):** Diese speziellen Elemente benötigen weitere Parameter:
   - **Wert (Force):** m/s für Lift/Bremse oder Schubkraft für den Booster.
   - **Länge:** Wie viele LEDs lang diese Zone aktiv sein soll.
   - **Aktiv/Inaktiv:** Zonen können zu Testzwecken schnell an- oder ausgeschaltet werden, ohne sie löschen zu müssen.
4. **Endpunkt:** Schließt die Strecke ab. Das System berechnet automatisch die Bezier-Kurven und die Beschleunigungswerte für jedes Teilstück dazwischen.

---

# Telemetrie & Kirmes-Modus
Während der Fahrt liefert das OLED-Display Live-Daten. Es gibt zwei Darstellungsmodi, die im Optik-Menü umgestellt werden können.

### Modus 0: Telemetrie (Der Nerd-Modus)
Liefert exakte Daten zur Physik-Engine:
- **Pos:** Aktuelle LED-Position des Wagens / Gesamtlänge.
- **Spd m/s:** Aktuelle Geschwindigkeit in Metern pro Sekunde, inkl. der **(max xx.x)** Anzeige für den bisherigen Top-Speed des Runs.
- **Status:** Zeigt an, ob der Wagen frei fährt (FREIE FAHRT), gebremst wird, auf dem Lift hängt oder geboostet wird.
- **Ziele & Zonen:**
  - `AKTIV: Elem X (Typ)`: Wird angezeigt, solange sich der Wagen *physisch innerhalb* der Länge einer Zone (z.B. eines Lifts) befindet.
  - `ZIEL: Elem Y (Typ)`: Fährt der Wagen frei, zeigt das System das nächste vorausliegende Element an.
- **LED & H:** Genaue Position und Höhe des aktuellen/nächsten Elements.

### Modus 1: Jahrmarkt Live (Der Show-Modus)
Perfekt, wenn die Bahn fertig aufgebaut ist. Zeigt nur den aktuellen Speed (inkl. Max) und präsentiert einen zufällig gewählten, dynamischen Spruch passend zur aktuellen Streckensituation (z.B. *"Zischhh... Bremsen greifen!"* oder *"Kopfueber ins Glueck! Looping in Sicht!"*).

---

# Physik‑System
Das Kernstück der Simulation. Der Code unterteilt die Bezier-Interpolation der Strecke in winzige Segmente.
- **Beschleunigung:** Wird durch den Hangabtrieb ($a = g \cdot \sin(\alpha)$) in jedem Segment bestimmt.
- **Reibung & Luftwiderstand:** Wirken dynamisch gegen die Bewegungsrichtung, abhängig von der aktuellen Geschwindigkeit ($v^2$).
- **Funktionszonen:** Überschreiben die Physik. Ein Lift zwingt den Wagen auf eine konstante Geschwindigkeit, eine Bremse baut Energie ab, ein Booster addiert Beschleunigung.
- Die Berechnung erfolgt zeitbasiert (`dt = millis() - lastTime`), um die Geschwindigkeit unabhängig von der CPU-Auslastung stabil zu halten.

---

# Dateisystem & Slots
Die Strecken werden im Flash-Speicher des ESP32 via LittleFS abgelegt.
- Bis zu **5 Strecken** speicherbar (Slot 1 bis Slot 5).
- Speichert alle Nodes, Optik-Einstellungen, Helligkeiten und FPS.
- **Migration / Abwärtskompatibilität:** Der Code nutzt einen `FILE_MAGIC` Header. Wenn alte Strecken geladen werden (ohne Enabled-Parameter für Zonen), migriert der Code diese automatisch beim Lesen und setzt sie auf aktiv.
- Funktionen: Laden, Neu erstellen (löscht den RAM und beginnt bei LED 0), Neu berechnen (falls die Bezier-Kurven haken) und Löschen (entfernt die .bin Datei aus dem Flash).

---

# Code‑Architektur
- **State‑Machine (UI_STATE):** Über 40 Zustände steuern, was gerade auf dem Display gerendert wird und wie der Encoder reagiert (z.B. `ST_EDIT_NODE_FORCE` oder `ST_PLAY`).
- **Nodes (`Node`):** Die Stützpfeiler der Strecke. Enthalten Typ, Höhe und ggf. Spezialfunktionen.
- **Bezier‑Interpolation (`calculations`, `bezierCalculations`):** Generiert aus wenigen Nodes eine geschwungene, realistische Bahn.
- **Segment‑Physik (`segmentCalculations`):** Wandelt die Bezier-Punkte in physikalische Steigungen um.
- **Renderer:** Getrennte Ausgabe für LEDs (`moveLED`, `drawSetupLeds`, `drawColorSelection`) und das Display (vielfältige `draw...` Funktionen).

---

# 🧩 Detaillierte Code‑Erklärung

## Bibliotheken & Hardware‑Setup
Der Code nutzt:
- `Adafruit_SH1106G` → OLED‑Display (I2C)
- `Adafruit_NeoPixel` → LED‑Streifen (WS2812B)
- `RotaryEncoder` → Interrupt-basierte Encoder-Auswertung für präzises Drehen.
- `LittleFS` → Dateisystem
- `Wire` → I²C für Display

## Datenstrukturen

### Node
```cpp
struct Node {
  uint16_t pos;      // Position auf dem LED-Streifen
  uint16_t height;   // "Bauhöhe" des Elements
  uint8_t  type;     // 0=Start, 1=Scheitel, 2=Ende
  uint8_t  art;      // Welches Streckenelement (Hill, Loop, Lift...)
  float    force;    // Kraft/Zielgeschwindigkeit
  uint16_t length;   // Ausdehnung des Elements in LEDs
  uint8_t  enabled;  // 1 = Aktiviert, 0 = Übersprungen
};