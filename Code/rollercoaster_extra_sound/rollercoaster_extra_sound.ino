#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <RotaryEncoder.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <DFRobotDFPlayerMini.h>   // NEU: DFPlayer Mini Bibliothek

// ==========================================
// PIN- & HARDWARE-EINSTELLUNGEN
// ==========================================
#define LED_PIN 33

#define PIN_CLK 25
#define PIN_DT  26
#define PIN_SW  27
RotaryEncoder encoder(PIN_CLK, PIN_DT, RotaryEncoder::LatchMode::FOUR3);

// NEU: DFPlayer Mini an Hardware-Serial2
#define DFP_RX_PIN 16   // ESP32 RX2  <-- an TX vom DFPlayer
#define DFP_TX_PIN 17   // ESP32 TX2  --> an RX vom DFPlayer (über 1kOhm Widerstand!)

HardwareSerial dfpSerial(2);       // UART2 des ESP32
DFRobotDFPlayerMini dfPlayer;
bool dfpReady = false;             // Merker: Player erfolgreich initialisiert?

// NEU: Sound-Einstellungen
uint8_t soundVolume = 20;          // Lautstärke 0-30
uint8_t soundEnabled = 1;          // 0 = Aus, 1 = An
uint8_t soundMode = 0;             // 0 = Effekte (Status-Sounds), 1 = Random (Zufallswiedergabe)

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==========================================
// GRUNDLEGENDE STRUKTUREN
// ==========================================
struct point2D {
  float x;
  float y;
};

// Element-Arten
const char* elementArtNames[] = {
  "Hill", "Loop", "Helix", "Steilkurve", "Valley", "Lift", "Brake", "Booster"
};
const int NUM_ARTEN = 8;

struct Node {
  uint16_t pos;
  uint16_t height;
  uint8_t  type;    // 0=Startpunkt, 1=Scheitelpunkt, 2=Endpunkt
  uint8_t  art;     // 0=Kuppe, 1=Loop, 2=Helix, 3=Steilkurve, 4=Bottom, 5=Lift, 6=Bremse, 7=Booster
  float    force;   // Einstellbarer Wert (Lift: m/s, Bremse: Ziel m/s, Booster: Schub)
  uint16_t length;  // Länge der Funktionszone in Fließrichtung (LEDs)
  uint8_t  enabled; // 0 = deaktiviert, 1 = aktiv (nur relevant für art >= 5)
};

Node* nodes = nullptr;
uint16_t nodeCount = 0;
Node currentNode = {};

struct SegmentData {
  float distance;
  float acc;
  float angle;      // Winkel des Segments
  float curvature;  // Krümmung für G-Kraft (Zentripetalbeschleunigung)
};

SegmentData* segments = nullptr;
uint16_t segmentCount = 0;

// ==========================================
// NEU: SOUND-DEFINITIONEN
// ==========================================
// Dateien auf der SD-Karte im Ordner /mp3:
// 0001.mp3 = Kettenlift (Klackern, Loop-fähig)
// 0002.mp3 = Booster / Launch
// 0003.mp3 = Bremsen (Zischen)
// 0004.mp3 = Freie Fahrt / Fahrtwind (Loop-fähig)
// 0005.mp3 = Start-Jingle (beim Betreten von ABSPIELEN)
#define SND_LIFT      1
#define SND_BOOSTER   2
#define SND_BRAKE     3
#define SND_RIDE      4
#define SND_START     5
#define RANDOM_FOLDER 2 // Ordner "02" auf der SD-Karte

String lastSoundStatus = "";   // Zuletzt vertonter Status
static unsigned long lastSoundCmd = 0;
int randomFolderCount = 0;       // Anzahl Dateien im Random-Ordner (wird im Setup ermittelt)
bool randomModeActive = false;   // Läuft gerade die Random-Wiedergabe?

void playSound(uint8_t track, bool loopIt = false) {
    if (!dfpReady || !soundEnabled) return;
    if (loopIt) dfPlayer.loopFolder(0);  // Vorsichtshalber Loop beenden... 
    if (loopIt) dfPlayer.loop(track);    // ...und Track als Endlosschleife starten
    else        dfPlayer.playMp3Folder(track);
}

void playRandomSound() {
    if (!dfpReady || !soundEnabled || randomFolderCount <= 0) return;
    int track = random(1, randomFolderCount + 1);   // 1 bis N (obere Grenze exklusiv)
    dfPlayer.playFolder(RANDOM_FOLDER, track);
    lastSoundCmd = millis();
}

void stopSound() {
    if (!dfpReady) return;
    randomModeActive = false;   // NEU
    dfPlayer.stop();
    lastSoundStatus = "";
}

// Wird bei jedem Physik-Durchlauf aufgerufen, reagiert nur auf Statuswechsel
void updateSound(String status) {
    if (!dfpReady || !soundEnabled) return;

    // NEU: Random-Modus - Status ist egal, einfach Zufallswiedergabe laufen lassen
    if (soundMode == 1) {
        if (!randomModeActive && randomFolderCount > 0) {
            randomModeActive = true;
            playRandomSound();
        }
        return;
    }

    // Effekte-Modus (wie bisher)
    if (status == lastSoundStatus) return;
    if (millis() - lastSoundCmd < 150) return;
    lastSoundCmd = millis();
    lastSoundStatus = status;

    if (status == "KETTENLIFT") {
        randomModeActive = false;
        dfPlayer.loop(SND_LIFT);
    } else if (status == "BOOSTER") {
        randomModeActive = false;
        dfPlayer.playMp3Folder(SND_BOOSTER);
    } else if (status == "BREMSEN") {
        randomModeActive = false;
        dfPlayer.playMp3Folder(SND_BRAKE);
    } else { // FREIE FAHRT & AUSROLLEN
        if (randomFolderCount > 0) {
            randomModeActive = true;
            playRandomSound();
        } else {
            randomModeActive = false;
            dfPlayer.loop(SND_RIDE);
        }
    }
}

// ==========================================
// STATE MACHINE (UI_STATE)
// ==========================================
enum UI_STATE {
    ST_MENU,
    ST_PLAY,

    ST_OPTICS_MENU,        
    ST_OPTICS_CAR_COLOR,   
    ST_OPTICS_TRACK_COLOR,  
    ST_OPTICS_CAR_LENGTH,  
    ST_OPTICS_CAR_BRIGHTNESS,   
    ST_OPTICS_TRACK_BRIGHTNESS, 
    ST_OPTICS_ZONE_BRIGHTNESS,
    ST_OPTICS_DISPLAY_MODE, 
    ST_OPTICS_DISPLAY_FPS,   
    ST_OPTICS_MAX_SPEED, 
    ST_OPTICS_GFORCE_SCALE, // NEU: G-Kraft Faktor im Menü   
    ST_OPTICS_ZONE_EFFECTS,
    ST_OPTICS_SOUND_VOLUME,   // NEU
    ST_OPTICS_SOUND_ENABLED,  // NEU
    ST_OPTICS_SOUND_MODE,     // NEU

    ST_ELEMENTS_MENU,      

    ST_EDIT_SELECT_NODE,   
    ST_EDIT_NODE_MENU,     
    ST_EDIT_NODE_POS,      
    ST_EDIT_NODE_HEIGHT,   
    ST_EDIT_NODE_ART,       
    ST_EDIT_NODE_FORCE,    
    ST_EDIT_NODE_LENGTH,   
    ST_EDIT_NODE_DELETE,   

    ST_INSERT_POS,         
    ST_INSERT_HEIGHT,      
    ST_INSERT_ART,         
    ST_INSERT_FORCE,       
    ST_INSERT_LENGTH,      

    ST_FILE_SELECT_SLOT,   
    ST_FILE_SLOT_ACTION,   
    ST_FILE_DELETE_CONFIRM,

    ST_SETUP_WARNING,
    ST_SETUP_LENGTH,

    ST_NODE_START,
    ST_NODE_POS,
    ST_NODE_HEIGHT,
    ST_NODE_TYPE,
    ST_NODE_ART_SETUP,     
    ST_NODE_FORCE_SETUP,   
    ST_NODE_LENGTH_SETUP,  

    ST_NODE_END
};

UI_STATE uiState = ST_MENU;

// ==========================================
// UI- & STEUERVARIABLEN
// ==========================================
int lastPos = -1;
int menuSelection = 0;
int opticsMenuSelection = 0; 
int elementsMenuSelection = 0;
int editNodeIdx = 0;        
int editMenuSelection = 0; 
int editDeleteConfirmSelection = 1; 

const int NUM_SLOTS = 5;
int selectedSlot = 1;           
int activeLoadedSlot = 1;       
int slotActionSelection = 0;    
int deleteConfirmSelection = 1;

// ==========================================
// GLOBALE VARIABLEN & LED-STREIFEN
// ==========================================
Adafruit_NeoPixel strip(0, LED_PIN, NEO_GRB + NEO_KHZ800);
uint16_t ledCount;          
uint16_t currentLED = 0;
int lengthSpeed = 0;
unsigned long lastLengthMove = 0;
unsigned long lastLengthDecel = 0;
const int lengthMaxSpeed = 10;

point2D* points = nullptr;
uint16_t pointCount = 0;
const int steps = 200;        
int targetLength = 0;   
const float tolerance = 0.001f; 

int startHeight; 
int endHeight;  

point2D p1; point2D c1; point2D c2; point2D p2;

point2D* bezierPoints = nullptr;
uint16_t bezierPointCount = 0;
const int intSteps = 40; // ERHÖHT für rundere Kurven (bessere G-Kraft-Berechnung)

float pos = 0.0f;
float vel = 0.0f;
float maxReachedSpeed = 0.0f;
float currentGForce = 1.0f;   
float maxGForce = 1.0f;       
float minGForce = 1.0f;       

uint16_t startLedPos = 0;
unsigned long lastTime = 0;
int currentSegment = 0;
uint8_t playBrightness = 255; 

// ==========================================
// ACHTERBAHN - OPTIONEN & FARBEN
// ==========================================
int carLength = 5;  
int carBrightness = 150;   
int trackBrightness = 10;
int zoneBrightness = 10; 
uint8_t displayMode = 0; 
int displayFps = 4;        
float colorMaxSpeed = 15.0f; 
float gForceScale = 0.20f; // NEU: Skalierungsfaktor für G-Kräfte (Default 20%)
uint8_t showZoneEffects = 2;

const int NUM_COLORS = 9;
uint32_t colorPalette[NUM_COLORS];
const char* colorNames[NUM_COLORS] = {
  "Rot", "Gruen", "Blau", "Gelb", "Magenta", "Cyan", "Weiss", "Aus", "Speed"
};

uint8_t selectedCarColorIdx = 0;   
uint8_t selectedTrackColorIdx = 7; 

uint32_t carColor;
uint32_t trackColor;

float friction = 0.015f;                           
float airResistance = 0.002f;                      

// ==========================================
// VORAB-DEKLARATIONEN
// ==========================================
void drawMainMenu();
void drawOpticsMenu();
void drawElementsMenu();
void drawColorSelection(bool isCar);
void drawBrightnessSelection(bool isCar);
void drawZoneBrightnessSelection(); 
void drawCarLengthSelection();
void drawDisplayModeSelection();
void drawDisplayFpsSelection(); 
void drawMaxSpeedSelection(); 
void drawGForceScaleSelection(); // NEU
void drawZoneEffectsSelection();
void drawEditSelectNode();
void drawEditNodeMenu();
void drawEditNodePos();    
void drawEditNodeHeight();
void drawEditNodeArt();
void drawEditNodeForce();
void drawEditNodeLength(); 
void drawEditNodeDelete(); 
void drawInsertPos();
void drawInsertHeight();
void drawInsertArt();
void drawInsertForce();
void drawInsertLength();   
void drawSetupWarning();
void drawSetupLength();
void drawNodeStart();
void drawNodePos();
void drawNodeHeight();
void drawNodeType();
void drawNodeArtSetup();
void drawNodeForceSetup();
void drawNodeLengthSetup();
void drawFileSelectSlot();
void drawFileSlotAction();
void drawFileDeleteConfirm();
void drawSetupLeds();
void drawTelemetry(int currentLed, float currentVel, float currentAcc, String status);
void drawSoundVolumeSelection();   // NEU
void drawSoundEnabledSelection();  // NEU
void drawSoundModeSelection();     // NEU

uint32_t getSpeedColor(float currentVel, uint8_t bright); 
void updateActualColors();
void calculations();
void bezierCalculations();
void segmentCalculations();
void sortNodesAndRecalculate(); 
void insertNodeAndRecalculate();
void deleteNodeAndRecalculate(int idx); 
void deleteEndpointAndReset();   
void triggerManualRecalc(); 
void moveLED(); 

void savePlayDataSlot(int slot);
bool loadPlayDataSlot(int slot);
bool slotExists(int slot);
String getSlotFilename(int slot);

bool isButtonPressed() {
    static bool stableState = HIGH;
    static bool lastReading = HIGH;
    static unsigned long lastChange = 0;
    const unsigned long debounceTime = 30;

    bool reading = digitalRead(PIN_SW);
    if (reading != lastReading) {
        lastChange = millis();
    }
    if ((millis() - lastChange) > debounceTime) {
        if (stableState == HIGH && reading == LOW) {
            stableState = reading;
            return true;   
        }
        stableState = reading;
    }
    lastReading = reading;
    return false;
}

