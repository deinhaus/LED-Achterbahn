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
6. [Bedienung](#bedienung)
7. [Streckeneditor](#streckeneditor)
8. [Physik‑System](#physik-system)
9. [Dateisystem & Slots](#dateisystem--slots)
10. [Code‑Architektur](#code-architektur)
11. [Detaillierte Code‑Erklärung](#🧩-detaillierte-code-erklärung)
12. [Lizenz](#lizenz)

---

# Überblick
- **rollercoaster_finale** ist **unverändert**.
- **Meine Änderungen** sind vollständig in **rollercoaster_extra** eingeflossen.

---

# FPS‑Hinweise & Performance‑Tabelle
Die folgende Tabelle zeigt, wie sich die Anzahl der LEDs auf die Datenraten, die maximal sinnvollen OLED‑FPS und das subjektive Fahrgefühl auswirken.

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

---

# Features
- LED‑Achterbahn mit realistisch berechneter Physik
- Bis zu **1000 LEDs**
- OLED‑Display mit Telemetrie
- Vollständiger **Streckeneditor** direkt am Gerät
- Lift, Brake, Booster, Loop, Helix, Steilkurve uvm.
- 5 Speicher‑Slots (LittleFS)
- Einstellbare Display‑FPS (1–20 FPS)
- Farbcodierung nach Geschwindigkeit
- Bezier‑Interpolation für glatte Strecken

---

# Hardware
- ESP32 DevKit
- SH1106 OLED 128×64
- WS2812B LED‑Strip
- Rotary Encoder mit Button
- 5V Netzteil (je nach LED‑Anzahl 5–20A)

---

# Installation
1. Repository klonen
2. Arduino IDE öffnen
3. Bibliotheken installieren:
   - Adafruit_GFX
   - Adafruit_SH110X
   - Adafruit_NeoPixel
   - RotaryEncoder
   - LittleFS (ESP32)
4. Sketch hochladen
5. Gerät starten

---

# Bedienung
- **Drehencoder drehen** → Menü navigieren
- **Encoder drücken** → Auswahl bestätigen
- Menüs:
  - Optik (Farben, Helligkeiten, Display‑FPS)
  - Streckenelemente bearbeiten
  - Datei‑Slots laden/speichern/löschen
  - Simulation starten

---

# Streckeneditor
- Nodes einfügen, bearbeiten, löschen
- Elementtypen:
  - Hill
  - Loop
  - Helix
  - Steilkurve
  - Valley
  - Lift
  - Brake
  - Booster
- Automatische Neuberechnung der Strecke
- Bezier‑Interpolation für glatte Übergänge

---

# Physik‑System
- Geschwindigkeit abhängig von Höhe
- Reibung & Luftwiderstand
- Lift/Bremse/Booster beeinflussen die Bewegung
- Farbcodierung abhängig von Geschwindigkeit
- Segmentbasierte Physikberechnung

---

# Dateisystem & Slots
- Bis zu **5 Strecken** speicherbar
- Automatisches Laden des letzten gültigen Slots
- Funktionen:
  - Laden
  - Speichern
  - Löschen
  - Neu berechnen

---

# Code‑Architektur
- **State‑Machine** für UI
- **Nodes** definieren die Strecke
- **Bezier‑Interpolation** für Kurven
- **Segment‑Physik** für Bewegung
- **LED‑Renderer** für Zug & Strecke
- **OLED‑Renderer** für Menüs & Telemetrie
- **LittleFS** für Speicher‑Slots

---

# 🧩 Detaillierte Code‑Erklärung

## Bibliotheken & Hardware‑Setup
Der Code nutzt:
- `Adafruit_SH1106G` → OLED‑Display
- `Adafruit_NeoPixel` → LED‑Streifen
- `RotaryEncoder` → Eingabegerät
- `LittleFS` → Dateisystem
- `Wire` → I²C für Display

---

## Datenstrukturen

### Node
```cpp
struct Node {
  uint16_t pos;
  uint16_t height;
  uint8_t  type;
  uint8_t  art;
  float    force;
  uint16_t length;
};
```

### SegmentData
```cpp
struct SegmentData {
  float distance;
  float acc;
};
```

---

## UI‑State‑Machine
Alle Menüs werden über ein großes `enum UI_STATE` gesteuert:
- Hauptmenü
- Optik‑Menü
- Elemente‑Menü
- Datei‑Menü
- Setup‑Menü
- Play‑Modus

Jeder Zustand hat eine eigene Render‑Funktion:
- `drawMainMenu()`
- `drawOpticsMenu()`
- `drawEditNodeMenu()`
- `drawTelemetry()`

---

## Physik & LED‑Simulation
Wichtige Variablen:
- `pos` → Position
- `vel` → Geschwindigkeit
- `friction` → Reibung
- `airResistance` → Luftwiderstand
- `carLength` → Länge des LED‑Zuges

Wichtige Funktionen:
- `calculations()` → Physik
- `bezierCalculations()` → Kurven glätten
- `segmentCalculations()` → Segmentliste erzeugen
- `moveLED()` → Zug bewegen

---

## Datei‑Handling (LittleFS)
Jeder Slot speichert:
- Nodes
- Optik‑Einstellungen
- Streckenlänge
- Display‑FPS

Funktionen:
- `savePlayDataSlot()`
- `loadPlayDataSlot()`
- `slotExists()`
- `getSlotFilename()`

---