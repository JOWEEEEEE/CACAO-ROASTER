#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ==================================================
// LCD I2C 16x2
// Arduino Mega:
// SDA = pin 20
// SCL = pin 21
// ==================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==================================================
// RTC DS3231
// ==================================================
RTC_DS3231 rtc;

// ==================================================
// BUTTON
// Arduino Pin 40 ---- Button ---- GND
// ==================================================
#define PIN_BUTTON 40

// ==================================================
// LPG RELAY
// HIGH = ON
// LOW  = OFF
// ==================================================
#define PIN_LPG_RELAY 52

// ==================================================
// COUNTDOWN SETTINGS
// ==================================================
const uint32_t PRE_HEATING_TIME_SEC = 5UL * 60UL;    // 5 minutes
const uint32_t ROASTING_TIME_SEC    = 25UL * 60UL;   // 25 minutes
const uint32_t TEMPERING_TIME_SEC   = 5UL * 60UL;    // 5 minutes

// For testing only:
// const uint32_t PRE_HEATING_TIME_SEC = 10;
// const uint32_t ROASTING_TIME_SEC    = 20;
// const uint32_t TEMPERING_TIME_SEC   = 10;

// ==================================================
// PHASES
// ==================================================
enum Phase {
  IDLE,
  PRE_HEATING,
  ROASTING,
  TEMPERING,
  DONE
};

Phase currentPhase = IDLE;

// ==================================================
// VARIABLES
// ==================================================
uint32_t phaseStartUnix = 0;
unsigned long lastLCDUpdate = 0;

// ==================================================
// LCD PRINT
// ==================================================
void printRow(byte row, String text) {
  while (text.length() < 16) {
    text += " ";
  }

  if (text.length() > 16) {
    text = text.substring(0, 16);
  }

  lcd.setCursor(0, row);
  lcd.print(text);
}

// ==================================================
// LPG CONTROL
// ==================================================
void lpgOn() {
  digitalWrite(PIN_LPG_RELAY, HIGH);
}

void lpgOff() {
  digitalWrite(PIN_LPG_RELAY, LOW);
}

// ==================================================
// BUTTON DEBOUNCE
// INPUT_PULLUP:
// Not pressed = HIGH
// Pressed     = LOW
// ==================================================
bool buttonPressed() {
  static bool lastReading = HIGH;
  static bool stableState = HIGH;
  static unsigned long lastDebounceTime = 0;

  bool reading = digitalRead(PIN_BUTTON);

  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > 50) {
    if (reading != stableState) {
      stableState = reading;

      if (stableState == LOW) {
        lastReading = reading;
        return true;
      }
    }
  }

  lastReading = reading;
  return false;
}

// ==================================================
// PHASE DURATION
// ==================================================
uint32_t getPhaseDuration() {
  if (currentPhase == PRE_HEATING) {
    return PRE_HEATING_TIME_SEC;
  }

  if (currentPhase == ROASTING) {
    return ROASTING_TIME_SEC;
  }

  if (currentPhase == TEMPERING) {
    return TEMPERING_TIME_SEC;
  }

  return 0;
}

// ==================================================
// RTC ELAPSED TIME
// ==================================================
uint32_t getElapsedTime() {
  if (currentPhase == IDLE || currentPhase == DONE) {
    return 0;
  }

  uint32_t nowUnix = rtc.now().unixtime();

  if (nowUnix >= phaseStartUnix) {
    return nowUnix - phaseStartUnix;
  }

  return 0;
}

// ==================================================
// RTC REMAINING TIME
// ==================================================
uint32_t getRemainingTime() {
  uint32_t duration = getPhaseDuration();
  uint32_t elapsed = getElapsedTime();

  if (elapsed >= duration) {
    return 0;
  }

  return duration - elapsed;
}

