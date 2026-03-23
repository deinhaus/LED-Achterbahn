#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <RotaryEncoder.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>

// Datei für Daten in LittleFS
#define LEDCFG_FILE "/ledcfg.bin"

// LED-STREIFEN
#define LED_PIN 33

// DREHENCODER-EINSTELLUNGEN
#define PIN_CLK 25
#define PIN_DT 26
#define PIN_SW 27
RotaryEncoder encoder(PIN_CLK, PIN_DT, RotaryEncoder::LatchMode::FOUR3);

// DISPLAY-EINSTELLUNGEN
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// PUNKT-STRUKTUR
struct point2D {
  float x;
  float y;
};

// STATE MACHINE
enum UI_STATE {
    ST_MENU,
    ST_PLAY,

    ST_SETUP_WARNING,
    ST_SETUP_LENGTH,

    ST_NODE_START,
    ST_NODE_POS,
    ST_NODE_HEIGHT,
    ST_NODE_TYPE,

    ST_NODE_END
};

UI_STATE uiState = ST_MENU;

// UI-VARIABLEN
int lastPos = -1;
int menuSelection = 0;

// BUTTON ENTPRELLEN

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
            return true;   // nur einmal pro Druck
        }
        stableState = reading;
    }

    lastReading = reading;
    return false;
}

// GLOBALE VARIABLEN
Adafruit_NeoPixel strip(0, LED_PIN, NEO_GRB + NEO_KHZ800);  // NeoPixel-Objekt (Länge wird in setup() gesetzt)
uint16_t ledCount;          // aktuelle Länge des Streifens
uint16_t currentLED = 0;
int lengthSpeed = 0;
unsigned long lastLengthMove = 0;
unsigned long lastLengthDecel = 0;
const int lengthMaxSpeed = 10;

// NODES
struct Node {
  uint16_t pos;
  uint16_t height;
  uint8_t type;
};

Node* nodes = nullptr;
uint16_t nodeCount = 0;
Node currentNode = {}; //Erzeugt die Variable und setzt alle Werte der ersten Node auf 0


/////////////////////////////////////////////////////////
// V A R I A B L E N   F Ü R   B E R E C H N U N G E N //
/////////////////////////////////////////////////////////

// POINTS

// Array mit Pointer für 2D-Punkte, um die Punkte für die Bezierkurven zu berechnen
point2D* points = nullptr;
uint16_t pointCount = 0;
const int steps = 200;       // Feinheit der Längenberechnung
int targetLength = 0;   //Distanz von p1 zu p2 in LEDs
const float tolerance = 0.001; // Genauigkeit

int startHeight; //Höhe des Startpunkts
int endHeight;  //Höhe des Endpunkts

// Punkte für Bezierberechnung
point2D p1;
point2D c1;
point2D c2;
point2D p2;

// BEZIER

// Array mit Pointer für die Bezier-Kurvenpunkte
point2D* bezierPoints = nullptr;
uint16_t bezierPointCount = 0;
const int intSteps = 20; // Interpolationsschritte der finalen Bezierkurve

// SEGMENTS AND ACCELERATION
struct SegmentData{
  float distance;
  float acc;
};

SegmentData* segments = nullptr;
uint16_t segmentCount = 0;

// PLAY
float pos = 0;
float vel = 0;
uint16_t startLedPos = 0;
// Timing
unsigned long lastTime = 0;
int currentSegment = 0;

// Helligkeit (steuerbar per Encoder im Play-Modus)
uint8_t playBrightness = 80;  // Startwert (0-255)




/////////////////////////
// V O I D   S E T U P //
/////////////////////////