// ==========================================
// SETUP
// ==========================================
void setup() {
    Serial.begin(115200);

    // NEU: DFPlayer initialisieren
    dfpSerial.begin(9600, SERIAL_8N1, DFP_RX_PIN, DFP_TX_PIN);
    delay(1000); // DFPlayer braucht kurz Zeit nach Power-On
    
    // GEÄNDERT: bis zu 3 Versuche, ACK-Check aus (Clone-kompatibel), Reset an
    for (int attempt = 1; attempt <= 3 && !dfpReady; attempt++) {
        if (dfPlayer.begin(dfpSerial, /*isACK=*/false, /*doReset=*/true)) {
            dfpReady = true;
        } else {
            Serial.print(F("DFPlayer Versuch ")); Serial.print(attempt); Serial.println(F(" fehlgeschlagen..."));
            delay(500);
        }
    }

    if (dfPlayer.begin(dfpSerial)) {
        dfpReady = true;
        dfPlayer.volume(soundVolume);
        delay(500);  // GEÄNDERT: mehr Zeit nach Reset

        // GEÄNDERT: zweimal abfragen, erster Wert ist oft ungueltig
        randomFolderCount = dfPlayer.readFileCountsInFolder(RANDOM_FOLDER);
        delay(200);
        int secondRead = dfPlayer.readFileCountsInFolder(RANDOM_FOLDER);
        if (secondRead > 0) randomFolderCount = secondRead;
        if (randomFolderCount < 0) randomFolderCount = 0;
        Serial.print(F("DFPlayer bereit. Random-Tracks: "));
        Serial.println(randomFolderCount);
    } else {
        Serial.println(F("DFPlayer nicht gefunden! (Verkabelung/SD-Karte pruefen)"));
    }

    pinMode(PIN_SW, INPUT_PULLUP);
    pinMode(PIN_CLK, INPUT_PULLUP);
    pinMode(PIN_DT, INPUT_PULLUP);

    if (!LittleFS.begin(true)) {
        Serial.println(F("LittleFS Fehler!"));
    }

    ledCount = 1000;
    strip.updateLength(ledCount);
    strip.begin(); strip.clear(); strip.show();

    colorPalette[0] = strip.Color(255, 0, 0);     
    colorPalette[1] = strip.Color(0, 255, 0);     
    colorPalette[2] = strip.Color(0, 0, 255);     
    colorPalette[3] = strip.Color(255, 255, 0);   
    colorPalette[4] = strip.Color(255, 0, 255);   
    colorPalette[5] = strip.Color(0, 255, 255);   
    colorPalette[6] = strip.Color(255, 255, 255); 
    colorPalette[7] = strip.Color(0, 0, 13);      
    colorPalette[8] = strip.Color(0, 0, 0); 

    Wire.begin();
    Wire.setClock(400000);

    if(!display.begin(0x3C)) {
        Serial.println(F("SH1106 Fehler"));
        for(;;);
    }

    if (loadPlayDataSlot(activeLoadedSlot)) {
        Serial.println(F("Slot 1 geladen."));
    } else {
        for(int s=2; s<=NUM_SLOTS; s++) {
            if(loadPlayDataSlot(s)) {
                activeLoadedSlot = s;
                break;
            }
        }
    }

    updateActualColors();
    display.clearDisplay();
    Serial.println(F("System bereit."));
}

