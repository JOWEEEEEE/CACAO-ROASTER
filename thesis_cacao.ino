#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include "max6675.h"
#include <PID_v1.h>
#include <RBDdimmer.h>

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
// MAX6675 THERMOCOUPLE - SOFTSPI
// DO  = Pin 4 (MISO)
// CLK = Pin 5 (SCK)
// CS  = Pin 6 (SS)
// ==================================================
#define PIN_THERMO_CLK  5
#define PIN_THERMO_CS   6
#define PIN_THERMO_DO   4

MAX6675 thermocouple(PIN_THERMO_CLK, PIN_THERMO_CS, PIN_THERMO_DO);

// ==================================================
// AC DIMMER (RBDDimmer PWM-3)
// ==================================================
#define PIN_ZERO_CROSS  2    // INT0 for zero-cross detection
#define PIN_DIMMER      3    // PWM output for heating element

dimmerLamp dimmer(PIN_DIMMER);

// ==================================================
// RELAYS
// ==================================================
#define PIN_LPG_RELAY   52   // LPG preheat relay
#define PIN_FAN_RELAY   51   // Fan cooling relay

// ==================================================
// BUTTON
// Arduino Pin 40 ---- Button ---- GND
// ==================================================
#define PIN_BUTTON      40

// ==================================================
// PID VARIABLES
// ==================================================
double currentTemp = 0.0;      // Current temperature from MAX6675
double setPoint = 90.0;        // Target temperature: 90°C
double pidOutput = 0.0;        // PID output (0-100%)

// PID tuning constants (adjust based on system response)
double kp = 2.5;               // Proportional gain
double ki = 0.15;              // Integral gain
double kd = 1.0;               // Derivative gain

PID pidController(&currentTemp, &pidOutput, &setPoint, kp, ki, kd, DIRECT);

// ==================================================
// COUNTDOWN SETTINGS
// ==================================================
const uint32_t INIT_TIME_SEC        = 5UL;          // 5 seconds initialization
const uint32_t PRE_HEATING_TIME_SEC = 5UL * 60UL;   // 5 minutes (300 seconds)
const uint32_t ROASTING_TIME_SEC    = 25UL * 60UL;  // 25 minutes (1500 seconds)
const uint32_t TEMPERING_TIME_SEC   = 5UL * 60UL;   // 5 minutes (300 seconds)
const uint32_t STAGE_INTRO_TIME_SEC = 5UL;          // 5 seconds for stage intro

// For testing only (uncomment to use):
// const uint32_t INIT_TIME_SEC        = 3;
// const uint32_t PRE_HEATING_TIME_SEC = 10;
// const uint32_t ROASTING_TIME_SEC    = 15;
// const uint32_t TEMPERING_TIME_SEC   = 10;
// const uint32_t STAGE_INTRO_TIME_SEC = 3;

// ==================================================
// PHASES
// ==================================================
enum Phase {
  IDLE,
  INIT_SCREEN,           // 5 second initialization display
  PRE_HEATING_INTRO,     // Waiting for button press to start
  PRE_HEATING,           // Countdown: LPG ON, Fan OFF
  ROASTING_INTRO,        // 5 second roasting intro
  ROASTING,              // Countdown: Dimmer PID ON, Fan ON (active cooling)
  TEMPERING_INTRO,       // 5 second tempering intro
  TEMPERING,             // Countdown: Fan OFF (passive cooling)
  DONE                   // Process complete
};

Phase currentPhase = IDLE;
Phase previousPhase = IDLE;  // Track previous phase for LCD updates

// ==================================================
// VARIABLES
// ==================================================
uint32_t phaseStartUnix = 0;
unsigned long lastLCDUpdate = 0;
unsigned long lastTempRead = 0;
unsigned long lastPIDCalculation = 0;

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
// FAN CONTROL
// ==================================================
void fanOn() {
  digitalWrite(PIN_FAN_RELAY, HIGH);
}

void fanOff() {
  digitalWrite(PIN_FAN_RELAY, LOW);
}

// ==================================================
// AC DIMMER CONTROL
// ==================================================
void setDimmerPower(double power) {
  // Clamp power between 0 and 100
  if (power < 0) power = 0;
  if (power > 100) power = 100;
  
  dimmer.setPower((int)power);
}