void setup() {
    Serial.begin(115200);

    pinMode(PIN_SW, INPUT_PULLUP);
    pinMode(PIN_CLK, INPUT_PULLUP);
    pinMode(PIN_DT, INPUT_PULLUP);

    // LittleFS initialisieren (true = formatiert beim ersten Mal)
    if (!LittleFS.begin(true)) {
        Serial.println(F("LittleFS Fehler!"));
    }

    ledCount = 1000;

    // LED-Streifen-Länge setzen und initialisieren
    strip.updateLength(ledCount);
    strip.begin();
    strip.clear();
    strip.show();

    if(!display.begin(0x3C)) {
        Serial.println(F("SH1106 Fehler"));
        for(;;);
    }

    // Gespeicherte Play-Daten laden (falls vorhanden)
    if (loadPlayData()) {
        Serial.println("Play-Daten aus Flash geladen.");
    } else {
        Serial.println("Keine gespeicherten Daten gefunden.");
    }

    display.clearDisplay();
    Serial.println("System bereit. Modus: MENU");
}


///////////////////////
// V O I D   L O O P //
///////////////////////

void loop() {

  //ENCODER AUSLESEN
  encoder.tick();
  int newPos = encoder.getPosition();

  ///////////////////////////////
  // S T A T E   M A C H I N E //
  ///////////////////////////////

  switch (uiState) {
    
    /////////////////////////////
    // 0 - H A U P T M E N U E //
    /////////////////////////////
    
    case ST_MENU:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 2) {encoder.setPosition(1); newPos = 1; }

        menuSelection = newPos;
        drawMainMenu();
        lastPos = newPos;
      }

      if (isButtonPressed()) {
        if (menuSelection == 0) {
          if (segments != nullptr) {
            uiState = ST_PLAY;
          } else {
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SH110X_WHITE);
            display.setCursor(0, 0);
            display.println("Keine Daten! Bitte zuerst Setup durchfuehren.");
            display.display();
            delay(2000);
            drawMainMenu();
          }
        }
        else if (menuSelection == 1) uiState = ST_SETUP_WARNING;

        menuSelection = 0;
        lastPos = -1;
        encoder.setPosition(0);
      }
            
      break;
      

    ///////////////////////////
    // 1 - A B S P I E L E N //
    ///////////////////////////

    case ST_PLAY:

      // Maker Faire Werbung
      drawRunning();
      // Encoder steuert Helligkeit im Play-Modus
      if (newPos != lastPos) {
        int delta = (newPos - lastPos) * 10;  // 10er-Schritte pro Tick
        playBrightness = constrain((int)playBrightness + delta, 5, 255);
        lastPos = newPos;
      }
      moveLED();
      break;



    ///////////////////
    // 2 - S E T U P //
    ///////////////////

    ///////////////////////////
    // 2 . 1 - W A R N U N G //
    ///////////////////////////

    case ST_SETUP_WARNING:
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }
        if (newPos >= 2) {encoder.setPosition(1); newPos = 1; }
                    
        menuSelection = newPos;
        drawSetupWarning();
        lastPos = newPos;
      }
                    
      if (isButtonPressed()) {
        if (menuSelection == 0) {
          uiState = ST_SETUP_LENGTH;

          // Löscht Inhalte des Node-Arrays und setzt Strip auf maximale Länge
          ledCount = 1000;
          strip.updateLength(ledCount);
          nodes = new Node[ledCount];     //Node-Array mit der maximalen Größe ledCount
          nodeCount = 0;                  //Zähler für die erstellten Nodes
          currentNode = {};
          currentLED = 0;
          Serial.print("Node Setup resetted.");
        } else if (menuSelection == 1) {
          uiState = ST_MENU;
          Serial.print("Returned to menu. Node Setup intact.");
          Serial.print(nodeCount);
        }
        
        menuSelection = 0;
        lastPos = -1;
        encoder.setPosition(0);
        newPos = 0;
        currentLED = newPos;
      }

      break;

    /////////////////////////
    // 2 . 2 - L A E N G E //
    /////////////////////////

    case (ST_SETUP_LENGTH):
      // Encoder steuert die Geschwindigkeit, nicht direkt die Position
      if (newPos != lastPos) {
        int delta = newPos - lastPos;
        lengthSpeed += delta;
        lengthSpeed = constrain(lengthSpeed, -lengthMaxSpeed, lengthMaxSpeed);
        lastLengthDecel = millis();

        // Bei Speed ±1: Encoder bewegt Cursor direkt (1 Tick = 1 LED)
        if (abs(lengthSpeed) == 1) {
          if (delta > 0) currentLED++;
          else if (delta < 0 && currentLED > 0) currentLED--;
          drawSetupLength();
        }

        lastPos = newPos;
      }

      // Auto-Abbremsen wenn nicht gedreht wird
      if (lengthSpeed != 0 && millis() - lastLengthDecel >= 400) {
        lastLengthDecel = millis();
        if (lengthSpeed > 0) lengthSpeed--;
        else lengthSpeed++;
      }

      // Ab Speed >=2: Cursor bewegt sich zeitgesteuert
      if (abs(lengthSpeed) >= 2) {
        unsigned long interval = 200 / abs(lengthSpeed);  // schneller bei höherem Speed
        if (interval < 20) interval = 20;                 // Minimum 20ms
        if (millis() - lastLengthMove >= interval) {
          lastLengthMove = millis();
          if (lengthSpeed > 0) {
            currentLED++;
          } else {
            if (currentLED > 0) currentLED--;
          }
          drawSetupLength();
        }
      }
 
      if (isButtonPressed()) {
 
        ledCount = currentLED + 1;  //Speichert die Länge des Streifens
        display.clearDisplay();
        display.display();
 
        for(int i = currentLED; i >= 0; i--) {
          currentLED = i;
          Serial.print(currentLED);
          strip.clear();
          strip.setPixelColor(0, strip.Color(0, 0, 100));            //Erste LED
          strip.setPixelColor(currentLED, strip.Color(0, 100, 0));  //Cursor-LED
          strip.setPixelColor(ledCount-1, strip.Color(0, 0, 100));
          strip.show();
          delay(10);
        }
 
        lengthSpeed = 0;
        lastPos = -1;
        encoder.setPosition(0);
        newPos = 0;
        currentLED = newPos;
        //uiState = ST_NODE_POS;
        uiState = ST_NODE_START;
 
      }
 
    break;
    

    /////////////////////////////////////////
    // 2 . 3 - S C H E I T E L P U N K T E //
    /////////////////////////////////////////

    ///////////////////////////
    // 2 . 3 . 0 - S T A R T //
    ///////////////////////////

    case (ST_NODE_START):
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(ledCount-1); newPos = ledCount-1; }
        if (newPos > ledCount-1) { encoder.setPosition(0); newPos = 0; }

      currentNode.pos = newPos;
      Serial.print("Position: ");
      Serial.println(currentNode.pos);
      currentLED = newPos;
      drawNodeStart();
      lastPos = newPos;

      }
      if (isButtonPressed()) {

        display.clearDisplay();
        display.display();

        encoder.setPosition(0);
        lastPos = -1;
        uiState = ST_NODE_HEIGHT;

      }
      break;



    /////////////////////////////////
    // 2 . 3 . 1 - P O S I T I O N //
    /////////////////////////////////

    case (ST_NODE_POS):
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(ledCount-1); newPos = ledCount-1; }
        if (newPos > ledCount-1) { encoder.setPosition(0); newPos = 0; }

      currentNode.pos = newPos;
      Serial.print("Position: ");
      Serial.println(currentNode.pos);
      currentLED = newPos;
      drawNodePos();
      lastPos = newPos;

      }
      if (isButtonPressed()) {

        display.clearDisplay();
        display.display();

        encoder.setPosition(0);
        lastPos = -1;
        uiState = ST_NODE_HEIGHT;

      }
      break;
                    
    ///////////////////////////
    // 2 . 3 . 2 - H O E H E //
    ///////////////////////////

    case (ST_NODE_HEIGHT):
      if (newPos != lastPos) {
        if (newPos < 0) { encoder.setPosition(0); newPos = 0; }

      currentNode.height = newPos;
      Serial.print("Height: ");
      Serial.println(currentNode.height);
      drawNodeHeight();
      lastPos = newPos;

      }

      if (isButtonPressed()) {

        display.clearDisplay();
        display.display();

        encoder.setPosition(currentNode.pos);
        lastPos = -1;
        currentLED = currentNode.pos;

        if (nodeCount == 0) {
          currentNode.type = 0; //Startpunkt
          nodes[nodeCount++] = currentNode;
          currentNode = {}; //Reset
          uiState = ST_NODE_POS;
        } else {
          uiState = ST_NODE_TYPE;
          menuSelection = 1;
          encoder.setPosition(menuSelection);
        }

      }
      break;

    ///////////////////////
    // 2 . 3 . 3 - T Y P //
    ///////////////////////
                    
    case (ST_NODE_TYPE):
      if (newPos != lastPos) {
        if (newPos < 1) {encoder.setPosition(1); newPos = 1; }
        if (newPos > 2) {encoder.setPosition(2); newPos = 2; }
                    
        menuSelection = newPos;
        currentNode.type = menuSelection;
        currentLED = currentNode.pos;

        Serial.print("Type: ");
        Serial.println(currentNode.type);

        drawNodeType();
        lastPos = newPos;
      }

      if (isButtonPressed()) {

        display.clearDisplay();
        display.display();

        encoder.setPosition(currentNode.pos);
        lastPos = -1;
        currentLED = currentNode.pos;

        if(currentNode.type == 2) {
          uiState = ST_NODE_END;
        } else {
          uiState = ST_NODE_POS;
        }

        if (nodeCount < ledCount) {
          nodes[nodeCount++] = currentNode;
          currentNode = {};
          Serial.print("Node gespeichert. Gesamt: "); Serial.println(nodeCount);
        } else {
          Serial.println("Max Nodes erreicht!");
        }

      }
      break;


    case (ST_NODE_END):

      display.clearDisplay();
      display.display();

      strip.clear();
      strip.show();

      for (int i = 0; i < nodeCount; i++) {
        Serial.print("Node ");
        Serial.print(i);
        Serial.print(" -> ");
        Serial.print("pos: ");
        Serial.print(nodes[i].pos);
        Serial.print(", height: ");
        Serial.print(nodes[i].height);
        Serial.print(", type: ");
        Serial.println(nodes[i].type);
      }

      encoder.setPosition(0);
      lastPos = -1;

      startLedPos = nodes[0].pos;
      strip.updateLength(ledCount);


      // LETZTE BERECHNUNGEN
      calculations();
      printPoints();

      bezierCalculations();
      printBezierPoints();

      segmentCalculations();
      printSegments();

      // Play-Daten im Flash speichern und neu starten
      savePlayData();
      Serial.println("Neustart...");
      delay(500);
      ESP.restart();

  } //SWITCH END

} //LOOP END