// ==========================================
// MAIN LOOP
// ==========================================
void loop() {
  encoder.tick();
  int newPos = encoder.getPosition();

  switch (uiState) {
    
    case ST_MENU:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 4) { encoder.setPosition(3); newPos = 3; } 
        menuSelection = newPos; drawMainMenu(); lastPos = newPos;
      }

      if (isButtonPressed()) {
        if (menuSelection == 0) {
          if (segments != nullptr) {
            uiState = ST_PLAY;
          } else {
            display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
            display.setCursor(0, 0); display.println(F("Keine Daten geladen!\nBitte erst Strecke\nladen oder erstellen."));
            display.display(); delay(2000); drawMainMenu();
          }
        }
        else if (menuSelection == 1) {
          uiState = ST_OPTICS_MENU; opticsMenuSelection = 0; encoder.setPosition(0);
        }
        else if (menuSelection == 2) {
          uiState = ST_ELEMENTS_MENU; elementsMenuSelection = 0; encoder.setPosition(0);
        }
        else if (menuSelection == 3) {
          uiState = ST_FILE_SELECT_SLOT; selectedSlot = activeLoadedSlot; encoder.setPosition(selectedSlot - 1);
        }
        menuSelection = 0; lastPos = -1;
      }
      break;
      
    case ST_FILE_SELECT_SLOT:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > NUM_SLOTS) { encoder.setPosition(NUM_SLOTS); newPos = NUM_SLOTS; }
        if (newPos < NUM_SLOTS) selectedSlot = newPos + 1;
        drawFileSelectSlot(); lastPos = newPos;
      }

      if (isButtonPressed()) {
        if (encoder.getPosition() == NUM_SLOTS) {
          uiState = ST_MENU; encoder.setPosition(3); 
        } else {
          uiState = ST_FILE_SLOT_ACTION; slotActionSelection = 0; encoder.setPosition(0);
        }
        lastPos = -1;
      }
      break;

    case ST_FILE_SLOT_ACTION:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 5) { encoder.setPosition(4); newPos = 4; } 
        slotActionSelection = newPos; drawFileSlotAction(); lastPos = newPos;
      }

      if (isButtonPressed()) {
        if (slotActionSelection == 0) { // Laden
          if (slotExists(selectedSlot)) {
            display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
            display.setCursor(0, 20); display.print(F("Laed Slot ")); display.print(selectedSlot); display.println(F("..."));
            display.display(); delay(400);

            if(loadPlayDataSlot(selectedSlot)) {
                activeLoadedSlot = selectedSlot;
                updateActualColors();
                uiState = ST_MENU; encoder.setPosition(0); 
            } else {
                display.clearDisplay(); display.setCursor(0, 20); display.println(F("Ladefehler!"));
                display.display(); delay(1500);
                uiState = ST_FILE_SELECT_SLOT; encoder.setPosition(selectedSlot - 1);
            }
          } else {
            display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
            display.setCursor(0, 20); display.println(F("Slot ist leer!")); display.display(); delay(1200);
            drawFileSlotAction();
          }
        } 
        else if (slotActionSelection == 1) { // Neu erstellen
          activeLoadedSlot = selectedSlot; uiState = ST_SETUP_WARNING; encoder.setPosition(0);
        } 
        else if (slotActionSelection == 2) { // Neu berechnen
          if (slotExists(selectedSlot)) {
              if(loadPlayDataSlot(selectedSlot)) {
                 activeLoadedSlot = selectedSlot;
                 updateActualColors();
                 triggerManualRecalc();
              }
          } else {
              display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
              display.setCursor(0, 20); display.println(F("Slot ist leer!")); display.display(); delay(1200);
              drawFileSlotAction();
          }
        }
        else if (slotActionSelection == 3) { // Loeschen
          if (slotExists(selectedSlot)) {
            uiState = ST_FILE_DELETE_CONFIRM; deleteConfirmSelection = 1; encoder.setPosition(1);
          } else {
            display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
            display.setCursor(0, 20); display.println(F("Slot bereits leer!")); display.display(); delay(1200);
            drawFileSlotAction();
          }
        } 
        else if (slotActionSelection == 4) { // Zurueck
          uiState = ST_FILE_SELECT_SLOT; encoder.setPosition(selectedSlot - 1);
        }
        lastPos = -1;
      }
      break;

    case ST_FILE_DELETE_CONFIRM:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 2) { encoder.setPosition(1); newPos = 1; }
        deleteConfirmSelection = newPos; drawFileDeleteConfirm(); lastPos = newPos;
      }

      if (isButtonPressed()) {
        if (deleteConfirmSelection == 0) {
          String filename = getSlotFilename(selectedSlot);
          LittleFS.remove(filename);

          if (selectedSlot == activeLoadedSlot) {
             if(nodes != nullptr) { delete[] nodes; nodes = nullptr; }
             if(segments != nullptr) { delete[] segments; segments = nullptr; }
             nodeCount = 0; segmentCount = 0;
          }
          display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
          display.setCursor(0, 25); display.println(F("Slot geloescht!")); display.display(); delay(1000);
        }
        uiState = ST_FILE_SLOT_ACTION; encoder.setPosition(3); lastPos = -1;
      }
      break;

    case ST_OPTICS_MENU:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 15) { encoder.setPosition(14); newPos = 14; } 
        opticsMenuSelection = newPos; drawOpticsMenu(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        if (opticsMenuSelection == 0) { uiState = ST_OPTICS_CAR_COLOR; encoder.setPosition(selectedCarColorIdx); }
        else if (opticsMenuSelection == 1) { uiState = ST_OPTICS_TRACK_COLOR; encoder.setPosition(selectedTrackColorIdx); }
        else if (opticsMenuSelection == 2) { uiState = ST_OPTICS_CAR_LENGTH; encoder.setPosition(carLength); }
        else if (opticsMenuSelection == 3) { uiState = ST_OPTICS_CAR_BRIGHTNESS; encoder.setPosition(carBrightness); }
        else if (opticsMenuSelection == 4) { uiState = ST_OPTICS_TRACK_BRIGHTNESS; encoder.setPosition(trackBrightness); }
        else if (opticsMenuSelection == 5) { uiState = ST_OPTICS_ZONE_BRIGHTNESS; encoder.setPosition(zoneBrightness); }
        else if (opticsMenuSelection == 6) { uiState = ST_OPTICS_DISPLAY_MODE; encoder.setPosition(displayMode); }
        else if (opticsMenuSelection == 7) { uiState = ST_OPTICS_DISPLAY_FPS; encoder.setPosition(displayFps); } 
        else if (opticsMenuSelection == 8) { uiState = ST_OPTICS_MAX_SPEED; encoder.setPosition((int)(colorMaxSpeed * 2.0f)); }
        else if (opticsMenuSelection == 9) { uiState = ST_OPTICS_GFORCE_SCALE; encoder.setPosition((int)(gForceScale * 100.0f)); } 
        else if (opticsMenuSelection == 10) { uiState = ST_OPTICS_ZONE_EFFECTS; encoder.setPosition(showZoneEffects); }
        else if (opticsMenuSelection == 11) { uiState = ST_OPTICS_SOUND_VOLUME; encoder.setPosition(soundVolume); 
                                              if (dfpReady && soundEnabled) dfPlayer.loop(SND_RIDE);}  // Hörprobe     // NEU
        else if (opticsMenuSelection == 12) { uiState = ST_OPTICS_SOUND_ENABLED; encoder.setPosition(soundEnabled); }   // NEU
        else if (opticsMenuSelection == 13) { uiState = ST_OPTICS_SOUND_MODE; encoder.setPosition(soundMode); }      // NEU
        else if (opticsMenuSelection == 14) { uiState = ST_MENU; encoder.setPosition(1); savePlayDataSlot(activeLoadedSlot); }
        lastPos = -1;
      }
      break;

    case ST_OPTICS_GFORCE_SCALE:
      if (newPos != lastPos) {
        if (newPos < 1) { encoder.setPosition(1); newPos = 1; }
        if (newPos > 100) { encoder.setPosition(100); newPos = 100; }
        gForceScale = (float)newPos / 100.0f; drawGForceScaleSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(9); lastPos = -1; }
      break;

    case ST_OPTICS_CAR_COLOR:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= NUM_COLORS) { encoder.setPosition(NUM_COLORS - 1); newPos = NUM_COLORS - 1; }
        selectedCarColorIdx = newPos; updateActualColors(); drawColorSelection(true); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(0); lastPos = -1; }
      break;

    case ST_OPTICS_TRACK_COLOR:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= NUM_COLORS) { encoder.setPosition(NUM_COLORS - 1); newPos = NUM_COLORS - 1; }
        selectedTrackColorIdx = newPos; updateActualColors(); drawColorSelection(false); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(1); lastPos = -1; }
      break;

    case ST_OPTICS_CAR_LENGTH:
      if (newPos != lastPos) {
        if (newPos < 1) { encoder.setPosition(1); newPos = 1; }
        if (newPos > 30) { encoder.setPosition(30); newPos = 30; }
        carLength = newPos; drawCarLengthSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(2); lastPos = -1; }
      break;

    case ST_OPTICS_CAR_BRIGHTNESS:
      if (newPos != lastPos) {
        if (newPos < 5) { encoder.setPosition(5); newPos = 5; }
        if (newPos > 255) { encoder.setPosition(255); newPos = 255; }
        carBrightness = newPos; updateActualColors(); drawBrightnessSelection(true); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(3); lastPos = -1; }
      break;

    case ST_OPTICS_TRACK_BRIGHTNESS:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 255) { encoder.setPosition(255); newPos = 255; }
        trackBrightness = newPos; updateActualColors(); drawBrightnessSelection(false); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(4); lastPos = -1; }
      break;

    case ST_OPTICS_ZONE_BRIGHTNESS:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 255) { encoder.setPosition(255); newPos = 255; }
        zoneBrightness = newPos; drawZoneBrightnessSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(5); lastPos = -1; }
      break;

    case ST_OPTICS_DISPLAY_MODE:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 1) { encoder.setPosition(1); newPos = 1; }
        displayMode = newPos; drawDisplayModeSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(6); lastPos = -1; }
      break;

    case ST_OPTICS_DISPLAY_FPS: 
      if (newPos != lastPos) {
        if (newPos < 1) { encoder.setPosition(1); newPos = 1; }
        if (newPos > 20) { encoder.setPosition(20); newPos = 20; }
        displayFps = newPos; drawDisplayFpsSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(7); lastPos = -1; }
      break;

    case ST_OPTICS_MAX_SPEED:
      if (newPos != lastPos) {
        if (newPos < 2) { encoder.setPosition(2); newPos = 2; }
        if (newPos > 60) { encoder.setPosition(60); newPos = 60; }
        colorMaxSpeed = (float)newPos / 2.0f; drawMaxSpeedSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(8); lastPos = -1; }
      break;

    case ST_OPTICS_ZONE_EFFECTS:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 2) { encoder.setPosition(2); newPos = 2; }
        showZoneEffects = newPos; drawZoneEffectsSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(10); lastPos = -1; } 
      break;
    
    case ST_OPTICS_SOUND_VOLUME:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 30) { encoder.setPosition(30); newPos = 30; }
        soundVolume = newPos;
        if (dfpReady) dfPlayer.volume(soundVolume);
        drawSoundVolumeSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        if (dfpReady) dfPlayer.stop();  // Hörprobe beenden
        uiState = ST_OPTICS_MENU; encoder.setPosition(11); lastPos = -1;
      }
      break;

    case ST_OPTICS_SOUND_ENABLED:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 1) { encoder.setPosition(1); newPos = 1; }
        soundEnabled = newPos;
        if (!soundEnabled && dfpReady) dfPlayer.stop();
        drawSoundEnabledSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(12); lastPos = -1; }
      break;

    case ST_OPTICS_SOUND_MODE:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 1) { encoder.setPosition(1); newPos = 1; }
        soundMode = newPos;
        drawSoundModeSelection(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_OPTICS_MENU; encoder.setPosition(13); lastPos = -1; }
      break;

    case ST_ELEMENTS_MENU:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 3) { encoder.setPosition(2); newPos = 2; } 
        elementsMenuSelection = newPos; drawElementsMenu(); lastPos = newPos;
      }

      if (isButtonPressed()) {
        if (elementsMenuSelection == 0) {
          if (nodeCount > 0 && nodes != nullptr) {
            uiState = ST_EDIT_SELECT_NODE; editNodeIdx = 0; encoder.setPosition(0);
          } else {
            display.clearDisplay(); display.setCursor(0, 20); display.println(F("Keine Elemente da!"));
            display.display(); delay(1200); drawElementsMenu();
          }
        } 
        else if (elementsMenuSelection == 1) {
          if (nodeCount < ledCount) {
            currentNode = {}; 
            currentLED = startLedPos; 
            currentNode.pos = currentLED;
            uiState = ST_INSERT_POS; encoder.setPosition(currentLED);
          } else {
            display.clearDisplay(); display.setCursor(0, 20); display.println(F("Max. Knoten erreicht!"));
            display.display(); delay(1200); drawElementsMenu();
          }
        } 
        else if (elementsMenuSelection == 2) {
          uiState = ST_MENU; encoder.setPosition(2);
        }
        lastPos = -1;
      }
      break;

    case ST_EDIT_SELECT_NODE:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > nodeCount) { encoder.setPosition(nodeCount); newPos = nodeCount; }
        editNodeIdx = newPos; drawEditSelectNode(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        if (editNodeIdx == nodeCount) {
          display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
          display.setCursor(0, 20); display.println(F("Speichere in\naktuellen Slot...")); display.display();
          sortNodesAndRecalculate(); 
          uiState = ST_ELEMENTS_MENU; encoder.setPosition(0); 
        } else {
          uiState = ST_EDIT_NODE_MENU; editMenuSelection = 0; encoder.setPosition(0);
        }
        lastPos = -1;
      }
      break;

    case ST_EDIT_NODE_MENU:
      if (newPos != lastPos) {
        bool isStart   = (nodes[editNodeIdx].type == 0);
        bool canDelete = (nodes[editNodeIdx].type == 1 || nodes[editNodeIdx].type == 2);
        bool hasFunc   = (nodes[editNodeIdx].art >= 5);

        int maxMenu = 1; 
        if (!isStart) maxMenu++;  
        maxMenu++; 
        if (hasFunc) maxMenu += 3; 
        if (canDelete) maxMenu++; 
        maxMenu++; 

        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= maxMenu) { encoder.setPosition(maxMenu - 1); newPos = maxMenu - 1; }
        
        editMenuSelection = newPos; drawEditNodeMenu(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        bool isStart   = (nodes[editNodeIdx].type == 0);
        bool canDelete = (nodes[editNodeIdx].type == 1 || nodes[editNodeIdx].type == 2);
        bool isFunc    = (nodes[editNodeIdx].art >= 5);

        int optIdx = 0;
        int optPos      = !isStart ? optIdx++ : -1;
        int optHeight   = optIdx++;
        int optArt      = optIdx++;
        int optForce    = isFunc ? optIdx++ : -1;
        int optLength   = isFunc ? optIdx++ : -1;
        int optEnabled  = isFunc ? optIdx++ : -1;  
        int optDel      = canDelete ? optIdx++ : -1;

        if (editMenuSelection == optPos) {
           uiState = ST_EDIT_NODE_POS; encoder.setPosition(nodes[editNodeIdx].pos);  
        } else if (editMenuSelection == optHeight) {
           uiState = ST_EDIT_NODE_HEIGHT; encoder.setPosition(nodes[editNodeIdx].height); 
        } else if (editMenuSelection == optArt) {
           uiState = ST_EDIT_NODE_ART; encoder.setPosition(nodes[editNodeIdx].art); 
        } else if (isFunc && editMenuSelection == optForce) {
           uiState = ST_EDIT_NODE_FORCE; encoder.setPosition((int)(nodes[editNodeIdx].force * 10.0f));
        } else if (isFunc && editMenuSelection == optLength) {
           uiState = ST_EDIT_NODE_LENGTH; encoder.setPosition(nodes[editNodeIdx].length);
        } else if (isFunc && editMenuSelection == optEnabled) {
           nodes[editNodeIdx].enabled = nodes[editNodeIdx].enabled ? 0 : 1;
           drawEditNodeMenu();
           drawSetupLeds();
        } else if (canDelete && editMenuSelection == optDel) {
           uiState = ST_EDIT_NODE_DELETE; editDeleteConfirmSelection = 1; encoder.setPosition(1);
        } else {
           uiState = ST_EDIT_SELECT_NODE; encoder.setPosition(editNodeIdx);
        }
        lastPos = -1;
      }
      break;

    case ST_EDIT_NODE_POS:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= ledCount) { encoder.setPosition(ledCount - 1); newPos = ledCount - 1; } 
        nodes[editNodeIdx].pos = newPos; drawEditNodePos(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        sortNodesAndRecalculate();
        uiState = ST_EDIT_NODE_MENU; encoder.setPosition(0); lastPos = -1;
      }
      break;

    case ST_EDIT_NODE_HEIGHT:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 50) { encoder.setPosition(50); newPos = 50; } 
        nodes[editNodeIdx].height = newPos; drawEditNodeHeight(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_EDIT_NODE_MENU; encoder.setPosition(0); lastPos = -1; }
      break;

    case ST_EDIT_NODE_ART:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= NUM_ARTEN) { encoder.setPosition(NUM_ARTEN - 1); newPos = NUM_ARTEN - 1; }
        nodes[editNodeIdx].art = newPos; 
        
        if (nodes[editNodeIdx].art == 5 && nodes[editNodeIdx].force == 0) { nodes[editNodeIdx].force = 1.2f; nodes[editNodeIdx].length = 30; nodes[editNodeIdx].enabled = 1; }
        if (nodes[editNodeIdx].art == 6 && nodes[editNodeIdx].force == 0) { nodes[editNodeIdx].force = 2.0f; nodes[editNodeIdx].length = 10; nodes[editNodeIdx].enabled = 1; } 
        if (nodes[editNodeIdx].art == 7 && nodes[editNodeIdx].force == 0) { nodes[editNodeIdx].force = 25.0f; nodes[editNodeIdx].length = 5; nodes[editNodeIdx].enabled = 1; }
        drawEditNodeArt(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_EDIT_NODE_MENU; encoder.setPosition(0); lastPos = -1; }
      break;

    case ST_EDIT_NODE_FORCE:
      if (newPos != lastPos) {
        float val = (float)newPos / 10.0f;
        if (nodes[editNodeIdx].art == 5) { 
           if (val < 0.1f) { encoder.setPosition(1); val = 0.1f; }
           if (val > 5.0f) { encoder.setPosition(50); val = 5.0f; }
        } else if (nodes[editNodeIdx].art == 6) { 
           if (val < 0.0f) { encoder.setPosition(0); val = 0.0f; }
           if (val > 20.0f) { encoder.setPosition(200); val = 20.0f; }
        } else if (nodes[editNodeIdx].art == 7) { 
           if (val < 1.0f) { encoder.setPosition(10); val = 1.0f; }
           if (val > 60.0f) { encoder.setPosition(600); val = 60.0f; }
        }
        nodes[editNodeIdx].force = val; drawEditNodeForce(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_EDIT_NODE_MENU; encoder.setPosition(0); lastPos = -1; }
      break;

    case ST_EDIT_NODE_LENGTH:
      if (newPos != lastPos) {
        if (newPos < 1) { encoder.setPosition(1); newPos = 1; }
        if (newPos > ledCount / 2) { encoder.setPosition(ledCount / 2); newPos = ledCount / 2; }
        nodes[editNodeIdx].length = newPos; drawEditNodeLength(); lastPos = newPos;
      }
      if (isButtonPressed()) { uiState = ST_EDIT_NODE_MENU; encoder.setPosition(0); lastPos = -1; }
      break;

    case ST_EDIT_NODE_DELETE:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 2) { encoder.setPosition(1); newPos = 1; }
        editDeleteConfirmSelection = newPos; drawEditNodeDelete(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        if (editDeleteConfirmSelection == 0) {
           if (nodes[editNodeIdx].type == 2) deleteEndpointAndReset();
           else deleteNodeAndRecalculate(editNodeIdx);
        } else {
           uiState = ST_EDIT_NODE_MENU; encoder.setPosition(0);
        }
        lastPos = -1;
      }
      break;

    case ST_INSERT_POS:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(ledCount - 1); newPos = ledCount - 1; }
        if (newPos >= ledCount) { encoder.setPosition(0); newPos = 0; }
        currentLED = newPos; currentNode.pos = currentLED;
        drawInsertPos(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        uiState = ST_INSERT_HEIGHT; encoder.setPosition(10); lastPos = -1; 
      }
      break;

    case ST_INSERT_HEIGHT:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos > 50) { encoder.setPosition(50); newPos = 50; }
        currentNode.height = newPos; drawInsertHeight(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        uiState = ST_INSERT_ART; encoder.setPosition(6); lastPos = -1; 
      }
      break;

    case ST_INSERT_ART:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= NUM_ARTEN) { encoder.setPosition(NUM_ARTEN - 1); newPos = NUM_ARTEN - 1; }
        currentNode.art = newPos;
        
        if (currentNode.art == 5) { currentNode.force = 1.2f; currentNode.length = 30; currentNode.enabled = 1; }        
        else if (currentNode.art == 6) { currentNode.force = 2.0f; currentNode.length = 10; currentNode.enabled = 1; } 
        else if (currentNode.art == 7) { currentNode.force = 25.0f; currentNode.length = 5; currentNode.enabled = 1; }  
        else { currentNode.force = 0.0f; currentNode.length = 0; currentNode.enabled = 0; }
        
        drawInsertArt(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        if (currentNode.art >= 5) {
          uiState = ST_INSERT_FORCE; encoder.setPosition((int)(currentNode.force * 10.0f));
        } else {
          insertNodeAndRecalculate();
        }
        lastPos = -1;
      }
      break;

    case ST_INSERT_FORCE:
      if (newPos != lastPos) {
        float val = (float)newPos / 10.0f;
        if (currentNode.art == 5) { 
          if (val < 0.1f) { encoder.setPosition(1); val = 0.1f; }
          if (val > 5.0f) { encoder.setPosition(50); val = 5.0f; }
        } else if (currentNode.art == 6) { 
          if (val < 0.0f) { encoder.setPosition(0); val = 0.0f; } 
          if (val > 20.0f) { encoder.setPosition(200); val = 20.0f; }
        } else if (currentNode.art == 7) { 
          if (val < 1.0f) { encoder.setPosition(10); val = 1.0f; }
          if (val > 60.0f) { encoder.setPosition(600); val = 60.0f; }
        }
        currentNode.force = val; drawInsertForce(); lastPos = newPos;
      }
      if (isButtonPressed()) { 
        uiState = ST_INSERT_LENGTH; encoder.setPosition(currentNode.length); lastPos = -1; 
      }
      break;

    case ST_INSERT_LENGTH:
      if (newPos != lastPos) {
        if (newPos < 1) { encoder.setPosition(1); newPos = 1; }
        if (newPos > ledCount / 2) { encoder.setPosition(ledCount / 2); newPos = ledCount / 2; }
        currentNode.length = newPos; drawInsertLength(); lastPos = newPos;
      }
      if (isButtonPressed()) { insertNodeAndRecalculate(); lastPos = -1; }
      break;

    case ST_PLAY:
      moveLED();
      break;

    case ST_SETUP_WARNING:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 2) {encoder.setPosition(1); newPos = 1; }
        menuSelection = newPos; drawSetupWarning(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        if (menuSelection == 0) {
          uiState = ST_SETUP_LENGTH; ledCount = 1000; strip.updateLength(ledCount);
          if (nodes != nullptr) delete[] nodes;
          nodes = new Node[ledCount];      
          nodeCount = 0; currentNode = {}; currentLED = 0;
        } else if (menuSelection == 1) { uiState = ST_FILE_SLOT_ACTION; }
        menuSelection = 0; lastPos = -1; encoder.setPosition(0); newPos = 0; currentLED = newPos;
      }
      break;

    case (ST_SETUP_LENGTH):
      if (newPos != lastPos) {
        int delta = newPos - lastPos; lengthSpeed += delta;
        lengthSpeed = constrain(lengthSpeed, -lengthMaxSpeed, lengthMaxSpeed);
        lastLengthDecel = millis();
        if (abs(lengthSpeed) == 1) {
          if (delta > 0) currentLED++; else if (delta < 0 && currentLED > 0) currentLED--;
          drawSetupLength();
        }
        lastPos = newPos;
      }
      if (lengthSpeed != 0 && millis() - lastLengthDecel >= 400) {
        lastLengthDecel = millis();
        if (lengthSpeed > 0) lengthSpeed--; else lengthSpeed++;
      }
      if (abs(lengthSpeed) >= 2) {
        unsigned long interval = 200 / abs(lengthSpeed); if (interval < 20) interval = 20;                 
        if (millis() - lastLengthMove >= interval) {
          lastLengthMove = millis();
          if (lengthSpeed > 0) currentLED++; else if (currentLED > 0) currentLED--;
          drawSetupLength();
        }
      }
      if (isButtonPressed()) {
        ledCount = currentLED + 1; 
        display.clearDisplay(); display.display();
        for(int i = currentLED; i >= 0; i--) {
          currentLED = i; strip.clear();
          strip.setPixelColor(0, strip.Color(0, 0, 100));            
          strip.setPixelColor(currentLED, strip.Color(0, 100, 0));  
          strip.setPixelColor(ledCount-1, strip.Color(0, 0, 100));
          strip.show(); delay(10);
        }
        lengthSpeed = 0; lastPos = -1; encoder.setPosition(0); newPos = 0; currentLED = newPos;
        uiState = ST_NODE_START;
      }
      break;
    
    case (ST_NODE_START):
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(ledCount-1); newPos = ledCount-1; }
        if (newPos > ledCount-1) { encoder.setPosition(0); newPos = 0; }
        currentNode.pos = newPos; currentLED = newPos; drawNodeStart(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        display.clearDisplay(); display.display(); encoder.setPosition(0); lastPos = -1; uiState = ST_NODE_HEIGHT;
      }
      break;

    case (ST_NODE_POS):
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(ledCount-1); newPos = ledCount-1; }
        if (newPos > ledCount-1) { encoder.setPosition(0); newPos = 0; }
        currentNode.pos = newPos; currentLED = newPos; drawNodePos(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        display.clearDisplay(); display.display(); encoder.setPosition(0); lastPos = -1; uiState = ST_NODE_HEIGHT;
      }
      break;
                    
    case (ST_NODE_HEIGHT):
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        currentNode.height = newPos; drawNodeHeight(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        display.clearDisplay(); display.display(); encoder.setPosition(currentNode.pos);
        lastPos = -1; currentLED = currentNode.pos;
        
        if (nodeCount == 0) {
          currentNode.type = 0; currentNode.art = 0; currentNode.force = 0; currentNode.length = 0;
          nodes[nodeCount++] = currentNode; currentNode = {}; uiState = ST_NODE_POS;
        } else {
          uiState = ST_NODE_TYPE; menuSelection = 1; encoder.setPosition(menuSelection);
        }
      }
      break;
                    
    case (ST_NODE_TYPE):
      if (newPos != lastPos) {
        if (newPos < 1) {encoder.setPosition(1); newPos = 1; }
        if (newPos > 2) {encoder.setPosition(2); newPos = 2; }
        menuSelection = newPos; currentNode.type = menuSelection; currentLED = currentNode.pos;
        drawNodeType(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        display.clearDisplay(); display.display(); encoder.setPosition(currentNode.pos);
        lastPos = -1; currentLED = currentNode.pos;

        if(currentNode.type == 2) {
          currentNode.art = 0; currentNode.force = 0; currentNode.length = 0;
          nodes[nodeCount++] = currentNode; 
          currentNode = {};
          uiState = ST_NODE_END;
        } else {
          uiState = ST_NODE_ART_SETUP; menuSelection = 0; encoder.setPosition(0);
        }
      }
      break;

    case ST_NODE_ART_SETUP:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= NUM_ARTEN) { encoder.setPosition(NUM_ARTEN - 1); newPos = NUM_ARTEN - 1; }
        menuSelection = newPos; currentNode.art = menuSelection;
        
        if (currentNode.art == 5) { currentNode.force = 1.2f; currentNode.length = 30; currentNode.enabled = 1; }        
        else if (currentNode.art == 6) { currentNode.force = 2.0f; currentNode.length = 10; currentNode.enabled = 1; }   
        else if (currentNode.art == 7) { currentNode.force = 25.0f; currentNode.length = 5; currentNode.enabled = 1; }  
        else { currentNode.force = 0.0f; currentNode.length = 0; currentNode.enabled = 0; }
        
        drawNodeArtSetup(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        display.clearDisplay(); display.display();
        if (currentNode.art >= 5) {
           uiState = ST_NODE_FORCE_SETUP; encoder.setPosition((int)(currentNode.force * 10.0f));
        } else {
           if (nodeCount < ledCount) { nodes[nodeCount++] = currentNode; currentNode = {}; }
           uiState = ST_NODE_POS; encoder.setPosition(currentLED);  
        }
        lastPos = -1;
      }
      break;

    case ST_NODE_FORCE_SETUP:
      if (newPos != lastPos) {
        float val = (float)newPos / 10.0f;
        if (currentNode.art == 5) {
           if (val < 0.1f) { encoder.setPosition(1); val = 0.1f; }
           if (val > 5.0f) { encoder.setPosition(50); val = 5.0f; }
        } else if (currentNode.art == 6) {
           if (val < 0.0f) { encoder.setPosition(0); val = 0.0f; } 
           if (val > 20.0f) { encoder.setPosition(200); val = 20.0f; }
        } else if (currentNode.art == 7) {
           if (val < 1.0f) { encoder.setPosition(10); val = 1.0f; }
           if (val > 60.0f) { encoder.setPosition(600); val = 60.0f; }
        }
        currentNode.force = val; drawNodeForceSetup(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        uiState = ST_NODE_LENGTH_SETUP; encoder.setPosition(currentNode.length); lastPos = -1;
      }
      break;

    case ST_NODE_LENGTH_SETUP:
      if (newPos != lastPos) {
        if (newPos < 1) { encoder.setPosition(1); newPos = 1; }
        if (newPos > ledCount / 2) { encoder.setPosition(ledCount / 2); newPos = ledCount / 2; }
        currentNode.length = newPos; drawNodeLengthSetup(); lastPos = newPos;
      }
      if (isButtonPressed()) {
        display.clearDisplay(); display.display();
        if (nodeCount < ledCount) { nodes[nodeCount++] = currentNode; currentNode = {}; }
        uiState = ST_NODE_POS; encoder.setPosition(currentLED); lastPos = -1;
      }
      break;

    case (ST_NODE_END):
      display.clearDisplay(); display.display(); strip.clear(); strip.show();
      encoder.setPosition(0); lastPos = -1; 
      
      sortNodesAndRecalculate();
      
      Serial.println(F("Neustart...")); delay(500); ESP.restart();
  } 
}