// ==================================================
// FORMAT TIME MM:SS
// ==================================================
String formatTime(uint32_t secondsLeft) {
  uint32_t minutes = secondsLeft / 60;
  uint32_t seconds = secondsLeft % 60;

  char buffer[6];
  sprintf(buffer, "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);

  return String(buffer);
}

// ==================================================
// START PHASES
// ==================================================
void startPreHeating() {
  currentPhase = PRE_HEATING;
  phaseStartUnix = rtc.now().unixtime();

  lpgOn();   // LPG relay HIGH during pre-heating countdown

  lcd.clear();
}

void startRoasting() {
  currentPhase = ROASTING;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();  // LPG OFF after pre-heating

  lcd.clear();
}

void startTempering() {
  currentPhase = TEMPERING;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();

  lcd.clear();
}

void finishProcess() {
  currentPhase = DONE;

  lpgOff();

  lcd.clear();
}

// ==================================================
// LCD DISPLAY
// ==================================================
void updateLCD() {
  if (currentPhase == IDLE) {
    printRow(0, "CACAO ROASTER");
    printRow(1, "Press BTN Start");
    return;
  }

  if (currentPhase == PRE_HEATING) {
    printRow(0, "PREHEATING");
    printRow(1, "Time: " + formatTime(getRemainingTime()));
    return;
  }

  if (currentPhase == ROASTING) {
    printRow(0, "ROASTING");
    printRow(1, "Time: " + formatTime(getRemainingTime()));
    return;
  }

  if (currentPhase == TEMPERING) {
    printRow(0, "TEMPERING");
    printRow(1, "Time: " + formatTime(getRemainingTime()));
    return;
  }

  if (currentPhase == DONE) {
    printRow(0, "PROCESS DONE");
    printRow(1, "Press BTN Reset");
    return;
  }
}

// ==================================================
// SETUP
// ==================================================
void setup() {
  Serial.begin(9600);

  Wire.begin();

  lcd.init();
  lcd.backlight();
  lcd.clear();

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  pinMode(PIN_LPG_RELAY, OUTPUT);
  lpgOff();

  printRow(0, "CACAO ROASTER");
  printRow(1, "Initializing");
  delay(1500);

  if (!rtc.begin()) {
    lcd.clear();
    printRow(0, "RTC NOT FOUND");
    printRow(1, "Check wiring");
    while (1);
  }

  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  lcd.clear();
  printRow(0, "CACAO ROASTER");
  printRow(1, "Press BTN Start");
}

// ==================================================
// LOOP
// ==================================================
void loop() {
  unsigned long nowMillis = millis();

  // Button function
  if (buttonPressed()) {
    if (currentPhase == IDLE) {
      startPreHeating();
    }
    else if (currentPhase == DONE) {
      currentPhase = IDLE;
      lpgOff();
      lcd.clear();
    }
  }

  // ==================================================
  // PRE-HEATING
  // LPG relay is HIGH while countdown is running
  // ==================================================
  if (currentPhase == PRE_HEATING) {
    lpgOn();

    if (getElapsedTime() >= PRE_HEATING_TIME_SEC) {
      startRoasting();
    }
  }

  // ==================================================
  // ROASTING
  // LPG relay is OFF
  // ==================================================
  else if (currentPhase == ROASTING) {
    lpgOff();

    if (getElapsedTime() >= ROASTING_TIME_SEC) {
      startTempering();
    }
  }

  // ==================================================
  // TEMPERING
  // LPG relay is OFF
  // ==================================================
  else if (currentPhase == TEMPERING) {
    lpgOff();

    if (getElapsedTime() >= TEMPERING_TIME_SEC) {
      finishProcess();
    }
  }

  else if (currentPhase == IDLE) {
    lpgOff();
  }

  else if (currentPhase == DONE) {
    lpgOff();
  }

  // Update LCD every 500ms
  if (nowMillis - lastLCDUpdate >= 500) {
    lastLCDUpdate = nowMillis;
    updateLCD();
  }
}