/////////////////////////////////
// M E N U E - G R A F I K E N //
/////////////////////////////////

void drawMainMenu() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);

    display.setCursor(0, 10);
    display.println(menuSelection == 0 ? "> ABSPIELEN" : "  ABSPIELEN");

    display.setCursor(0, 30);
    display.println(menuSelection == 1 ? "> KONFIGURIEREN" : "  KONFIGURIEREN");

    display.display();
}

void drawSetupWarning() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("Alle Einstellungen werden entfernt. Weiter?");

  display.setCursor(0, 40);
  display.println(menuSelection == 0 ? "> JA" : "  JA");

  display.setCursor(32, 40);
  display.println(menuSelection == 1 ? "> NEIN" : "  NEIN");

  display.display();
}

void drawSetupLength() {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("LED-Cursor mit dem");
  display.println("Drehencoder zu der");
  display.println("letzten LED bewegen");
  display.println("und druecken.");

  //Zeigt die Länge des Streifens auf dem Display an.
  //display.setTextSize(3);
  //display.setCursor(64, 32);
  //display.println(currentLED+1);

  display.display();

  //Zeige Einstellungen mit LEDs an
  drawSetupLeds();

}

void drawNodeStart() {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("LED zum hoechsten");
  display.println("Punkt der Strecke");
  display.println("bewegen und den");
  display.println("Encoder druecken.");

  //Gibt die aktuelle LED auf dem Display aus.
  //display.setTextSize(3);
  //display.setCursor(64, 32);
  //display.println(currentLED+1);

  display.display();

  //Zeige Einstellungen mit LEDs an
  drawSetupLeds();

}