// ==========================================
// DYNAMISCHE SPEED-FARBE
// ==========================================
uint32_t getSpeedColor(float currentVel, uint8_t bright) {
    float v = constrain(currentVel, 0.0f, colorMaxSpeed);
    float halfMax = colorMaxSpeed / 2.0f;
    uint8_t r = 0, g = 0, b = 0;
    
    if (v <= halfMax) {
        float t = v / halfMax;
        r = (uint8_t)(t * 255.0f);
        g = 255;
    } else {
        float t = (v - halfMax) / halfMax;
        r = 255;
        g = (uint8_t)((1.0f - t) * 255.0f);
    }
    
    r = ((uint16_t)r * bright) / 255;
    g = ((uint16_t)g * bright) / 255;
    return strip.Color(r, g, b);
}

// ==========================================
// MENÜ- & DISPLAY-GRAFIKEN
// ==========================================
void drawMainMenu() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 5);  display.println(menuSelection == 0 ? "> ABSPIELEN" : "  ABSPIELEN");
    display.setCursor(0, 20); display.println(menuSelection == 1 ? "> KONFIG" : "  KONFIG");
    display.setCursor(0, 35); display.println(menuSelection == 2 ? "> STRECKE ANPASSEN" : "  STRECKE ANPASSEN");
    display.setCursor(0, 50); 
    display.print(menuSelection == 3 ? "> STRECKEN [S" : "  STRECKEN [S");
    display.print(activeLoadedSlot); display.println("]");
    display.display();
}

void drawFileSelectSlot() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("-- STRECKE WAEHLEN --"));
    int encPos = encoder.getPosition();
    if (encPos == NUM_SLOTS) { display.setCursor(0, 45); display.println(F("> ZURUECK")); } 
    else {
        display.setCursor(0, 15); display.print(F("Speicher-Slot: ")); display.println(selectedSlot);
        display.setCursor(0, 30); display.print(F("Status: "));
        if (slotExists(selectedSlot)) display.println(F("BELEGT (Daten ok)")); else display.println(F("LEER"));
        if (selectedSlot == activeLoadedSlot) { display.setCursor(0, 45); display.println(F(" (Aktuell aktiv)")); }
    }
    display.display();
}

void drawFileSlotAction() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.print(F("-- SLOT ")); display.print(selectedSlot); display.println(F(" --"));
    display.setCursor(0, 12); display.println(slotActionSelection == 0 ? "> Laden" : "  Laden");
    display.setCursor(0, 22); display.println(slotActionSelection == 1 ? "> Neu erstellen" : "  Neu erstellen");
    display.setCursor(0, 32); display.println(slotActionSelection == 2 ? "> Neu berechnen" : "  Neu berechnen");
    display.setCursor(0, 42); display.println(slotActionSelection == 3 ? "> Loeschen" : "  Loeschen");
    display.setCursor(0, 52); display.println(slotActionSelection == 4 ? "> ZURUECK" : "  ZURUECK");
    display.display();
}

void drawFileDeleteConfirm() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.print(F("Slot ")); display.print(selectedSlot); display.println(F(" wirklich"));
    display.println(F("unwiderruflich loeschen?"));
    display.setCursor(20, 40); display.println(deleteConfirmSelection == 0 ? "> JA" : "  JA");
    display.setCursor(70, 40); display.println(deleteConfirmSelection == 1 ? "> NEIN" : "  NEIN");
    display.display();
}

void drawOpticsMenu() {
    const int TOTAL_OPTIONS = 15; // NEU: Auf 15 erhöht
    const int VISIBLE_LINES = 7;
    const int LINE_HEIGHT = 8;
    const int START_Y = 9;
    
    int firstVisible = 0;
    if (opticsMenuSelection > VISIBLE_LINES - 1) {
        firstVisible = opticsMenuSelection - (VISIBLE_LINES - 1);
    }
    if (firstVisible > TOTAL_OPTIONS - VISIBLE_LINES) {
        firstVisible = TOTAL_OPTIONS - VISIBLE_LINES;
    }
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0);
    display.println(F("-- KONFIG --"));
    
    for (int i = 0; i < VISIBLE_LINES; i++) {
        int idx = firstVisible + i;
        if (idx >= TOTAL_OPTIONS) break;
        
        display.setCursor(0, START_Y + i * LINE_HEIGHT);
        bool sel = (idx == opticsMenuSelection);
        display.print(sel ? "> " : "  ");
        
        switch(idx) {
            case 0: display.print(F("W-Farbe: ")); display.println(colorNames[selectedCarColorIdx]); break;
            case 1: display.print(F("B-Farbe: ")); display.println(colorNames[selectedTrackColorIdx]); break;
            case 2: display.print(F("W-Len:   ")); display.println(carLength); break;
            case 3: display.print(F("W-Glow:  ")); display.println(carBrightness); break;
            case 4: display.print(F("B-Glow:  ")); display.println(trackBrightness); break;
            case 5: display.print(F("Z-Glow:  ")); display.println(zoneBrightness); break; 
            case 6: display.print(F("Mode:    ")); display.println(displayMode == 0 ? "Tech" : "Kirmes"); break;
            case 7: display.print(F("OLED FPS:")); display.println(displayFps); break; 
            case 8: display.print(F("MaxSpd:  ")); display.println(colorMaxSpeed, 1); break;
            case 9: display.print(F("G-Faktor:")); display.println(gForceScale, 2); break; // NEU
            case 10: display.print(F("Zonen:   ")); display.println(showZoneEffects == 0 ? "Aus" : (showZoneEffects == 1 ? "Statisch" : "Lauflicht")); break;
            case 11: display.print(F("Sound-V: ")); display.println(soundVolume); break;                        // NEU
            case 12: display.print(F("Sound:   ")); display.println(soundEnabled ? "An" : "Aus"); break;        // NEU
            case 13: display.print(F("S-Modus: ")); display.println(soundMode == 0 ? "Effekte" : "Random"); break;  // NEU
            case 14: display.println(F("ZURUECK")); break;                                                      // GEÄNDERT
        }
    }
    display.display();
}

void drawDisplayFpsSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("OLED DISPLAY FPS:"));
    display.setTextSize(3); display.setCursor(45, 25);
    display.println(displayFps);
    display.display();
}

void drawZoneBrightnessSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("ZONEN-HELLIGKEIT:"));
    display.setTextSize(3); display.setCursor(40, 25);
    display.println(zoneBrightness);
    display.display();

    strip.clear(); strip.fill(trackColor, 0, ledCount); 
    int mid = ledCount / 2;
    for(int i=-5; i<=5; i++) {
        uint16_t zPos = (mid + i + ledCount) % ledCount;
        strip.setPixelColor(zPos, strip.Color(255 * zoneBrightness / 255, 50 * zoneBrightness / 255, 0)); 
    }
    strip.setBrightness(playBrightness); strip.show();
}

void drawDisplayModeSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("DISPLAY-MODUS:"));
    display.setTextSize(2); display.setCursor(10, 30);
    display.println(displayMode == 0 ? F("Telemetrie") : F("Sprueche"));
    display.display();
}

void drawMaxSpeedSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("MAX SPEED SKALA:"));
    display.setTextSize(3); display.setCursor(30, 25);
    display.print(colorMaxSpeed, 1);
    display.display();
}

// NEU: Auswahlbildschirm für den G-Kraft Faktor
void drawGForceScaleSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("G-KRAFT FAKTOR:"));
    display.setTextSize(3); display.setCursor(20, 25);
    display.print(gForceScale, 2);
    display.display();
}

void drawZoneEffectsSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("ZONEN-DARSTELLUNG:"));
    display.setTextSize(2); display.setCursor(10, 30);
    if (showZoneEffects == 0) display.println(F("Aus"));
    else if (showZoneEffects == 1) display.println(F("Statisch"));
    else display.println(F("Lauflicht"));
    display.display();
}

void drawElementsMenu() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);  display.println(F("- STRECKE ANPASSEN -"));
  display.setCursor(0, 18); display.println(elementsMenuSelection == 0 ? "> Punkt bearbeiten" : "  Punkt bearbeiten");
  display.setCursor(0, 32); display.println(elementsMenuSelection == 1 ? "> Punkt hinzufuegen" : "  Punkt hinzufuegen");
  display.setCursor(0, 46); display.println(elementsMenuSelection == 2 ? "> ZURUECK" : "  ZURUECK");
  display.display();
}

