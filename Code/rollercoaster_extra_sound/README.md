# LED-Achterbahn mit Sound (ESP32 + DFPlayer Mini)

LED-Achterbahn-Simulation auf einem WS2812-Streifen (bis 1000 LEDs) mit physikbasierter
Wagenbewegung, OLED-Menü, Drehencoder-Bedienung und Soundausgabe über einen DFPlayer Mini.

---

## Hardware

| Komponente        | Typ / Hinweis                                  |
|-------------------|------------------------------------------------|
| Mikrocontroller   | ESP32                                          |
| LED-Streifen      | WS2812 (NeoPixel), bis 1000 LEDs               |
| Display           | OLED SH1106, 128x64, I2C (Adresse 0x3C)        |
| Bedienung         | Drehencoder mit Taster                         |
| Sound             | DFPlayer Mini (**Original YX5200 empfohlen!**) |
| Lautsprecher      | Mono, 4-8 Ohm, max. 3 W                        |
| Netzteil          | 5 V / 10 A (fuer LED-Streifen zwingend!)       |

> **Wichtig:** Clone-DFPlayer (z. B. GD3200B, MH2024K) verursachen Probleme:
> falsche Ordner-Zaehlung, defektes loop(), spontanes Abspielen beim Einschalten.
> Ein Original-Modul erspart viel Fehlersuche.

---

## Pinbelegung ESP32

| ESP32-Pin | verbunden mit                          |
|-----------|----------------------------------------|
| GPIO 33   | LED-Streifen Datenleitung (DIN)        |
| GPIO 25   | Encoder CLK                            |
| GPIO 26   | Encoder DT                             |
| GPIO 27   | Encoder Taster (SW)                    |
| GPIO 21   | OLED SDA (I2C)                         |
| GPIO 22   | OLED SCL (I2C)                         |
| GPIO 17   | DFPlayer RX (**ueber 1 kOhm Widerstand!**) |
| GPIO 16   | DFPlayer TX                            |

## Anschluss DFPlayer Mini

Modul so halten, dass der SD-Karten-Slot nach oben zeigt.
Pin 1 (VCC) ist dann oben links; die linke Seite zaehlt von oben nach unten.

| DFPlayer-Pin | verbunden mit                                   |
|--------------|--------------------------------------------------|
| VCC (Pin 1)  | 5 V                                              |
| RX  (Pin 2)  | ESP32 GPIO 17, **1 kOhm Widerstand in Serie**    |
| TX  (Pin 3)  | ESP32 GPIO 16 (direkt)                           |
| SPK_1 (Pin 6)| Lautsprecher +                                   |
| GND (Pin 7)  | Masse (gemeinsam mit ESP32 und Netzteil!)        |
| SPK_2 (Pin 8)| Lautsprecher -                                   |

Alle uebrigen Pins (BUSY, DAC, ADKEY, IO, USB) bleiben frei.
Freie Pins der rechten Seite nicht beruehren lassen - ADKEY/IO loesen sonst
ungewollt Wiedergabe aus.

### Stromversorgung (wichtig!)

- **LED-Streifen zwingend an ein eigenes 5-V-Netzteil** - niemals ueber den
  USB-Port versorgen. Ein gemeinsamer USB-Betrieb fuehrt zu Brummen aus dem
  Lautsprecher, Sound-Aussetzern und Systemabstuerzen.
- Bei 1000 LEDs: Strom an **mehreren Punkten** einspeisen (Anfang, Mitte, Ende),
  Kabelquerschnitt mind. 1,5 mm².
- **Elko 470-1000 uF** direkt an VCC/GND des DFPlayer (Polung beachten).
- **Alle Massen verbinden** (Netzteil, ESP32, DFPlayer, LED-Streifen),
  sternfoermig zum Netzteil gefuehrt.
- Der ESP32 selbst kann am USB bleiben (praktisch zum Flashen).

### Software-Hinweis zur Initialisierung

Der DFPlayer wird mit `dfPlayer.begin(dfpSerial, /*isACK=*/false, /*doReset=*/true)`
initialisiert: Der abgeschaltete ACK-Check verhindert, dass Befehle wie `stop()`
blockieren; der Reset sorgt fuer einen definierten Startzustand.
Nach Tests das System **10-15 Sekunden stromlos** lassen - der Elko haelt den
Player sonst am Leben und alte Zustaende (Loop-Modus etc.) bleiben erhalten.

---

## SD-Karte (DFPlayer)

- Max. 32 GB, **FAT32** formatiert (nicht exFAT!)
- Dateien in numerischer Reihenfolge einzeln kopieren (FAT-Reihenfolge!)
- Bei macOS: versteckte `._`-Dateien entfernen (`dot_clean /Volumes/SD`)

```
SD-Karte (Root)
├── mp3/
│   ├── 0001.mp3   Kettenlift          (Modus "Effekte")
│   ├── 0002.mp3   Booster / Launch    (Modus "Effekte")
│   ├── 0003.mp3   Bremse / Zischen    (Modus "Effekte")
│   ├── 0004.mp3   Theme-Song          (Modus "Theme+FX" und "Theme")
│   └── 0005.mp3   Welcome / Start     (Modus "Effekte", beim Fahrtbeginn)
├── ADVERT/
│   ├── 0001.mp3   Kettenlift   ┐
│   ├── 0002.mp3   Booster      ├─ identische Dateien wie /mp3/0001-0003,
│   └── 0003.mp3   Bremse       ┘  als Einwurf fuer Modus "Theme+FX"
└── 02/
    ├── 001.mp3    ┐
    ├── 002.mp3    ├─ beliebig viele Random-Tracks (Modus "Random"),
    └── ...        ┘  lueckenlos nummeriert, drei Ziffern
```

**Namenskonventionen beachten:**
- `/mp3` und `/ADVERT`: Dateinamen mit **vier** Ziffern (`0001.mp3`)
- Nummerierte Ordner (`02`): Ordnername **zwei** Ziffern, Dateien **drei** Ziffern (`001.mp3`)

---

## Sound-Modi (KONFIG-Menue > S-Modus)

| Modus      | Verhalten                                                          |
|------------|--------------------------------------------------------------------|
| Theme+FX   | Theme-Song laeuft in Schleife; Zonen-Effekte (Lift, Booster, Bremse) werden per Advertise eingeblendet, danach laeuft der Theme an alter Stelle weiter |
| Random     | Zufaellige Tracks aus `/02` in Endlosschleife, keine Unterbrechungen |
| Theme      | Nur der Theme-Song in Schleife, keine Effekte                       |
| Effekte    | Stille bei freier Fahrt; Zonen-Sounds als Einzeltracks, Welcome beim Start |

Weitere Sound-Einstellungen im KONFIG-Menue: Lautstaerke (0-30, mit Hoerprobe)
und Sound An/Aus. Alle Einstellungen werden im aktiven Strecken-Slot gespeichert
(beim Verlassen des Menues ueber ZURUECK).