void drawNodePos() {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("LED zum folgenden");
  display.println("Scheitelpunkt bewegen");
  display.println("und Encoder druecken.");

  //Gibt die aktuelle LED auf dem Display aus.
  //display.setTextSize(3);
  //display.setCursor(64, 32);
  //display.println(currentLED+1);

  display.display();

  //Zeige Einstellungen mit LEDs an
  drawSetupLeds();

}

void drawNodeHeight() {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("Hoehe einstellen und den Encoder druecken:");

  display.setTextSize(3);
  display.setCursor(64, 32);
  display.println(currentNode.height);

  display.display();

  //Zeige Einstellungen mit LEDs an
  drawSetupLeds();

}

void drawNodeType() {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("Art des Punktes");
  display.println("bestimmen:");

  display.setCursor(0, 20);
  display.println(menuSelection == 1 ? "> Scheitelpunkt" : "  Scheitelpunkt");

  display.setCursor(0, 30);
  display.println(menuSelection == 2 ? "> Endpunkt" : "  Endpunkt");

  display.display();

  //Zeige Einstellungen mit LEDs an
  drawSetupLeds();

}

void drawRunning() {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  display.setCursor(0, 0);
  display.println("Wer hat noch nicht,");
  display.println("wer will noch mal?");
  display.println("Anschnallen bitte!");
  display.println("Die naechste Runde");
  display.println("geht los-os-os-s.");

  display.display();

}