void drawSetupLeds() {
  strip.clear();
  strip.setPixelColor(0, strip.Color(80, 80, 80)); 
  if(ledCount > 1) strip.setPixelColor(ledCount-1, strip.Color(100, 0, 0)); 
  
  for (uint16_t i = 0; i < nodeCount; i++){
    uint8_t r = 0, g = 0, b = 0;
    switch(nodes[i].type) {
      case 0: r = 80; g = 80; b = 80; break;
      case 1: 
        if(nodes[i].art == 5) { r = 0; g = 100; b = 255; }      
        else if(nodes[i].art == 6) { r = 255; g = 50; b = 0; }  
        else if(nodes[i].art == 7) { r = 255; g = 255; b = 0; } 
        else { r = 100; g = 0; b = 100; }                       
        break;
      case 2: r = 100; g = 0; b = 0; break;
    }
    
    bool dim = (nodes[i].art >= 5 && !nodes[i].enabled);
    if (dim) { r /= 5; g /= 5; b /= 5; }
    
    if (nodes[i].art >= 5 && nodes[i].length > 0) {
        uint16_t zBright = dim ? (zoneBrightness / 5) : zoneBrightness;
        for (uint16_t l = 0; l < nodes[i].length; l++) {
            uint16_t zPos = (nodes[i].pos + l) % ledCount;
            strip.setPixelColor(zPos, strip.Color((r * zBright)/255, (g * zBright)/255, (b * zBright)/255)); 
        }
    }
    strip.setPixelColor(nodes[i].pos, strip.Color(r, g, b));
  }
  
  if (uiState == ST_NODE_LENGTH_SETUP || uiState == ST_INSERT_LENGTH || uiState == ST_EDIT_NODE_LENGTH) {
      uint16_t basePos = (uiState == ST_EDIT_NODE_LENGTH) ? nodes[editNodeIdx].pos : currentNode.pos;
      uint8_t  baseArt = (uiState == ST_EDIT_NODE_LENGTH) ? nodes[editNodeIdx].art : currentNode.art;
      uint16_t baseLen = (uiState == ST_EDIT_NODE_LENGTH) ? nodes[editNodeIdx].length : currentNode.length;
      
      uint8_t r = 0, g = 0, b = 0;
      if(baseArt == 5) { r = 0; g = 100; b = 255; }      
      else if(baseArt == 6) { r = 255; g = 50; b = 0; }  
      else if(baseArt == 7) { r = 255; g = 255; b = 0; } 
      else { r = 100; g = 0; b = 100; }
      
      for (uint16_t l = 0; l < baseLen; l++) {
          uint16_t zPos = (basePos + l) % ledCount;
          strip.setPixelColor(zPos, strip.Color((r * zoneBrightness)/255, (g * zoneBrightness)/255, (b * zoneBrightness)/255)); 
      }
  }
  
  if (uiState == ST_SETUP_LENGTH || uiState == ST_NODE_START || uiState == ST_NODE_POS || uiState == ST_NODE_ART_SETUP || uiState == ST_NODE_FORCE_SETUP || uiState == ST_NODE_LENGTH_SETUP || uiState == ST_INSERT_POS || uiState == ST_INSERT_HEIGHT || uiState == ST_INSERT_ART || uiState == ST_INSERT_FORCE || uiState == ST_INSERT_LENGTH) {
     strip.setPixelColor(currentLED, strip.Color(0, 255, 0)); 
  }
  if (uiState == ST_EDIT_NODE_POS) {
     strip.setPixelColor(nodes[editNodeIdx].pos, strip.Color(0, 255, 0));
  }

  strip.show();
}

void drawEditSelectNode() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("-- ELEMENT WAEHLEN --"));
  if (editNodeIdx == nodeCount) { display.setCursor(0, 30); display.println(F("> ZURUECK & SPEICHERN")); } 
  else {
    Node n = nodes[editNodeIdx];
    display.setCursor(0, 12); display.print(F("Elem: ")); display.print(editNodeIdx + 1); display.print(F(" / ")); display.println(nodeCount);
    display.setCursor(0, 22); display.print(F("Typ:  "));
    if (n.type == 0) display.println(F("Startpunkt")); else if (n.type == 1) display.println(F("Scheitelpunkt")); else display.println(F("Endpunkt"));
    display.setCursor(0, 32); display.print(F("Art:  ")); display.println(elementArtNames[n.art]);
    display.setCursor(0, 42); display.print(F("H: ")); display.print(n.height); 
    if (n.art >= 5) { 
        display.print(F("  F: ")); display.print(n.force, 1); 
        display.print(F(" L: ")); display.print(n.length);
        if (!n.enabled) { display.print(F(" OFF")); }
    }
    display.setCursor(0, 52); display.print(F("LED-Pos: ")); display.println(n.pos);
  }
  display.display();

  strip.clear(); strip.fill(trackColor, 0, ledCount); 
  if (editNodeIdx < nodeCount) {
    int targetLed = nodes[editNodeIdx].pos;
    
    strip.setPixelColor(targetLed, strip.Color(0, 255, 0)); 
    strip.setPixelColor((targetLed + 1) % ledCount, strip.Color(0, 60, 0));
    strip.setPixelColor((targetLed + 2) % ledCount, strip.Color(0, 10, 0));
  }
  strip.setBrightness(playBrightness); strip.show();
}

void drawEditNodeMenu() {
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.printf("ELEM %d SETUP:", editNodeIdx + 1);

    bool isStart = (nodes[editNodeIdx].type == 0);
    bool hasFunc = (nodes[editNodeIdx].art >= 5);
    bool canDel  = (nodes[editNodeIdx].type != 0);

    struct MItem { const char* l; String v; };
    MItem m[10]; int c = 0;

    if (!isStart) m[c++] = {"Pos", String(nodes[editNodeIdx].pos)};
    m[c++] = {"Hoehe", String(nodes[editNodeIdx].height)};
    m[c++] = {"Art", String(elementArtNames[nodes[editNodeIdx].art])};
    if (hasFunc) {
        m[c++] = {"Wert", String(nodes[editNodeIdx].force, 1)};
        m[c++] = {"Len", String(nodes[editNodeIdx].length)};
        m[c++] = {"Aktiv", nodes[editNodeIdx].enabled ? "JA" : "NEIN"};
    }
    if (canDel) m[c++] = {"LOESCHEN", ""};
    m[c++] = {"ZURUECK", ""};

    int VIS = 6; int first = 0;
    if (editMenuSelection >= VIS) first = editMenuSelection - VIS + 1;

    for (int i = 0; i < VIS; i++) {
        int idx = first + i; if (idx >= c) break;
        display.setCursor(0, 12 + i * 8);
        display.print(idx == editMenuSelection ? "> " : "  ");
        display.print(m[idx].l);
        if (m[idx].v != "") { display.print(": "); display.print(m[idx].v); }
    }
    display.display();
}

void drawEditNodeDelete() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print(F("Element ")); display.print(editNodeIdx + 1); display.println(F(" wirklich"));
  if (nodes[editNodeIdx].type == 2) display.println(F("loeschen/neu setzen?"));
  else display.println(F("aus Strecke loeschen?"));
  display.setCursor(20, 35); display.println(editDeleteConfirmSelection == 0 ? "> JA" : "  JA");
  display.setCursor(70, 35); display.println(editDeleteConfirmSelection == 1 ? "> NEIN" : "  NEIN");
  display.display();
}

void drawEditNodePos() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print(F("Elem ")); display.print(editNodeIdx + 1); display.println(F(": POSITION"));
  display.setTextSize(3); display.setCursor(35, 25); display.println(nodes[editNodeIdx].pos); 
  display.display(); drawSetupLeds();
}

void drawEditNodeHeight() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print(F("Elem ")); display.print(editNodeIdx + 1); display.println(F(": HOEHE"));
  display.setTextSize(3); display.setCursor(45, 25); display.println(nodes[editNodeIdx].height); display.display();
}

void drawEditNodeArt() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print(F("Elem ")); display.print(editNodeIdx + 1); display.println(F(": ART"));
  display.setTextSize(2); display.setCursor(10, 30); display.println(elementArtNames[nodes[editNodeIdx].art]); display.display();
}

void drawEditNodeForce() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print(F("Elem ")); display.print(editNodeIdx + 1); 
  if (nodes[editNodeIdx].art == 5) display.println(F(": LIFT SPEED")); 
  else if (nodes[editNodeIdx].art == 6) display.println(F(": ZIEL SPEED (m/s)")); 
  else display.println(F(": BOOSTER SCHUB"));
  display.setTextSize(2); display.setCursor(20, 30); display.println(nodes[editNodeIdx].force, 1); display.display();
}

void drawEditNodeLength() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.print(F("Elem ")); display.print(editNodeIdx + 1); display.println(F(": ZONENLAENGE"));
  display.setTextSize(3); display.setCursor(35, 25); display.println(nodes[editNodeIdx].length); display.display(); drawSetupLeds();
}

void drawInsertPos() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("NEUES ELEMENT:"));
  display.setCursor(0, 15); display.println(F("LED-Position waehlen:"));
  display.setTextSize(3); display.setCursor(35, 30); display.println(currentLED); display.display(); drawSetupLeds(); 
}

void drawInsertHeight() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("NEUES ELEMENT:"));
  display.setCursor(0, 15); display.println(F("Hoehe festlegen:"));
  display.setTextSize(3); display.setCursor(45, 30); display.println(currentNode.height); display.display(); drawSetupLeds();
}

void drawInsertArt() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("NEUES ELEMENT:"));
  display.setCursor(0, 15); display.println(F("Art waehlen:"));
  display.setTextSize(2); display.setCursor(10, 35); display.println(elementArtNames[currentNode.art]); display.display(); drawSetupLeds();
}

void drawInsertForce() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); 
  if (currentNode.art == 5) display.println(F("LIFT SPEED (m/s):"));
  else if (currentNode.art == 6) display.println(F("ZIEL SPEED (m/s):")); 
  else display.println(F("BOOSTER SCHUB"));
  display.setTextSize(2); display.setCursor(20, 30); display.println(currentNode.force, 1); display.display(); drawSetupLeds();
}

void drawInsertLength() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("ZONENLAENGE (LEDs):"));
  display.setTextSize(3); display.setCursor(35, 25); display.println(currentNode.length); display.display(); drawSetupLeds();
}

void drawNodePos() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("LED zum folgenden\nScheitelpunkt bewegen\nund Encoder druecken."));
  display.display(); drawSetupLeds();
}

void drawNodeHeight() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("Hoehe einstellen und den Encoder druecken:"));
  display.setTextSize(3); display.setCursor(64, 32); display.println(currentNode.height);
  display.display(); drawSetupLeds();
}

void drawNodeType() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("Art des Punktes\nbestimmen:"));
  display.setCursor(0, 20); display.println(menuSelection == 1 ? "> Scheitelpunkt" : "  Scheitelpunkt");
  display.setCursor(0, 30); display.println(menuSelection == 2 ? "> Endpunkt" : "  Endpunkt");
  display.display(); drawSetupLeds();
}

void drawNodeArtSetup() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("Art des Elements:"));
  display.setTextSize(2); display.setCursor(10, 25); display.println(elementArtNames[currentNode.art]);
  display.display(); drawSetupLeds();
}

void drawNodeForceSetup() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); 
  if (currentNode.art == 5) display.println(F("LIFT SPEED (m/s):")); 
  else if (currentNode.art == 6) display.println(F("ZIEL SPEED (m/s):")); 
  else display.println(F("BOOSTER SCHUB:"));
  display.setTextSize(2); display.setCursor(20, 25); display.println(currentNode.force, 1);
  display.display(); drawSetupLeds();
}

void drawNodeLengthSetup() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("ZONENLAENGE (LEDs):"));
  display.setTextSize(3); display.setCursor(35, 25); display.println(currentNode.length);
  display.display(); drawSetupLeds();
}

void drawColorSelection(bool isCar) {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); if (isCar) display.println(F("WAGENFARBE Waehlen:")); else display.println(F("BAHNFARBE Waehlen:"));
    display.setTextSize(2); display.setCursor(10, 30);
    
    uint8_t selectedIdx = isCar ? selectedCarColorIdx : selectedTrackColorIdx;
    display.println(colorNames[selectedIdx]);
    display.display();

    strip.clear(); 
    if (selectedIdx == 8) {
        int mid = ledCount / 2;
        for(int i=0; i<carLength; i++) {
            float simV = (float)i / (float)(carLength - 1) * colorMaxSpeed;
            strip.setPixelColor((mid + i)%ledCount, getSpeedColor(simV, isCar ? carBrightness : trackBrightness));
        }
    } else {
        strip.fill(trackColor, 0, ledCount); 
        int mid = ledCount / 2;
        for(int i=0; i<carLength; i++) strip.setPixelColor((mid + i)%ledCount, carColor);
    }
    strip.setBrightness(playBrightness); strip.show();
}

void drawCarLengthSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("WAGENLAENGE (LEDs):"));
    display.setTextSize(3); display.setCursor(45, 25); display.println(carLength);
    display.display();

    strip.clear(); strip.fill(trackColor, 0, ledCount); 
    int mid = ledCount / 2;
    for(int i=0; i<carLength; i++) strip.setPixelColor((mid + i)%ledCount, carColor);
    strip.setBrightness(playBrightness); strip.show();
}

void drawBrightnessSelection(bool isCar) {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); 
    if (isCar) display.println(F("WAGEN-HELLIGKEIT:")); else display.println(F("BAHN-HELLIGKEIT:"));
    display.setTextSize(3); display.setCursor(40, 25);
    if (isCar) display.println(carBrightness); else display.println(trackBrightness);
    display.display();

    strip.clear(); strip.fill(trackColor, 0, ledCount); 
    int mid = ledCount / 2;
    for(int i=0; i<carLength; i++) strip.setPixelColor((mid + i)%ledCount, carColor);
    strip.setBrightness(playBrightness); strip.show();
}