// ==================================================
// READ TEMPERATURE FROM MAX6675
// ==================================================
void readTemperature() {
  unsigned long nowMillis = millis();
  
  // Read temperature every 100ms to avoid excessive SPI reads
  if (nowMillis - lastTempRead >= 100) {
    lastTempRead = nowMillis;
    
    // Read from MAX6675 (SoftSPI)
    currentTemp = thermocouple.readCelsius();
    
    // Check for sensor error (MAX6675 returns -0.25 on error)
    if (currentTemp < 0) {
      currentTemp = 0;  // Reset on error
    }
  }
}

// ==================================================
// PID TEMPERATURE CONTROL
// ==================================================
void updatePIDControl() {
  unsigned long nowMillis = millis();
  
  // Calculate PID every 100ms
  if (nowMillis - lastPIDCalculation >= 100) {
    lastPIDCalculation = nowMillis;
    
    pidController.Compute();
    
    // Set dimmer power based on PID output
    setDimmerPower(pidOutput);
  }
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
  if (currentPhase == INIT_SCREEN) {
    return INIT_TIME_SEC;
  }

  if (currentPhase == PRE_HEATING) {
    return PRE_HEATING_TIME_SEC;
  }

  if (currentPhase == ROASTING) {
    return ROASTING_TIME_SEC;
  }

  if (currentPhase == ROASTING_INTRO || currentPhase == TEMPERING_INTRO) {
    return STAGE_INTRO_TIME_SEC;
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
void startInitScreen() {
  currentPhase = INIT_SCREEN;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();
  fanOff();
  setDimmerPower(0);

  lcd.clear();
}

void startPreHeatingIntro() {
  currentPhase = PRE_HEATING_INTRO;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();
  fanOff();
  setDimmerPower(0);

  lcd.clear();
}

void startPreHeating() {
  currentPhase = PRE_HEATING;
  phaseStartUnix = rtc.now().unixtime();

  lpgOn();   // LPG relay ON during pre-heating
  fanOff();  // No fan during preheat
  setDimmerPower(0);

  lcd.clear();
}

void startRoastingIntro() {
  currentPhase = ROASTING_INTRO;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();
  fanOn();   // Fan starts for active cooling intro
  setDimmerPower(0);

  lcd.clear();
}

void startRoasting() {
  currentPhase = ROASTING;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();
  fanOn();   // Fan ON for active cooling during roasting
  
  // Initialize PID controller
  pidController.SetMode(AUTOMATIC);
  pidController.SetOutputLimits(0, 100);  // Output: 0-100%
  pidOutput = 0;

  lcd.clear();
}

void startTemperingIntro() {
  currentPhase = TEMPERING_INTRO;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();
  fanOff();  // Fan OFF for passive cooling intro
  setDimmerPower(0);

  lcd.clear();
}

void startTempering() {
  currentPhase = TEMPERING;
  phaseStartUnix = rtc.now().unixtime();

  lpgOff();
  fanOff();  // Fan OFF - passive cooling only
  setDimmerPower(0);

  lcd.clear();
}

void finishProcess() {
  currentPhase = DONE;

  lpgOff();
  fanOff();
  setDimmerPower(0);

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

  if (currentPhase == INIT_SCREEN) {
    printRow(0, "CACAO ROASTER");
    printRow(1, "Initializing...");
    return;
  }

  if (currentPhase == PRE_HEATING_INTRO) {
    printRow(0, "PRE-HEATING");
    printRow(1, "Press BTN Start");
    return;
  }

  if (currentPhase == PRE_HEATING) {
    String line1 = "Time: " + formatTime(getRemainingTime());
    String line2 = "T:" + String((int)currentTemp) + "C";
    
    printRow(0, line1);
    printRow(1, line2);
    return;
  }

  if (currentPhase == ROASTING_INTRO) {
    printRow(0, "ROASTING");
    printRow(1, "STAGE INTRO...");
    return;
  }

  if (currentPhase == ROASTING) {
    String line1 = "Time: " + formatTime(getRemainingTime()) + " T:" + String((int)currentTemp) + "C";
    String line2 = "SP:90C PWM:" + String((int)pidOutput) + "%";
    
    printRow(0, line1);
    printRow(1, line2);
    return;
  }

  if (currentPhase == TEMPERING_INTRO) {
    printRow(0, "TEMPERING");
    printRow(1, "STAGE INTRO...");
    return;
  }

  if (currentPhase == TEMPERING) {
    String line1 = "Time: " + formatTime(getRemainingTime()) + " T:" + String((int)currentTemp) + "C";
    String line2 = "Cooling...FAN OFF";
    
    printRow(0, line1);
    printRow(1, line2);
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
  pinMode(PIN_FAN_RELAY, OUTPUT);

  lpgOff();
  fanOff();

  // Initialize dimmer
  dimmer.begin(NORMAL_MODE, ON);
  setDimmerPower(0);

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

  // Start with IDLE state
  currentPhase = IDLE;
  previousPhase = IDLE;
  
  // Force first LCD update
  updateLCD();
  lastLCDUpdate = millis();
}

// ==================================================
// LOOP
// ==================================================
void loop() {
  unsigned long nowMillis = millis();

  // Read temperature continuously
  readTemperature();

  // Button function
  if (buttonPressed()) {
    if (currentPhase == IDLE) {
      startInitScreen();
    }
    else if (currentPhase == INIT_SCREEN) {
      startPreHeatingIntro();
    }
    else if (currentPhase == PRE_HEATING_INTRO) {
      startPreHeating();
    }
    else if (currentPhase == DONE) {
      currentPhase = IDLE;
      lpgOff();
      fanOff();
      setDimmerPower(0);
      lcd.clear();
    }
  }

  // ==================================================
  // INITIALIZATION SCREEN
  // ==================================================
  if (currentPhase == INIT_SCREEN) {
    if (getElapsedTime() >= INIT_TIME_SEC) {
      startPreHeatingIntro();
    }
  }

  // ==================================================
  // PRE-HEATING INTRO
  // Waiting for button press
  // ==================================================
  else if (currentPhase == PRE_HEATING_INTRO) {
    // Wait for button press (handled above)
  }

  // ==================================================
  // PRE-HEATING
  // LPG relay is ON, countdown running
  // ==================================================
  else if (currentPhase == PRE_HEATING) {
    lpgOn();
    fanOff();

    if (getElapsedTime() >= PRE_HEATING_TIME_SEC) {
      startRoastingIntro();
    }
  }

  // ==================================================
  // ROASTING INTRO
  // 5 second intro screen
  // ==================================================
  else if (currentPhase == ROASTING_INTRO) {
    fanOn();  // Fan starts during intro

    if (getElapsedTime() >= STAGE_INTRO_TIME_SEC) {
      startRoasting();
    }
  }

  // ==================================================
  // ROASTING
  // Dimmer with PID control + Active fan cooling
  // ==================================================
  else if (currentPhase == ROASTING) {
    fanOn();  // Keep fan ON for active cooling

    // Update PID controller for temperature control
    updatePIDControl();

    if (getElapsedTime() >= ROASTING_TIME_SEC) {
      startTemperingIntro();
    }
  }

  // ==================================================
  // TEMPERING INTRO
  // 5 second intro screen
  // ==================================================
  else if (currentPhase == TEMPERING_INTRO) {
    fanOff();  // Fan OFF for passive cooling

    if (getElapsedTime() >= STAGE_INTRO_TIME_SEC) {
      startTempering();
    }
  }

  // ==================================================
  // TEMPERING
  // Passive cooling, fan OFF
  // ==================================================
  else if (currentPhase == TEMPERING) {
    lpgOff();
    fanOff();
    setDimmerPower(0);

    if (getElapsedTime() >= TEMPERING_TIME_SEC) {
      finishProcess();
    }
  }

  else if (currentPhase == IDLE) {
    lpgOff();
    fanOff();
    setDimmerPower(0);
  }

  else if (currentPhase == DONE) {
    lpgOff();
    fanOff();
    setDimmerPower(0);
  }

  // ==================================================
  // UPDATE LCD DISPLAY
  // Update every 500ms OR when phase changes
  // ==================================================
  if (previousPhase != currentPhase || nowMillis - lastLCDUpdate >= 500) {
    lastLCDUpdate = nowMillis;
    previousPhase = currentPhase;
    updateLCD();
  }
}