void drawSetupLeds() {

  strip.clear();

  //Start und Ende
  strip.setPixelColor(0, strip.Color(0, 0, 100));
  strip.setPixelColor(ledCount-1, strip.Color(0, 0, 100));

  //Bereits gespeicherte Nodes farblich nach Typ
  for (uint16_t i = 0; i < nodeCount; i++){
    uint8_t r = 0, g = 0, b = 0;

    switch(nodes[i].type) {
      case 0: //Startpunkt
        r = 80; g = 80; b = 80; //weiß
        break;
      case 1: //Scheitelpunkt
        r = 100; g = 0; b = 100; //pink
        break;
      case 2: //Tunnel
        r= 100; g = 65; b = 0; //orange
        break;
      case 3: //Endpunkt
        r = 100; g = 0; b = 0;
        break;
    }
    strip.setPixelColor(nodes[i].pos, strip.Color(r, g, b));
  }

  //Bewegliche LED
  strip.setPixelColor(currentLED, strip.Color(0, 100, 0));
  strip.show();

}


///////////////////////////////////////////
///////////////////////////////////////////
///// F U N K T I O N E N   F U E R   /////
///// D I E   B E R E C H N U N G E N /////
///////////////////////////////////////////
///////////////////////////////////////////

// Die lerp-Funktion, was Bezier 1. Grades ist
point2D lerp(point2D a, point2D b, float t) {
  return {
    a.x + t * (b.x - a.x),
    a.y + t * (b.y - a.y)
  };
}

// Die Bezierfunktion 3. Grades
point2D bezier(point2D p1, point2D c1, point2D c2, point2D p2, float t) {
  point2D a = lerp(p1, c1, t);
  point2D b = lerp(c1, c2, t);
  point2D c = lerp(c2, p2, t);
  point2D d = lerp(a, b, t);
  point2D e = lerp(b, c, t);
  return lerp(d, e, t);
}