void updateActualColors() {
    if (selectedCarColorIdx < 8) {
        uint8_t cr = (colorPalette[selectedCarColorIdx] >> 16) * carBrightness / 255;
        uint8_t cg = (colorPalette[selectedCarColorIdx] >> 8)  * carBrightness / 255;
        uint8_t cb = (colorPalette[selectedCarColorIdx])       * carBrightness / 255;
        carColor = strip.Color(cr, cg, cb);
    }
    if (selectedTrackColorIdx < 8) {
        uint8_t tr = (colorPalette[selectedTrackColorIdx] >> 16) * trackBrightness / 255;
        uint8_t tg = (colorPalette[selectedTrackColorIdx] >> 8)  * trackBrightness / 255;
        uint8_t tb = (colorPalette[selectedTrackColorIdx])       * trackBrightness / 255;
        trackColor = strip.Color(tr, tg, tb);
    }
}

void drawSetupWarning() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println("Strecke in Slot " + String(activeLoadedSlot));
  display.println(F("wird ueberschrieben!"));
  display.setCursor(0, 40); display.println(menuSelection == 0 ? "> FORTFAHREN" : "  FORTFAHREN");
  display.setCursor(0, 52); display.println(menuSelection == 1 ? "> ABBRECHEN" : "  ABBRECHEN");
  display.display();
}

void drawSetupLength() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("LED-Cursor mit dem\nDrehencoder zu der\nletzten LED bewegen\nund druecken."));
  display.display(); drawSetupLeds();
}

void drawNodeStart() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0); display.println(F("LED zum hoechsten\nPunkt der Strecke\nbewegen und den\nEncoder druecken."));
  display.display(); drawSetupLeds();
}

// NEU: Auswahlbildschirm für die Lautstärke
void drawSoundVolumeSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("SOUND-LAUTSTAERKE:"));
    if (!dfpReady) { display.setCursor(0, 55); display.println(F("(Player nicht bereit)")); }
    display.setTextSize(3); display.setCursor(45, 25);
    display.println(soundVolume);
    display.display();
}

// NEU: Auswahlbildschirm für Sound An/Aus
void drawSoundEnabledSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("SOUND:"));
    display.setTextSize(3); display.setCursor(35, 25);
    display.println(soundEnabled ? F("An") : F("Aus"));
    display.display();
}

// NEU: Auswahlbildschirm für den Sound-Modus
void drawSoundModeSelection() {
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
    display.setCursor(0, 0); display.println(F("SOUND-MODUS:"));
    display.setTextSize(2); display.setCursor(10, 25);
    display.println(soundMode == 0 ? F("Effekte") : F("Random"));
    display.setTextSize(1); display.setCursor(0, 50);
    if (soundMode == 1 && randomFolderCount == 0) display.println(F("Ordner 02 ist leer!"));
    display.display();
}

// ------------------------------------------
// RANDOMISIERTE JAHRMARKTSPRÜCHE
// ------------------------------------------
String getRandomSpruch(int art, String status) {
    static String cachedSpruch = "";
    static String lastCat = "";
    String currentCat = status + "_" + String(art);

    if (currentCat != lastCat) {
        lastCat = currentCat;
        int r = random(0, 3);
        
        if (status == "KETTENLIFT") {
            if(r == 0) cachedSpruch = F("Klick-Klack... Ganz\nentspannt dem Gipfel\nentgegen!");
            else if(r == 1) cachedSpruch = F("Aussicht geniessen!\nGleich geht die\nPost ab!");
            else cachedSpruch = F("Ruhig Blut... Noch\nkannst du winken!");
        } else if (status == "BOOSTER") {
            if(r == 0) cachedSpruch = F("ZUENDUNG!\nVoller Schub voraus!");
            else if(r == 1) cachedSpruch = F("Festhalten!\nKatapult-Launch\naktiviert!");
            else cachedSpruch = F("Raketen-Modus!\nSchubkraft aufs\nMaximum!");
        } else if (status == "BREMSEN") {
            if(r == 0) cachedSpruch = F("Zischhh...\nBremsen greifen!");
            else if(r == 1) cachedSpruch = F("Voll in die Eisen!\nBitte recht freundlich\nfuers Foto!");
            else cachedSpruch = F("Sanfte Landung!\nSchwung wird exakt\nabgebaut!");
        } else if (status == "AUSROLLEN") {
            if(r == 0) cachedSpruch = F("Haende hoch!\nSchwung in den Lift\nmitnehmen!");
            else if(r == 1) cachedSpruch = F("Auslaufen...\nEnergie fuer die\nnaechste Runde!");
            else cachedSpruch = F("Freilauf aktiv!\nDen Berg elegant\nerklimmen!");
        } else {
            switch (art) {
              case 0:
                if(r == 0) cachedSpruch = F("Gleich gehts hinab!\nAirtime voraus!");
                else if(r == 1) cachedSpruch = F("Schwerelosigkeit!\nDer Magen bleibt\noben!");
                else cachedSpruch = F("Gipfelstuermer!\nHaende in die Hoehe!");
                break;
              case 1:
                if(r == 0) cachedSpruch = F("Kopfueber ins Glueck!\nLooping in Sicht!");
                else if(r == 1) cachedSpruch = F("360 Grad Wahnsinn!\nDie Welt steht\nkopf!");
                else cachedSpruch = F("Zentrifugalkraft!\nVolle Pulle durch\nden Looping!");
                break;
              case 2:
                if(r == 0) cachedSpruch = F("Schwindelgefahr!\nAb in die Helix!");
                else if(r == 1) cachedSpruch = F("Korkenzieher!\nDrehwurm garantiert!");
                else cachedSpruch = F("Spirale des Gluecks!\nImmer im Kreis\nherum!");
                break;
              case 3:
                if(r == 0) cachedSpruch = F("Mit Vollgas in die\nSteilkurve!\nFesthalten!");
                else if(r == 1) cachedSpruch = F("Extreme Querlage!\nKurvendrift der\nExtraklasse!");
                else cachedSpruch = F("Wie auf Schienen!\nSchraeglage am\nLimit!");
                break;
              case 4:
                if(r == 0) cachedSpruch = F("Talsohle naht!\nVolle G-Kraefte!");
                else if(r == 1) cachedSpruch = F("Maximaler Druck!\nUnten durch mit\nVollgas!");
                else cachedSpruch = F("Tiefpunkt erreicht!\nJetzt drueckts dich\nin den Sitz!");
                break;
              case 5:
                if(r == 0) cachedSpruch = F("Gleich gibts neuen\nSchub an der Kette!");
                else if(r == 1) cachedSpruch = F("Boxenstopp am Hang!\nDer Lift wartet\nschon!");
                else cachedSpruch = F("Aufzug in Sicht!\nNachladen fuer die\nnaechste Hoehe!");
                break;
              case 6:
                if(r == 0) cachedSpruch = F("Bremszone in Sicht!\nBitte laecheln fuers\nZielfoto!");
                else if(r == 1) cachedSpruch = F("Endstation Sehnsucht!\nGleich greifen die\nSchwerter!");
                else cachedSpruch = F("Abkuehlung naht!\nDie Bremslamellen\nwarten!");
                break;
              case 7:
                if(r == 0) cachedSpruch = F("Booster-Launch naht!\nBereit zum Abheben?");
                else if(r == 1) cachedSpruch = F("Katapult in Sicht!\nGleich zuendet die\nnaechste Stufe!");
                else cachedSpruch = F("Beschleuniger voraus!\nVolle Power!");
                break;
              default:
                cachedSpruch = F("Die wilde Fahrt\nbeginnt!");
                break;
            }
        }
    }
    return cachedSpruch;
}

// ==========================================
// TELEMETRIE & ZIEL-LOGIK (AKTIV/ZIEL)
// ==========================================
void drawTelemetry(int currentLed, float currentVel, float currentAcc, String status) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    int activeIdx = -1;
    int nextIdx = -1;
    float minDist = (float)ledCount + 10.0f;
    float p_curr = pos; 

    // 1. Check: Sind wir GERADE IN einer Zone?
    for (int i = 0; i < nodeCount; i++) {
        if (nodes[i].art >= 5) {
            float n_rel = (nodes[i].pos >= startLedPos) ? (float)(nodes[i].pos - startLedPos) : (float)((ledCount - startLedPos) + nodes[i].pos);
            float distToStart = p_curr - n_rel;
            if (distToStart < 0) distToStart += ledCount;
            if (distToStart <= nodes[i].length && nodes[i].length > 0) { 
                activeIdx = i; 
                break; 
            }
        }
    }

    // 2. Nächstes Ziel suchen
    if (activeIdx == -1) {
        for (int i = 0; i < nodeCount; i++) {
            float n_rel = (nodes[i].pos >= startLedPos) ? (float)(nodes[i].pos - startLedPos) : (float)((ledCount - startLedPos) + nodes[i].pos);
            float dist = (n_rel >= p_curr) ? (n_rel - p_curr) : ((ledCount - p_curr) + n_rel);
            if (dist > 0.5f && dist < minDist) { 
                minDist = dist; 
                nextIdx = i; 
            }
        }
    }

    int displayIdx = (activeIdx != -1) ? activeIdx : nextIdx;
    if (displayIdx == -1 && nodeCount > 0) displayIdx = 0;
    
    int art = (displayIdx != -1) ? nodes[displayIdx].art : 0;

    if (displayMode == 1) { // Kirmes Modus
        display.setTextSize(1);
        display.setCursor(0, 0); display.println(F("-- JAHRMARKT LIVE --"));
        display.setCursor(0, 14); display.printf("Spd: %.1f G: %.1f", currentVel, currentGForce);
        display.setCursor(0, 32); display.println(getRandomSpruch(art, status));
    } else { // Telemetrie Modus 
        display.setTextSize(1);
        display.setCursor(0, 0); display.println(F("-- TELEMETRIE --"));
        display.setCursor(0, 10); display.print(F("Pos: ")); display.print(currentLed); display.print(F("/")); display.println(ledCount);
        display.setCursor(0, 19); display.printf("Spd m/s: %.1f (%.1f)", currentVel, maxReachedSpeed);
        
        // --- HIER DIE ÄNDERUNG FÜR DIE AIRTIME G-KRÄFTE ---
        display.setCursor(0, 28); display.printf("G: %.1f (%.1f/%.1f)", currentGForce, maxGForce, minGForce);
        
        display.setCursor(0, 37); display.print(F("Status: ")); display.println(status);

        display.setCursor(0, 46);
        if (activeIdx != -1) display.print(F("Aktiv: Elem")); else display.print(F("Ziel: Elem"));
        if (displayIdx != -1) {
            display.print(displayIdx + 1); display.print(F(" (")); display.print(elementArtNames[art]); display.println(F(")"));
        } else display.println(F("-"));

        display.setCursor(0, 55);
        if (displayIdx != -1) {
            display.print(F("Led: ")); display.print(nodes[displayIdx].pos);
            display.print(F(" H: ")); display.println(nodes[displayIdx].height);
        }
    }
    display.display();
}

// ==========================================
// BERECHNUNGEN (BEZIER & PHYSIK)
// ==========================================
point2D lerp(point2D a, point2D b, float t) { return { a.x + t * (b.x - a.x), a.y + t * (b.y - a.y) }; }
point2D bezier(point2D p1, point2D c1, point2D c2, point2D p2, float t) {
  point2D a = lerp(p1, c1, t); point2D b = lerp(c1, c2, t); point2D c = lerp(c2, p2, t);
  point2D d = lerp(a, b, t); point2D e = lerp(b, c, t); return lerp(d, e, t);
}
float findA();

void calculations() {
  if (nodes == nullptr || nodeCount == 0) return;
  if (points != nullptr) { delete[] points; points = nullptr; }
  points = new point2D[ledCount + 2];     
  points[0] = {0, (float)nodes[0].height}; pointCount = 1;                 
  
  for(int i = 0; i < nodeCount - 1; i++) {
    int posCurrent = nodes[i].pos; int posNext = nodes[i+1].pos;
    if (posNext >= posCurrent) targetLength = posNext - posCurrent; else targetLength = (ledCount - posCurrent) + posNext;
    startHeight = nodes[i].height; endHeight = nodes[i+1].height;
    float a = findA(); points[i+1] = {points[i].x+a, (float)nodes[i+1].height}; pointCount++;
  }
  
  int posLast = nodes[nodeCount-1].pos; int posFirst = nodes[0].pos;
  if (posFirst >= posLast) targetLength = posFirst - posLast; else targetLength = (ledCount - posLast) + posFirst;
  startHeight  = nodes[nodeCount - 1].height; endHeight    = nodes[0].height;
  float a = findA(); points[pointCount] = {points[pointCount - 1].x+a, (float)nodes[0].height}; pointCount++; 
}

float bezierLength(float aValue){
  p1 = {0, (float)startHeight}; c1 = {aValue / 2.0f, (float)startHeight};
  c2 = {aValue / 2.0f, (float)endHeight}; p2 = {aValue, (float)endHeight};
  float length = 0; point2D prev = bezier(p1, c1, c2, p2, 0);
  for (int i = 0; i < steps; i++){
    float t = (float)i / (steps - 1); point2D curr = bezier(p1, c1, c2, p2, t);
    float dx = curr.x - prev.x; float dy = curr.y - prev.y; length += sqrt(dx*dx + dy*dy); prev = curr;
  }
  return length;
}

float findA(){
  float low = 0; float high = targetLength;      
  while ((high - low) > tolerance){
    float mid = (low + high) / 2.0f; float len = bezierLength(mid);
    if (len > targetLength) high = mid; else low = mid;
  }
  return (low + high) / 2.0f;
}