// Umrechnung der Nodes auf dem LED-Streifen in 2D-Punkte
void calculations() {

  points = new point2D[ledCount];     //Node-Array mit der maximalen Größe ledCount

  points[0] = {0, nodes[0].height};
  pointCount = 1;                  //Zähler für die erstellte erste Node

  for(int i = 0; i < nodeCount - 1; i++) {

    int posCurrent = nodes[i].pos;
    int posNext = nodes[i+1].pos;

    if (posNext >= posCurrent) {
      targetLength = posNext - posCurrent;
    } else {
      targetLength = (ledCount - posCurrent) + posNext;
    }

    startHeight = nodes[i].height;
    endHeight = nodes[i+1].height;

    Serial.print("Segment ");
    Serial.print(i);
    Serial.print(": pos ");
    Serial.print(posCurrent);
    Serial.print(" -> ");
    Serial.print(posNext);
    Serial.print(" = ");
    Serial.print(targetLength);
    Serial.println(" LEDs");

    float a = findA();
    points[i+1] = {points[i].x+a, nodes[i+1].height};
    pointCount++;

    Serial.print("Gefundenes a = ");
    Serial.println(a, 6);
  }

  // Schließt den Kreis
  int posLast = nodes[nodeCount-1].pos;
  int posFirst = nodes[0].pos;

  if (posFirst >= posLast) {
    targetLength = posFirst - posLast;
  } else {
    targetLength = (ledCount - posLast) + posFirst;
  }

  startHeight  = nodes[nodeCount - 1].height;
  endHeight    = nodes[0].height;

  Serial.print("Closing segment: pos ");
  Serial.print(posLast);
  Serial.print(" -> ");
  Serial.print(posFirst);
  Serial.print(" = ");
  Serial.print(targetLength);
  Serial.println(" LEDs");

  float a = findA();

  points[pointCount] = {points[pointCount - 1].x+a, nodes[0].height};

  pointCount++; //da nodes[0] mit reinzählt, ist pointCount 1 größer als nodeCount

  Serial.print("Gefundenes a (closing) = ");
  Serial.println(a, 6);

  Serial.print("pointCount: ");
  Serial.println(pointCount);

}

// Funktion, um die Kurvenlänge zu berechnen
float bezierLength(float aValue){
  p1 = {0, startHeight};
  c1 = {aValue / 2.0f, startHeight};
  c2 = {aValue / 2.0f, endHeight};
  p2 = {aValue,        endHeight};

  float length = 0;
  point2D prev = bezier(p1, c1, c2, p2, 0);

  for (int i = 0; i < steps; i++){
    float t = (float)i / (steps - 1);
    point2D curr = bezier(p1, c1, c2, p2, t);

    // Abstand addieren
    float dx = curr.x - prev.x;
    float dy = curr.y - prev.y;
    length += sqrt(dx*dx + dy*dy);

    prev = curr;
  }

  return length;
}

// Bestimmung von A bzw. x-Position von p2 mithilfe einer Annäherung (Binary Search)
float findA(){
  float low = 0;
  float high = targetLength;      // darf groß sein, solange > erwartetes a

  while ((high - low) > tolerance){
    float mid = (low + high) / 2.0f;
    float len = bezierLength(mid);

    if (len > targetLength){
      high = mid;
      //Serial.print("high: ");
      //Serial.println(high);
    } else {
      low = mid;
      //Serial.print("low: ");
      //Serial.println(low);
    }
  }
  return (low + high) / 2.0f;
}

// X- und Y-Werte der 2D-Punkte ausgeben
void printPoints() {
  for (int i = 0; i < pointCount; i++) {
    Serial.print(i);
    Serial.print("  x= ");
    Serial.print(points[i].x);
    Serial.print("  y= ");
    Serial.println(points[i].y);
  }
}

// Bezier-Kurven mit jeweils 20 Punkten erzeugen.
void bezierCalculations() {

  bezierPointCount = 0;
  bezierPoints = new point2D[(pointCount - 1) * intSteps];

  for (int i = 0; i < pointCount - 1; i++) {

    // Start- und Endpunkt
    p1 = points[i];
    p2 = points[i + 1];

    // Kontrollpunkte (wie bei deiner Längenberechnung)
    float midX = (p1.x + p2.x) / 2.0f;
    c1 = { midX, p1.y };
    c2 = { midX, p2.y };

    // Interpolation
    for (int j = 0; j < intSteps; j++) {

      float t = (float)j / (intSteps - 1);
      point2D bp = bezier(p1, c1, c2, p2, t);

      if (i > 0 && j == 0) continue;

      bezierPoints[bezierPointCount++] = bp;
    }
  }


}

// X- und Y-Werte der interpolierten Bezier-Punkte ausgeben
void printBezierPoints() {
  for (int i = 0; i < bezierPointCount; i++) {
    Serial.print(i);
    Serial.print("  x=");
    Serial.print(bezierPoints[i].x, 4);
    Serial.print("  y=");
    Serial.println(bezierPoints[i].y, 4);
  }
}

// Segmente und Beschleunigung für letztes Array berechnen

void segmentCalculations() {

  if (bezierPointCount < 2) return;

  segmentCount = bezierPointCount; //Ginge vlt. auch ausschl. mit bezierPointCount
  segments = new SegmentData[segmentCount];

  for (int i = 0; i < segmentCount - 1; i++) {

    float dx = bezierPoints[i + 1].x - bezierPoints[i].x;
    float dy = bezierPoints[i + 1].y - bezierPoints[i].y;
    float distance = sqrt(dx * dx + dy * dy);

    float acc = 0.0f;
    if (distance > 0.0001f) {
      acc = (-dy / distance) * 9.81f;
    }

    if (i == 0) {
      Serial.println("i == 0");
      segments[i].distance = distance;
    } else {
      segments[i].distance = segments[i-1].distance + distance;
    }

    segments[i].acc = acc;
  }

  // Letztes Segment (57)
  int last = segmentCount - 1;
  float lastDistance = ledCount - segments[last - 1].distance;
  float dy = bezierPoints[0].y - bezierPoints[last].y;

  float acc = 0.0f;
  if (lastDistance > 0.0001f) {
    acc = (-dy / lastDistance) * 9.81f;
  }

  segments[last].distance = ledCount;
  segments[last].acc = acc;
}

void printSegments() {

  for (int i = 0; i < segmentCount; i++) {
    Serial.print(i);
    Serial.print("  dist=");
    Serial.print(segments[i].distance, 4);
    Serial.print("  acc=");
    Serial.println(segments[i].acc, 4);
  }
}


///////////////////////////////////////////
// S A V E  /  L O A D   ( L i t t l e F S )
///////////////////////////////////////////

void savePlayData() {
    File f = LittleFS.open(LEDCFG_FILE, "w");
    if (!f) {
        Serial.println("Fehler: Datei konnte nicht geöffnet werden!");
        return;
    }

    f.write((uint8_t*)&ledCount, sizeof(ledCount));
    f.write((uint8_t*)&startLedPos, sizeof(startLedPos));
    f.write((uint8_t*)segments, sizeof(SegmentData) * segmentCount);
    f.close();

    Serial.print("Daten gespeichert: ");
    Serial.print(segmentCount);
    Serial.print(" Segmente, ");
    Serial.print(4 + sizeof(SegmentData) * segmentCount);
    Serial.println(" Bytes");
}

bool loadPlayData() {
    if (!LittleFS.exists(LEDCFG_FILE)) return false;

    File f = LittleFS.open(LEDCFG_FILE, "r");
    if (!f) return false;

    size_t fileSize = f.size();
    if (fileSize < 4) { f.close(); return false; }

    f.read((uint8_t*)&ledCount, sizeof(ledCount));
    f.read((uint8_t*)&startLedPos, sizeof(startLedPos));

    segmentCount = (fileSize - 4) / sizeof(SegmentData);
    if (segmentCount == 0) { f.close(); return false; }

    if (segments != nullptr) delete[] segments;
    segments = new SegmentData[segmentCount];
    f.read((uint8_t*)segments, sizeof(SegmentData) * segmentCount);
    f.close();

    // LED-Streifen an geladene Länge anpassen
    strip.updateLength(ledCount);

    Serial.print("Geladen: ");
    Serial.print(segmentCount);
    Serial.print(" Segmente, ledCount=");
    Serial.print(ledCount);
    Serial.print(", startLedPos=");
    Serial.println(startLedPos);

    return true;
}