void bezierCalculations() {
  if (pointCount < 2) return;
  bezierPointCount = 0; if (bezierPoints != nullptr) delete[] bezierPoints;
  bezierPoints = new point2D[(pointCount - 1) * intSteps];
  for (int i = 0; i < pointCount - 1; i++) {
    p1 = points[i]; p2 = points[i + 1]; float midX = (p1.x + p2.x) / 2.0f;
    c1 = { midX, p1.y }; c2 = { midX, p2.y };
    for (int j = 0; j < intSteps; j++) {
      float t = (float)j / (intSteps - 1); point2D bp = bezier(p1, c1, c2, p2, t);
      if (i > 0 && j == 0) continue; bezierPoints[bezierPointCount++] = bp;
    }
  }
}

// ==========================================
// SEGMENT & G-KRAFT BERECHNUNG
// ==========================================
void segmentCalculations() {
  if (bezierPointCount < 2) return;
  segmentCount = bezierPointCount; 
  if (segments != nullptr) delete[] segments;
  segments = new SegmentData[segmentCount];
  
  for (int i = 0; i < segmentCount - 1; i++) {
    float dx = bezierPoints[i + 1].x - bezierPoints[i].x; 
    float dy = bezierPoints[i + 1].y - bezierPoints[i].y;
    float dist = sqrt(dx * dx + dy * dy); 
    float acc = 0.0f;
    if (dist > 0.0001f) acc = (-dy / dist) * 9.81f;
    
    segments[i].distance = (i == 0) ? dist : (segments[i-1].distance + dist);
    segments[i].acc = acc;
    segments[i].angle = atan2(dy, dx);
  }
  
  int last = segmentCount - 1; 
  float dx_last = ledCount - bezierPoints[last].x; 
  if (dx_last < 0.1f) dx_last = 0.1f;
  float dy_last = bezierPoints[0].y - bezierPoints[last].y; 
  float dist_last = sqrt(dx_last * dx_last + dy_last * dy_last);
  float acc_last = 0.0f;
  if (dist_last > 0.0001f) acc_last = (-dy_last / dist_last) * 9.81f;
  
  segments[last].distance = ledCount; 
  segments[last].acc = acc_last;
  segments[last].angle = atan2(dy_last, dx_last);

  for (int i = 0; i < segmentCount; i++) {
    float prevAngle = (i == 0) ? segments[segmentCount - 1].angle : segments[i - 1].angle;
    float currAngle = segments[i].angle;
    float dAngle = currAngle - prevAngle;
    
    while (dAngle > PI) dAngle -= 2.0f * PI;
    while (dAngle < -PI) dAngle += 2.0f * PI;
    
    float dist = (i == 0) ? segments[0].distance : (segments[i].distance - segments[i - 1].distance);
    
    if (dist > 0.0001f) segments[i].curvature = dAngle / dist;
    else segments[i].curvature = 0.0f;
  }
}

void sortNodesAndRecalculate() {
  if (nodeCount == 0) return;

  uint16_t targetPos = nodes[editNodeIdx].pos;
  uint8_t targetArt  = nodes[editNodeIdx].art;

  uint16_t refStartPos = startLedPos;
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].type == 0) {
      refStartPos = nodes[i].pos;
      break;
    }
  }
  startLedPos = refStartPos;

  for (int i = 0; i < nodeCount - 1; i++) {
    for (int j = 0; j < nodeCount - i - 1; j++) {
      float distJ = (nodes[j].pos >= refStartPos) ? (nodes[j].pos - refStartPos) : ((ledCount - refStartPos) + nodes[j].pos);
      float distNext = (nodes[j + 1].pos >= refStartPos) ? (nodes[j + 1].pos - refStartPos) : ((ledCount - refStartPos) + nodes[j + 1].pos);
      
      if (distJ > distNext) {
        Node temp = nodes[j];
        nodes[j] = nodes[j + 1];
        nodes[j + 1] = temp;
      }
    }
  }

  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].pos == targetPos && nodes[i].art == targetArt) {
      editNodeIdx = i;
      break;
    }
  }

  if (nodeCount > 0) nodes[0].type = 0;
  if (nodeCount > 1) nodes[nodeCount - 1].type = 2;
  for (int i = 1; i < nodeCount - 1; i++) {
    nodes[i].type = 1;
  }

  startLedPos = nodes[0].pos;

  calculations(); 
  bezierCalculations(); 
  segmentCalculations();
  savePlayDataSlot(activeLoadedSlot);
}

void insertNodeAndRecalculate() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20); display.println(F("Fuege Element ein\n& berechne neu..."));
  display.display();

  currentNode.type = 1; 
  nodes[nodeCount++] = currentNode;

  sortNodesAndRecalculate();

  delay(800);
  uiState = ST_ELEMENTS_MENU; encoder.setPosition(1);
}

void deleteNodeAndRecalculate(int idx) {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20); display.println(F("Loesche Knoten\n& berechne neu..."));
  display.display();

  if (nodeCount == 0) {
    delay(500);
    uiState = ST_ELEMENTS_MENU; encoder.setPosition(0);
    return;
  }

  for (int i = idx; i < nodeCount - 1; i++) {
      nodes[i] = nodes[i + 1];
  }
  nodeCount--; 

  if (nodeCount > 0) {
      sortNodesAndRecalculate();
  } else {
      savePlayDataSlot(activeLoadedSlot);
  }

  delay(800);
  uiState = ST_EDIT_SELECT_NODE; 
  if (nodeCount == 0) editNodeIdx = 0;
  else if (editNodeIdx >= nodeCount) editNodeIdx = nodeCount - 1;
  encoder.setPosition(editNodeIdx);
}

void deleteEndpointAndReset() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20); display.println(F("Endpunkt geloescht.\nBitte neu setzen..."));
  display.display();
  delay(1200);

  if (nodeCount > 1) nodeCount--;

  currentNode = {};
  currentNode.type = 2; 
  currentNode.art = 0;
  currentNode.force = 0;
  currentNode.length = 0;
  
  if (nodeCount > 0) {
    currentLED = nodes[nodeCount - 1].pos + 5;
    if (currentLED >= ledCount) currentLED = ledCount - 1; 
  } else {
    currentLED = 0;
  }

  uiState = ST_NODE_POS;
  encoder.setPosition(currentLED);
}

void triggerManualRecalc() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20); display.println(F("Berechne Geometrie\nfrisch durch..."));
  display.display();
  
  if (nodeCount > 0) {
      uint16_t refStartPos = startLedPos;
      for (int i = 0; i < nodeCount; i++) {
        if (nodes[i].type == 0) { refStartPos = nodes[i].pos; break; }
      }
      startLedPos = refStartPos;
      
      for (int i = 0; i < nodeCount - 1; i++) {
        for (int j = 0; j < nodeCount - i - 1; j++) {
          float distJ = (nodes[j].pos >= refStartPos) ? (nodes[j].pos - refStartPos) : ((ledCount - refStartPos) + nodes[j].pos);
          float distNext = (nodes[j + 1].pos >= refStartPos) ? (nodes[j + 1].pos - refStartPos) : ((ledCount - refStartPos) + nodes[j + 1].pos);
          if (distJ > distNext) {
            Node temp = nodes[j]; nodes[j] = nodes[j + 1]; nodes[j + 1] = temp;
          }
        }
      }
      nodes[0].type = 0;
      if (nodeCount > 1) nodes[nodeCount - 1].type = 2;
      for (int i = 1; i < nodeCount - 1; i++) nodes[i].type = 1;
      startLedPos = nodes[0].pos;
  }
  
  calculations(); 
  bezierCalculations(); 
  segmentCalculations();
  savePlayDataSlot(activeLoadedSlot);
  
  delay(1000);
  uiState = ST_FILE_SLOT_ACTION; 
  encoder.setPosition(2); 
}

// ==========================================
// DATEISYSTEM (SLOTS)
// ==========================================
String getSlotFilename(int slot) {
    return "/track_v5_" + String(slot) + ".bin";
}

bool slotExists(int slot) {
    return LittleFS.exists(getSlotFilename(slot));
}

// File-Format-Versionierung
const uint16_t FILE_MAGIC   = 0xABCD;
const uint8_t  FORMAT_VER   = 6; // V6: Enthält jetzt auch soundMode

void savePlayDataSlot(int slot) {
    String filename = getSlotFilename(slot);
    File f = LittleFS.open(filename, "w");
    if (!f) return;

    uint16_t magic = FILE_MAGIC;
    uint8_t  ver   = FORMAT_VER;
    f.write((uint8_t*)&magic, sizeof(magic));
    f.write((uint8_t*)&ver, sizeof(ver));

    f.write((uint8_t*)&ledCount, sizeof(ledCount));
    f.write((uint8_t*)&startLedPos, sizeof(startLedPos));
    f.write((uint8_t*)&selectedCarColorIdx, sizeof(selectedCarColorIdx));
    f.write((uint8_t*)&selectedTrackColorIdx, sizeof(selectedTrackColorIdx));
    f.write((uint8_t*)&carLength, sizeof(carLength)); 
    f.write((uint8_t*)&carBrightness, sizeof(carBrightness));      
    f.write((uint8_t*)&trackBrightness, sizeof(trackBrightness)); 
    f.write((uint8_t*)&displayMode, sizeof(displayMode)); 
    f.write((uint8_t*)&colorMaxSpeed, sizeof(colorMaxSpeed)); 
    f.write((uint8_t*)&showZoneEffects, sizeof(showZoneEffects));
    
    f.write((uint8_t*)&nodeCount, sizeof(nodeCount));
    if (nodeCount > 0 && nodes != nullptr) {
        f.write((uint8_t*)nodes, sizeof(Node) * nodeCount);
    }
    
    f.write((uint8_t*)&segmentCount, sizeof(segmentCount));
    if (segmentCount > 0 && segments != nullptr) {
        f.write((uint8_t*)segments, sizeof(SegmentData) * segmentCount);
    }
    
    f.write((uint8_t*)&zoneBrightness, sizeof(zoneBrightness));
    f.write((uint8_t*)&displayFps, sizeof(displayFps)); 
    
    f.write((uint8_t*)&gForceScale, sizeof(gForceScale)); // NEU

    f.write((uint8_t*)&soundVolume, sizeof(soundVolume));   // NEU (V5)
    f.write((uint8_t*)&soundEnabled, sizeof(soundEnabled)); // NEU (V5)
    f.write((uint8_t*)&soundMode, sizeof(soundMode));       // NEU (V6)
    
    f.close();
}

bool loadPlayDataSlot(int slot) {
    String filename = getSlotFilename(slot);
    if (!LittleFS.exists(filename)) return false;
    File f = LittleFS.open(filename, "r");
    if (!f) return false;

    if (f.size() < 10) { f.close(); return false; }

    bool isV2 = false;
    uint8_t loadedVer = 1;
    uint16_t firstWord;
    f.read((uint8_t*)&firstWord, sizeof(firstWord));
    
    if (firstWord == FILE_MAGIC) {
        f.read((uint8_t*)&loadedVer, sizeof(loadedVer));
        isV2 = (loadedVer >= 2);
    } else {
        f.seek(0); 
    }

    f.read((uint8_t*)&ledCount, sizeof(ledCount));
    f.read((uint8_t*)&startLedPos, sizeof(startLedPos));
    f.read((uint8_t*)&selectedCarColorIdx, sizeof(selectedCarColorIdx));
    f.read((uint8_t*)&selectedTrackColorIdx, sizeof(selectedTrackColorIdx));
    f.read((uint8_t*)&carLength, sizeof(carLength)); 
    
    if (f.available() >= (int)(sizeof(carBrightness) + sizeof(trackBrightness))) {
        f.read((uint8_t*)&carBrightness, sizeof(carBrightness));
        f.read((uint8_t*)&trackBrightness, sizeof(trackBrightness));
    } else {
        carBrightness = 150; trackBrightness = 10;
    }

    if (f.available() >= (int)sizeof(displayMode)) f.read((uint8_t*)&displayMode, sizeof(displayMode));
    else displayMode = 0;

    if (f.available() >= (int)sizeof(colorMaxSpeed)) f.read((uint8_t*)&colorMaxSpeed, sizeof(colorMaxSpeed));
    else colorMaxSpeed = 15.0f;

    if (f.available() >= (int)sizeof(showZoneEffects)) f.read((uint8_t*)&showZoneEffects, sizeof(showZoneEffects));
    else showZoneEffects = 2;
    
    if(selectedCarColorIdx >= NUM_COLORS) selectedCarColorIdx = 0;
    if(selectedTrackColorIdx >= NUM_COLORS) selectedTrackColorIdx = 7;
    if(carLength < 1 || carLength > 100) carLength = 5;
    if(carBrightness < 5 || carBrightness > 255) carBrightness = 150;
    if(trackBrightness < 0 || trackBrightness > 255) trackBrightness = 10;
    if(displayMode > 1) displayMode = 0;
    if(colorMaxSpeed < 1.0f || colorMaxSpeed > 60.0f) colorMaxSpeed = 15.0f;
    if(showZoneEffects > 2) showZoneEffects = 2;

    if (f.available() >= (int)sizeof(nodeCount)) {
        f.read((uint8_t*)&nodeCount, sizeof(nodeCount));
        if (nodeCount > 0 && nodeCount <= ledCount) {
            if (nodes != nullptr) delete[] nodes;
            nodes = new Node[ledCount]; 
            
            if (isV2) {
                f.read((uint8_t*)nodes, sizeof(Node) * nodeCount);
            } else {
                for (int i = 0; i < nodeCount; i++) {
                    f.read((uint8_t*)&nodes[i].pos,    sizeof(nodes[i].pos));
                    f.read((uint8_t*)&nodes[i].height, sizeof(nodes[i].height));
                    f.read((uint8_t*)&nodes[i].type,   sizeof(nodes[i].type));
                    f.read((uint8_t*)&nodes[i].art,    sizeof(nodes[i].art));
                    f.read((uint8_t*)&nodes[i].force,  sizeof(nodes[i].force));
                    f.read((uint8_t*)&nodes[i].length, sizeof(nodes[i].length));
                    nodes[i].enabled = 1; 
                }
            }
        }
    }

    if (f.available() >= (int)sizeof(segmentCount)) {
        f.read((uint8_t*)&segmentCount, sizeof(segmentCount));
        if (segmentCount > 0) {
            if (segments != nullptr) delete[] segments;
            segments = new SegmentData[segmentCount];
            if (loadedVer >= 3) {
                f.read((uint8_t*)segments, sizeof(SegmentData) * segmentCount);
            } else {
                struct OldSegmentData { float distance; float acc; };
                OldSegmentData* oldSegs = new OldSegmentData[segmentCount];
                f.read((uint8_t*)oldSegs, sizeof(OldSegmentData) * segmentCount);
                for(int i=0; i<segmentCount; i++) {
                    segments[i].distance = oldSegs[i].distance;
                    segments[i].acc = oldSegs[i].acc;
                    segments[i].angle = 0.0f;     
                    segments[i].curvature = 0.0f; 
                }
                delete[] oldSegs;
            }
        }
    }

    if (f.available() >= (int)sizeof(zoneBrightness)) f.read((uint8_t*)&zoneBrightness, sizeof(zoneBrightness));
    else zoneBrightness = 10;

    if (f.available() >= (int)sizeof(displayFps)) {
        f.read((uint8_t*)&displayFps, sizeof(displayFps));
        if (displayFps < 1 || displayFps > 20) displayFps = 4;
    } else displayFps = 4;

    // NEU: G-Kraft Skalierung aus dem Speicher laden
    if (f.available() >= (int)sizeof(gForceScale)) {
        f.read((uint8_t*)&gForceScale, sizeof(gForceScale));
        if (gForceScale < 0.01f || gForceScale > 1.0f) gForceScale = 0.20f;
    } else {
        gForceScale = 0.20f;
    }

    // NEU (V5): Sound-Einstellungen laden
    if (f.available() >= (int)(sizeof(soundVolume) + sizeof(soundEnabled))) {
        f.read((uint8_t*)&soundVolume, sizeof(soundVolume));
        f.read((uint8_t*)&soundEnabled, sizeof(soundEnabled));
        if (soundVolume > 30) soundVolume = 20;
        if (soundEnabled > 1) soundEnabled = 1;
    } else {
        soundVolume = 20;
        soundEnabled = 1;
    }

    // NEU (V6): Sound-Modus laden
    if (f.available() >= (int)sizeof(soundMode)) {
        f.read((uint8_t*)&soundMode, sizeof(soundMode));
        if (soundMode > 1) soundMode = 0;
    } else {
        soundMode = 0;
    }

    f.close();
    strip.updateLength(ledCount);

    if (loadedVer < 3 && nodeCount > 0) {
        calculations();
        bezierCalculations();
        segmentCalculations();
        savePlayDataSlot(slot); 
    }

    if (dfpReady) dfPlayer.volume(soundVolume);

    return true;
}