/////////////////////////////////////////
/////////////////////////////////////////
///// F U N K T I O N E N   Z U M   /////
///// A B S P I E L E N ( P L A Y ) /////
/////////////////////////////////////////
/////////////////////////////////////////

// Funktion zum Finden des aktuellen Segments
int findSegment(float position) {
    // Position normalisieren (falls sie über ledCount hinausgeht)
    if (position >= ledCount) position -= ledCount;
    if (position < 0) position += ledCount;
    
    for (int i = 0; i < segmentCount; i++) {
        if (position < segments[i].distance) {
            return i;
        }
    }
    return segmentCount - 1; // Fallback
}

void moveLED() {
  static bool initialized = false;
    if (!initialized) {
        pos = 0.0f;
        vel = 0.5f;  // Startgeschwindigkeit (anpassen nach Bedarf)
        lastTime = millis();
        currentSegment = 0;

        initialized = true;
        Serial.println("Play gestartet!");
    }
    
    // Zeitberechnung
    unsigned long now = millis();
    float dt = (now - lastTime) / 1000.0f; // in Sekunden
    
    // Delta-Zeit begrenzen (verhindert Sprünge bei langen Pausen)
    if (dt > 0.05f) dt = 0.05f;
    
    if (dt > 0.001f) { // Nur updaten wenn genug Zeit vergangen
        lastTime = now;
        
        // Aktuelles Segment finden
        currentSegment = findSegment(pos);
        
        // Beschleunigung anwenden
        float acc = segments[currentSegment].acc;
        vel += acc * dt;

        // Mindestgeschwindigkeit in VORWÄRTS-Richtung
        const float minVel = 0.1f;
        if (vel < minVel) vel = minVel;  // Verhindert Rückwärtslaufen!
        
        // Position aktualisieren
        pos += vel * dt;
        
        // Position wrappen (Kreisbahn)
        // Beim Übergang: Geschwindigkeit zurücksetzen, um
        // Energieaufbau durch Rundungsfehler zu verhindern
        if (pos >= ledCount) {
            pos -= ledCount;
            vel = 0.5f;
        }
        if (pos < 0) pos += ledCount;
        
        // LED anzeigen: Bahn blau ausleuchten
        for (int i = 0; i < ledCount; i++) {
          strip.setPixelColor(i, strip.Color(0, 0, 13));  // 5% blau
        }

        // Hauptposition (mit Startpunkt-Offset)
        int ledPos = ((int)pos + startLedPos) % ledCount;

        // Zwei-LED-Crossfade (weiße LED überschreibt Bahn)
        float frac = pos - (int)pos;
        int nextLed = (ledPos + 1) % ledCount;

        float wCurr = (1.0f - frac) * (1.0f - frac);
        float wNext = frac * frac;
        float wSum = wCurr + wNext;
        uint8_t bCurr = (uint8_t)(255 * wCurr / wSum);
        uint8_t bNext = (uint8_t)(255 * wNext / wSum);

        strip.setPixelColor(ledPos, strip.Color(bCurr, bCurr, bCurr));
        strip.setPixelColor(nextLed, strip.Color(bNext, bNext, bNext));

        strip.setBrightness(playBrightness);
        strip.show();
        
        // Debug-Ausgabe (alle 500ms)
        static unsigned long lastDebug = 0;
        if (now - lastDebug > 500) {
            Serial.print("pos="); Serial.print(pos, 2);
            Serial.print(" vel="); Serial.print(vel, 2);
            Serial.print(" seg="); Serial.print(currentSegment);
            Serial.print(" acc="); Serial.println(acc, 2);
            lastDebug = now;
        }
    }
    
    // Zurück zum Menü bei Tastendruck
    if (isButtonPressed()) {
        initialized = false;
        strip.clear();
        strip.show();
        uiState = ST_MENU;
        encoder.setPosition(0);
        lastPos = -1;
    }
}