// ==========================================
// ABSPIELEN
// ==========================================
int findSegment(float position) {
    if (position >= ledCount) position -= ledCount;
    if (position < 0) position += ledCount;
    for (int i = 0; i < segmentCount; i++) {
        if (position < segments[i].distance) return i;
    }
    return segmentCount - 1; 
}

void moveLED() {
    static bool initialized = false;
    static unsigned long lastDisplayUpdate = 0; 

    if (!initialized) {
        pos = 0.0f; vel = 0.5f;  
        maxReachedSpeed = vel; 
        currentGForce = 1.0f;  
        maxGForce = 1.0f;
        minGForce = 1.0f;
        lastTime = millis(); currentSegment = 0;
        initialized = true;
        lastSoundStatus = "";              // NEU
        randomModeActive = false;          // NEU: sauberer Start
        // playSound(SND_START);              // Start-Jingle
        if (soundMode == 0) playSound(SND_START);   // Jingle nur im Effekte-Modus
    }
    
    unsigned long now = millis();
    float dt = (now - lastTime) / 1000.0f; 
    if (dt > 0.05f) dt = 0.05f;
    
    if (dt > 0.001f) { 
        lastTime = now;
        currentSegment = findSegment(pos);
        
        // --- 1. GRUNDPHYSIK ---
        float acc_gravity = segments[currentSegment].acc;
        float ratio = constrain(acc_gravity / 9.81f, -1.0f, 1.0f);
        float cosAlpha = sqrt(1.0f - pow(ratio, 2));
        float direction = (vel > 0) ? 1.0f : ((vel < 0) ? -1.0f : 0.0f);
        
        float acc_total = acc_gravity 
                          - direction * (friction * 9.81f * cosAlpha) 
                          - direction * (airResistance * vel * vel);

        String trackStatus = "FREIE FAHRT";

        // --- 2. DYNAMISCHER KETTENLIFT ÜBER DIE ZONENLÄNGE ---
        bool onLift = false;
        float activeLiftSpeed = 0.0f;
        
        for (int k = 0; k < carLength; k++) {
            float p_k = pos - k;
            while (p_k < 0.0f) p_k += ledCount;
            while (p_k >= ledCount) p_k -= ledCount;
            
            float pixelRunDist = p_k;
            
            for (int i = 0; i < nodeCount; i++) {
                if (nodes[i].art == 5 && nodes[i].enabled) { // Lift (nur wenn aktiv)
                    float nodeRunDist = (nodes[i].pos >= startLedPos) ? (nodes[i].pos - startLedPos) : ((ledCount - startLedPos) + nodes[i].pos);
                    float zoneDist = pixelRunDist - nodeRunDist;
                    if (zoneDist < 0.0f) zoneDist += ledCount; 
                    
                    if (zoneDist <= nodes[i].length) {
                        onLift = true;
                        activeLiftSpeed = nodes[i].force;
                        break;
                    }
                }
            }
            if (onLift) break;
        }

        if (onLift) {
            if (vel <= activeLiftSpeed) {
                vel = activeLiftSpeed;
                acc_total = 0.0f;
                trackStatus = "KETTENLIFT";
            } else {
                trackStatus = "AUSROLLEN"; 
            }
        }

        // --- 3. DYNAMISCHE ZONEN FÜR BREMSEN UND BOOSTER ---
        bool braking = false;
        float brakeTarget = 0.0f;

        if (trackStatus != "KETTENLIFT") {
            float noseRunDist = pos;

            for (int i = 0; i < nodeCount; i++) {
                if ((nodes[i].art == 6 || nodes[i].art == 7) && nodes[i].enabled) {
                    float nodeRunDist = (nodes[i].pos >= startLedPos) ? (nodes[i].pos - startLedPos) : ((ledCount - startLedPos) + nodes[i].pos);
                    float zoneDist = noseRunDist - nodeRunDist;
                    if (zoneDist < 0.0f) zoneDist += ledCount;
                    
                    if (zoneDist <= nodes[i].length) {
                        if (nodes[i].art == 6) { // BREMSE
                            float targetVel = nodes[i].force;
                            if (vel > targetVel) {
                                float remainingDist = nodes[i].length - zoneDist;
                                if (remainingDist > 0.01f) {
                                    float reqAcc = (targetVel * targetVel - vel * vel) / (2.0f * remainingDist);
                                    acc_total += reqAcc; 
                                    braking = true;
                                    brakeTarget = targetVel;
                                }
                                trackStatus = "BREMSEN";
                            } else {
                                trackStatus = "FREIE FAHRT"; 
                            }
                        } else if (nodes[i].art == 7) { // BOOSTER
                            acc_total += nodes[i].force; 
                            trackStatus = "BOOSTER";
                        }
                        break;
                    }
                }
            }
        }
        
        // NEU: Sound passend zum Streckenstatus
        updateSound(trackStatus);
        // NEU: Random-Wiedergabe - naechsten Track starten wenn der aktuelle fertig ist
        if (randomModeActive && dfPlayer.available()) {
            uint8_t type = dfPlayer.readType();
            if (type == DFPlayerPlayFinished) {
                playRandomSound();
            }
        }

        vel += acc_total * dt;

        if (braking && vel < brakeTarget) vel = brakeTarget;
        if (vel < 0.02f) vel = 0.02f; 

        if (vel > maxReachedSpeed) maxReachedSpeed = vel;

        // --- 3B. G-KRAFT BERECHNUNG INKLUSIVE SKALIERUNG ---
        float curvature = segments[currentSegment].curvature;
        float angle = segments[currentSegment].angle;
        
        // Die Fliehkraft (dynamischer Anteil) wird mit dem einstellbaren Faktor abgeschwächt.
        // Die Schwerkraft (cos(angle)) bleibt bei exakt 1G, damit die Physik stimmt (stehend = 1G).
        float dynamicG = ((vel * vel * curvature) / 9.81f) * gForceScale;
        float rawGForce = cos(angle) + dynamicG;
        
        // Sanfter Tiefpassfilter (Glättung) für saubere Display-Werte
        currentGForce = (currentGForce * 0.9f) + (rawGForce * 0.1f);
        
        if (currentGForce > maxGForce) maxGForce = currentGForce;
        if (currentGForce < minGForce) minGForce = currentGForce;

        pos += vel * dt;
        
        if (pos >= ledCount) pos -= ledCount;
        if (pos < 0) pos += ledCount;
        
        // --- 4. FARB-ZUWEISUNG DYNAMISCH ODER STATISCH ---
        if (selectedTrackColorIdx == 8) trackColor = getSpeedColor(vel, trackBrightness);
        if (selectedCarColorIdx == 8)   carColor   = getSpeedColor(vel, carBrightness);

        // A) Grundbahn ausleuchten
        for (int i = 0; i < ledCount; i++) strip.setPixelColor(i, trackColor);

        // B) Zonen-Effekte
        if (showZoneEffects > 0) {
            for (int i = 0; i < nodeCount; i++) {
                if (nodes[i].art >= 5 && nodes[i].length > 0) {
                    uint8_t zr=0, zg=0, zb=0;
                    if(nodes[i].art == 5)      { zr=0;   zg=100; zb=255; }
                    else if(nodes[i].art == 6) { zr=255; zg=50;  zb=0;   }
                    else if(nodes[i].art == 7) { zr=255; zg=255; zb=0;   }

                    // Deaktivierte Zonen: nur sehr schwaches Glimmen, kein Animations-Effekt
                    if (!nodes[i].enabled) {
                        uint16_t dimBright = zoneBrightness / 5;
                        for (uint16_t l = 0; l < nodes[i].length; l++) {
                            uint16_t zPos = (nodes[i].pos + l) % ledCount;
                            strip.setPixelColor(zPos, strip.Color((zr * dimBright)/255, (zg * dimBright)/255, (zb * dimBright)/255));
                        }
                        continue;
                    }

                    if (showZoneEffects == 1) { 
                        for (uint16_t l = 0; l < nodes[i].length; l++) {
                            uint16_t zPos = (nodes[i].pos + l) % ledCount;
                            strip.setPixelColor(zPos, strip.Color((zr * zoneBrightness)/255, (zg * zoneBrightness)/255, (zb * zoneBrightness)/255));
                        }
                    } else if (showZoneEffects == 2) { 
                        for (uint16_t l = 0; l < nodes[i].length; l++) {
                            uint16_t zPos = (nodes[i].pos + l) % ledCount;
                            float bFactor = 0.1f;

                            if (nodes[i].art == 5) {
                                uint16_t cTime = constrain(1200 / max(nodes[i].force, 0.1f), 100, 2000);
                                uint16_t phase = (now % cTime) * nodes[i].length / cTime;
                                if (l == phase) bFactor = 1.0f;
                                else if (abs((int)l - (int)phase) == 1) bFactor = 0.4f;
                            }
                            else if (nodes[i].art == 7) {
                                uint16_t cTime = constrain(4000 / max(nodes[i].force, 1.0f), 50, 400);
                                uint16_t phase = (now % cTime) * nodes[i].length / cTime;
                                if (l <= phase && l >= (phase > 2 ? phase - 2 : 0)) bFactor = 1.0f;
                            }
                            else if (nodes[i].art == 6) {
                                float pulse = (sin(now * 0.006f) + 1.0f) / 2.0f;
                                bFactor = 0.1f + pulse * 0.5f;
                            }

                            uint8_t fR = (uint8_t)(zr * bFactor * zoneBrightness / 255.0f);
                            uint8_t fG = (uint8_t)(zg * bFactor * zoneBrightness / 255.0f);
                            uint8_t fB = (uint8_t)(zb * bFactor * zoneBrightness / 255.0f);
                            strip.setPixelColor(zPos, strip.Color(fR, fG, fB));
                        }
                    }
                }
            }
        }

        // C) Wagen zeichnen
        int pInt = (int)pos;
        float frac = pos - pInt;
        
        uint8_t r = (uint8_t)(carColor >> 16);
        uint8_t g = (uint8_t)(carColor >> 8);
        uint8_t b = (uint8_t)carColor;

        int nextHead = (pInt + 1 + startLedPos) % ledCount;
        strip.setPixelColor(nextHead, strip.Color(r * frac, g * frac, b * frac));

        for (int i = 0; i < carLength - 1; i++) {
            int bodyPos = (pInt - i + startLedPos + 2 * ledCount) % ledCount;
            strip.setPixelColor(bodyPos, carColor);
        }

        int tailPos = (pInt - (carLength - 1) + startLedPos + 2 * ledCount) % ledCount;
        strip.setPixelColor(tailPos, strip.Color(r * (1.0f - frac), g * (1.0f - frac), b * (1.0f - frac)));

        strip.setBrightness(playBrightness);
        strip.show();
        
        // --- 5. DISPLAY-TELEMETRIE ---
        int currentLedPos = ((int)pos + startLedPos) % ledCount;
        unsigned long refreshInterval = 1000 / displayFps;
        
        if (now - lastDisplayUpdate >= refreshInterval) {
            drawTelemetry(currentLedPos, vel, acc_total, trackStatus);
            lastDisplayUpdate = now;
        }
    }
    
    if (isButtonPressed()) {
        initialized = false;
        stopSound();                       // NEU: Sound aus
        strip.clear(); strip.show();
        uiState = ST_MENU; encoder.setPosition(0); lastPos = -1;
    }
}