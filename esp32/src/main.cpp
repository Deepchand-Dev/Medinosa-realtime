/*

  HARDWARE:
  - ESP32 DevKit
  - PCA9685 Servo Driver
  - 4 Servos
  - DS3231 RTC
  - 16x2 I2C LCD
  - 4 Push Buttons
  - Buzzer

  I2C:
  SDA = GPIO 21
  SCL = GPIO 22

  BUTTONS:
  CH1 = GPIO 32
  CH2 = GPIO 33
  CH3 = GPIO 26
  CH4 = GPIO 27

  BUZZER:
  GPIO 25

  PCA9685:
  CH1 Servo = channel 0
  CH2 Servo = channel 1
  CH3 Servo = channel 2
  CH4 Servo = channel 3

  DS3231 = MASTER CLOCK

  DASHBOARD API:
  /status
  /dispense?chamber=1
  /api/status
  /api/set?chamber=1&time=08:00
  /api/rtc
  /api/rtc-set?time=14:30:00
  /api/rtc-sync
*/


// ============================================================
// LIBRARIES
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include <RTClib.h>
#include <Adafruit_PWMServoDriver.h>
#include <LiquidCrystal_I2C.h>


// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID = "Deepchand";
const char* WIFI_PASSWORD = "12345678";

const char* DEVICE_ID = "ESP32-001";


// ============================================================
// I2C
// ============================================================

#define I2C_SDA 21
#define I2C_SCL 22

#define PCA9685_ADDRESS 0x40
#define LCD_ADDRESS 0x27


// ============================================================
// DEVICES
// ============================================================

WebServer server(80);

RTC_DS3231 rtc;

Adafruit_PWMServoDriver pwm =
  Adafruit_PWMServoDriver(PCA9685_ADDRESS);

LiquidCrystal_I2C lcd(
  LCD_ADDRESS,
  16,
  2
);

Preferences preferences;


// ============================================================
// BUZZER
// ============================================================

#define BUZZER_PIN 25


// ============================================================
// BUTTONS
// ============================================================

#define SERVO_COUNT 4

const uint8_t buttonPin[SERVO_COUNT] = {
  32,
  33,
  26,
  27
};


// ============================================================
// PCA9685 SERVO CHANNELS
// ============================================================

const uint8_t servoChannel[SERVO_COUNT] = {
  0,
  1,
  2,
  3
};


// ============================================================
// SERVO SETTINGS
// ============================================================

#define SERVO_MIN 110
#define SERVO_MAX 500

#define SERVO_CLOSED_ANGLE 0
#define SERVO_OPEN_ANGLE 90


// ============================================================
// MEDICINE QUANTITY
// ============================================================

int medicineQty[SERVO_COUNT] = {
  10,
  10,
  10,
  10
};


// ============================================================
// TIMING
// ============================================================

const unsigned long DEBOUNCE_MS = 80UL;

const unsigned long CLOSE_DELAY_MS =
  30000UL;

const unsigned long MISSED_DOSE_MS =
  3600000UL;


// ============================================================
// DOSE STATUS
// ============================================================

enum DoseStatus {
  STATUS_IDLE,
  STATUS_WAITING,
  STATUS_TAKEN,
  STATUS_MISSED,
  STATUS_MANUAL
};


String statusText(DoseStatus status) {

  switch (status) {

    case STATUS_IDLE:
      return "IDLE";

    case STATUS_WAITING:
      return "WAITING";

    case STATUS_TAKEN:
      return "TAKEN";

    case STATUS_MISSED:
      return "MISSED";

    case STATUS_MANUAL:
      return "MANUAL";
  }

  return "IDLE";
}


// ============================================================
// CHAMBER STATE
// ============================================================

struct ChamberState {

  bool active;

  bool automatic;

  bool waitingForConfirmation;

  bool waitingToClose;

  unsigned long doseStartedAt;

  unsigned long confirmationAt;

  DoseStatus status;

  bool scheduleHandledToday;
};


ChamberState chamber[SERVO_COUNT];


// ============================================================
// DEFAULT SCHEDULE
// ============================================================

uint16_t defaultSchedule[SERVO_COUNT] = {
  8 * 60,
  12 * 60,
  16 * 60,
  20 * 60
};


uint16_t scheduleMinutes[SERVO_COUNT];


// ============================================================
// DATE TRACKING
// ============================================================

int currentDay = -1;
int currentMonth = -1;
int currentYear = -1;

int lastScheduleMinute = -1;

unsigned long lcdMessageUntil = 0;


// ============================================================
// BUTTON STATE
// ============================================================

bool lastButtonReading[SERVO_COUNT] = {
  HIGH,
  HIGH,
  HIGH,
  HIGH
};

bool stableButtonState[SERVO_COUNT] = {
  HIGH,
  HIGH,
  HIGH,
  HIGH
};

unsigned long lastDebounceTime[SERVO_COUNT] = {
  0,
  0,
  0,
  0
};


// ============================================================
// WIFI RECONNECT TIMER
// ============================================================

unsigned long lastWiFiCheck = 0;


// ============================================================
// UTILITY
// ============================================================

String twoDigit(int value) {

  if (value < 10) {
    return "0" + String(value);
  }

  return String(value);
}


// ============================================================
// FORMAT SCHEDULE
// ============================================================

String formatMinutes(uint16_t totalMinutes) {

  int hour = totalMinutes / 60;

  int minute = totalMinutes % 60;

  return twoDigit(hour) + ":" +
         twoDigit(minute);
}


// ============================================================
// TODAY KEY
// ============================================================

uint32_t todayKey() {

  DateTime now = rtc.now();

  return
    (uint32_t)now.year() * 10000UL +
    (uint32_t)now.month() * 100UL +
    (uint32_t)now.day();
}


// ============================================================
// SERVO ANGLE -> PCA9685 PULSE
// ============================================================

uint16_t servoPulseFromAngle(int angle) {

  angle = constrain(
    angle,
    0,
    180
  );

  return map(
    angle,
    0,
    180,
    SERVO_MIN,
    SERVO_MAX
  );
}


// ============================================================
// MOVE SERVO
// ============================================================

void moveServo(
  int index,
  int angle
) {

  if (
    index < 0 ||
    index >= SERVO_COUNT
  ) {
    return;
  }

  pwm.setPWM(
    servoChannel[index],
    0,
    servoPulseFromAngle(angle)
  );
}


// ============================================================
// BUZZER
// ============================================================

void buzzerOn() {

  digitalWrite(
    BUZZER_PIN,
    HIGH
  );
}


void buzzerOff() {

  digitalWrite(
    BUZZER_PIN,
    LOW
  );
}


void shortBeep() {

  buzzerOn();

  delay(100);

  buzzerOff();
}


void doseReminder() {

  for (int i = 0; i < 2; i++) {

    buzzerOn();

    delay(180);

    buzzerOff();

    delay(120);
  }
}


void missedBeep() {

  for (int i = 0; i < 3; i++) {

    buzzerOn();

    delay(120);

    buzzerOff();

    delay(120);
  }
}


// ============================================================
// LCD
// ============================================================

void showLCD(
  String line1,
  String line2,
  unsigned long duration = 2500
) {

  line1 = line1.substring(0, 16);

  line2 = line2.substring(0, 16);

  lcd.clear();

  lcd.setCursor(0, 0);

  lcd.print(line1);

  lcd.setCursor(0, 1);

  lcd.print(line2);

  lcdMessageUntil =
    millis() + duration;
}


// ============================================================
// SAVE SCHEDULES
// ============================================================

void saveSchedules() {

  preferences.begin(
    "medicine",
    false
  );

  for (int i = 0; i < SERVO_COUNT; i++) {

    String key =
      "sch" + String(i);

    preferences.putUShort(
      key.c_str(),
      scheduleMinutes[i]
    );
  }

  preferences.end();

  Serial.println(
    "Schedules saved."
  );
}


// ============================================================
// LOAD SCHEDULES
// ============================================================

void loadSchedules() {

  preferences.begin(
    "medicine",
    false
  );

  for (int i = 0; i < SERVO_COUNT; i++) {

    String key =
      "sch" + String(i);

    if (
      !preferences.isKey(
        key.c_str()
      )
    ) {

      scheduleMinutes[i] =
        defaultSchedule[i];

      preferences.putUShort(
        key.c_str(),
        scheduleMinutes[i]
      );

    } else {

      scheduleMinutes[i] =
        preferences.getUShort(
          key.c_str(),
          defaultSchedule[i]
        );
    }

    Serial.print("CH");

    Serial.print(i + 1);

    Serial.print(" Schedule: ");

    Serial.println(
      formatMinutes(
        scheduleMinutes[i]
      )
    );
  }

  preferences.end();
}


// ============================================================
// SAVE DAILY STATUS
// ============================================================

void saveDailyStatus(
  int index
) {

  if (
    index < 0 ||
    index >= SERVO_COUNT
  ) {
    return;
  }

  preferences.begin(
    "daily",
    false
  );

  String h =
    "h" + String(index);

  String s =
    "s" + String(index);

  preferences.putBool(
    h.c_str(),
    chamber[index].scheduleHandledToday
  );

  preferences.putUChar(
    s.c_str(),
    (uint8_t)chamber[index].status
  );

  preferences.putUInt(
    "date",
    todayKey()
  );

  preferences.end();
}


// ============================================================
// LOAD DAILY STATUS
// ============================================================

void loadDailyStatus() {

  uint32_t today =
    todayKey();

  preferences.begin(
    "daily",
    true
  );

  uint32_t storedDate =
    preferences.getUInt(
      "date",
      0
    );

  for (int i = 0; i < SERVO_COUNT; i++) {

    String h =
      "h" + String(i);

    String s =
      "s" + String(i);

    if (storedDate == today) {

      chamber[i].scheduleHandledToday =
        preferences.getBool(
          h.c_str(),
          false
        );

      chamber[i].status =
        (DoseStatus)
        preferences.getUChar(
          s.c_str(),
          STATUS_IDLE
        );

    } else {

      chamber[i].scheduleHandledToday =
        false;

      chamber[i].status =
        STATUS_IDLE;
    }
  }

  preferences.end();
}


// ============================================================
// RTC INITIALIZATION
// ============================================================

bool initializeRTC() {

  if (!rtc.begin()) {

    Serial.println(
      "ERROR: DS3231 NOT FOUND!"
    );

    return false;
  }

  DateTime now =
    rtc.now();

  Serial.printf(
    "RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
    now.year(),
    now.month(),
    now.day(),
    now.hour(),
    now.minute(),
    now.second()
  );

  if (rtc.lostPower()) {

    Serial.println(
      "RTC LOST POWER!"
    );

    Serial.println(
      "NTP recovery will be attempted."
    );
  }

  return true;
}


// ============================================================
// NTP SYNC
// ============================================================

bool syncRTCFromNTP() {

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    Serial.println(
      "NTP failed: WiFi not connected."
    );

    return false;
  }

  Serial.println(
    "Starting NTP sync..."
  );

  configTime(
    19800,
    0,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  struct tm timeinfo;

  unsigned long start =
    millis();

  while (
    !getLocalTime(
      &timeinfo,
      1000
    )
  ) {

    if (
      millis() - start >
      15000UL
    ) {

      Serial.println(
        "NTP SYNC FAILED!"
      );

      return false;
    }
  }

  DateTime ntpTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  );

  rtc.adjust(ntpTime);

  currentYear =
    ntpTime.year();

  currentMonth =
    ntpTime.month();

  currentDay =
    ntpTime.day();

  lastScheduleMinute = -1;

  Serial.printf(
    "NTP -> RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
    ntpTime.year(),
    ntpTime.month(),
    ntpTime.day(),
    ntpTime.hour(),
    ntpTime.minute(),
    ntpTime.second()
  );

  return true;
}


// ============================================================
// INITIALIZE CHAMBERS
// ============================================================

void initializeChambers() {

  for (int i = 0; i < SERVO_COUNT; i++) {

    chamber[i].active =
      false;

    chamber[i].automatic =
      false;

    chamber[i].waitingForConfirmation =
      false;

    chamber[i].waitingToClose =
      false;

    chamber[i].doseStartedAt =
      0;

    chamber[i].confirmationAt =
      0;

    chamber[i].status =
      STATUS_IDLE;

    chamber[i].scheduleHandledToday =
      false;
  }
}


// ============================================================
// INITIALIZE HARDWARE
// ============================================================

void initializeHardware() {

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  buzzerOff();

  // Buttons
  for (int i = 0; i < SERVO_COUNT; i++) {

    pinMode(
      buttonPin[i],
      INPUT_PULLUP
    );
  }

  // LCD
  lcd.init();

  lcd.backlight();

  showLCD(
    "MEDINOSA",
    "Starting..."
  );

  // PCA9685
  pwm.begin();

  pwm.setOscillatorFrequency(
    27000000
  );

  pwm.setPWMFreq(50);

  delay(10);

  // Close all servos
  for (int i = 0; i < SERVO_COUNT; i++) {

    moveServo(
      i,
      SERVO_CLOSED_ANGLE
    );
  }

  // Button initial state
  for (int i = 0; i < SERVO_COUNT; i++) {

    lastButtonReading[i] =
      digitalRead(
        buttonPin[i]
      );

    stableButtonState[i] =
      lastButtonReading[i];
  }
}


// ============================================================
// WIFI CONNECT
// ============================================================

void connectWiFi() {

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  Serial.println();

  Serial.print(
    "Connecting to WiFi: "
  );

  Serial.println(
    WIFI_SSID
  );

  unsigned long start =
    millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 20000UL
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "================================"
    );

    Serial.println(
      "WIFI CONNECTED"
    );

    Serial.print(
      "ESP32 IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "RSSI: "
    );

    Serial.println(
      WiFi.RSSI()
    );

    Serial.println(
      "================================"
    );

    showLCD(
      "WiFi Connected",
      WiFi.localIP().toString()
    );

    // If RTC battery was lost,
    // recover using NTP.
    if (rtc.lostPower()) {

      if (syncRTCFromNTP()) {

        Serial.println(
          "RTC recovered from NTP."
        );
      }
    }

  } else {

    Serial.println(
      "WiFi connection FAILED!"
    );

    showLCD(
      "WiFi Failed",
      "RTC Mode"
    );
  }
}


// ============================================================
// CORS
// ============================================================

void enableCORS() {

  server.sendHeader(
    "Access-Control-Allow-Origin",
    "*"
  );

  server.sendHeader(
    "Access-Control-Allow-Methods",
    "GET,POST,OPTIONS"
  );

  server.sendHeader(
    "Access-Control-Allow-Headers",
    "*"
  );

  server.sendHeader(
    "Cache-Control",
    "no-store, no-cache, must-revalidate, max-age=0"
  );

  server.sendHeader(
    "Pragma",
    "no-cache"
  );
}


// ============================================================
// OPEN CHAMBER
// ============================================================

bool openChamber(
  int index,
  bool automatic
) {

  if (
    index < 0 ||
    index >= SERVO_COUNT
  ) {
    return false;
  }

  if (
    chamber[index].active
  ) {
    return false;
  }

  // Do not open empty chamber.
  if (
    medicineQty[index] <= 0
  ) {

    Serial.print(
      "CH"
    );

    Serial.print(
      index + 1
    );

    Serial.println(
      " EMPTY!"
    );

    showLCD(
      "CH" + String(index + 1),
      "EMPTY"
    );

    return false;
  }

  chamber[index].active =
    true;

  chamber[index].automatic =
    automatic;

  chamber[index].waitingForConfirmation =
    automatic;

  chamber[index].waitingToClose =
    false;

  chamber[index].doseStartedAt =
    millis();

  chamber[index].confirmationAt =
    0;

  chamber[index].status =
    automatic
      ? STATUS_WAITING
      : STATUS_MANUAL;

  moveServo(
    index,
    SERVO_OPEN_ANGLE
  );

  if (automatic) {

    doseReminder();

  } else {

    shortBeep();
  }

  Serial.print(
    "CH"
  );

  Serial.print(
    index + 1
  );

  Serial.println(
    automatic
      ? " AUTOMATIC OPEN"
      : " MANUAL OPEN"
  );

  showLCD(
    "CH" + String(index + 1),
    automatic
      ? "Take Medicine"
      : "OPEN"
  );

  return true;
}


// ============================================================
// CLOSE CHAMBER
// ============================================================

void closeChamber(
  int index
) {

  if (
    index < 0 ||
    index >= SERVO_COUNT
  ) {
    return;
  }

  moveServo(
    index,
    SERVO_CLOSED_ANGLE
  );

  chamber[index].active =
    false;

  chamber[index].waitingForConfirmation =
    false;

  chamber[index].waitingToClose =
    false;

  chamber[index].doseStartedAt =
    0;

  chamber[index].confirmationAt =
    0;

  Serial.print(
    "CH"
  );

  Serial.print(
    index + 1
  );

  Serial.println(
    " CLOSED"
  );

  showLCD(
    "CH" + String(index + 1),
    "CLOSED"
  );
}


// ============================================================
// MISSED DOSE
// ============================================================

void closeChamberMissed(
  int index
) {

  if (
    index < 0 ||
    index >= SERVO_COUNT
  ) {
    return;
  }

  moveServo(
    index,
    SERVO_CLOSED_ANGLE
  );

  chamber[index].active =
    false;

  chamber[index].automatic =
    false;

  chamber[index].waitingForConfirmation =
    false;

  chamber[index].waitingToClose =
    false;

  chamber[index].doseStartedAt =
    0;

  chamber[index].confirmationAt =
    0;

  chamber[index].status =
    STATUS_MISSED;

  chamber[index].scheduleHandledToday =
    true;

  saveDailyStatus(index);

  Serial.print(
    "MISSED DOSE - CH"
  );

  Serial.println(
    index + 1
  );

  missedBeep();

  showLCD(
    "CH" + String(index + 1),
    "MISSED DOSE",
    5000
  );
}


// ============================================================
// CONFIRM DOSE
// ============================================================

void confirmDose(
  int index
) {

  if (
    index < 0 ||
    index >= SERVO_COUNT
  ) {
    return;
  }

  if (
    !chamber[index].active
  ) {
    return;
  }

  if (
    !chamber[index].automatic
  ) {
    return;
  }

  if (
    !chamber[index].waitingForConfirmation
  ) {
    return;
  }

  chamber[index].waitingForConfirmation =
    false;

  chamber[index].waitingToClose =
    true;

  chamber[index].confirmationAt =
    millis();

  chamber[index].status =
    STATUS_TAKEN;

  // One medicine dose has been taken.
  if (
    medicineQty[index] > 0
  ) {

    medicineQty[index]--;
  }

  Serial.print(
    "DOSE CONFIRMED - CH"
  );

  Serial.print(
    index + 1
  );

  Serial.print(
    " | Remaining: "
  );

  Serial.println(
    medicineQty[index]
  );

  saveDailyStatus(index);

  showLCD(
    "CH" + String(index + 1),
    "DOSE CONFIRMED"
  );

  shortBeep();
}


// ============================================================
// PROCESS CHAMBER
// ============================================================

void processChamber(
  int index
) {

  if (
    !chamber[index].active
  ) {
    return;
  }

  // Waiting for patient confirmation
  if (
    chamber[index].automatic &&
    chamber[index].waitingForConfirmation
  ) {

    if (
      millis() -
      chamber[index].doseStartedAt >=
      MISSED_DOSE_MS
    ) {

      closeChamberMissed(index);

      return;
    }
  }

  // Confirmed -> close after 30 seconds
  if (
    chamber[index].waitingToClose
  ) {

    if (
      millis() -
      chamber[index].confirmationAt >=
      CLOSE_DELAY_MS
    ) {

      closeChamber(index);
    }
  }
}


// ============================================================
// PROCESS BUTTONS
// ============================================================

void processButtons() {

  for (int i = 0; i < SERVO_COUNT; i++) {

    bool reading =
      digitalRead(
        buttonPin[i]
      );

    // Reading changed
    if (
      reading !=
      lastButtonReading[i]
    ) {

      lastDebounceTime[i] =
        millis();

      lastButtonReading[i] =
        reading;
    }

    // Stable for debounce time
    if (
      millis() -
      lastDebounceTime[i] >=
      DEBOUNCE_MS
    ) {

      if (
        reading !=
        stableButtonState[i]
      ) {

        stableButtonState[i] =
          reading;

        // BUTTON PRESSED
        if (
          stableButtonState[i] ==
          LOW
        ) {

          Serial.print(
            "BUTTON CH"
          );

          Serial.print(
            i + 1
          );

          Serial.println(
            " PRESSED"
          );

          // ------------------------------------------
          // CLOSED -> MANUAL OPEN
          // ------------------------------------------

          if (
            !chamber[i].active
          ) {

            openChamber(
              i,
              false
            );
          }

          // ------------------------------------------
          // AUTOMATIC -> CONFIRM
          // ------------------------------------------

          else if (
            chamber[i].automatic &&
            chamber[i].waitingForConfirmation
          ) {

            confirmDose(i);
          }

          // ------------------------------------------
          // MANUAL -> CLOSE
          // ------------------------------------------

          else if (
            !chamber[i].automatic
          ) {

            closeChamber(i);
          }

          // ------------------------------------------
          // ALREADY CONFIRMED
          // ------------------------------------------

          else {

            Serial.print(
              "CH"
            );

            Serial.print(
              i + 1
            );

            Serial.println(
              " already confirmed."
            );
          }
        }
      }
    }
  }
}


// ============================================================
// CHECK AUTOMATIC SCHEDULE
// ============================================================

void checkSchedules() {

  DateTime now =
    rtc.now();

  int nowMinute =
    now.hour() * 60 +
    now.minute();

  // Prevent checking the same minute repeatedly.
  if (
    nowMinute ==
    lastScheduleMinute
  ) {
    return;
  }

  lastScheduleMinute =
    nowMinute;

  for (int i = 0; i < SERVO_COUNT; i++) {

    if (
      chamber[i].scheduleHandledToday
    ) {
      continue;
    }

    if (
      nowMinute ==
      scheduleMinutes[i]
    ) {

      if (
        openChamber(
          i,
          true
        )
      ) {

        chamber[i].scheduleHandledToday =
          true;

        saveDailyStatus(i);

        Serial.print(
          "SCHEDULE STARTED - CH"
        );

        Serial.print(
          i + 1
        );

        Serial.print(
          " @ "
        );

        Serial.println(
          formatMinutes(
            scheduleMinutes[i]
          )
        );
      }
    }
  }
}


// ============================================================
// NEW DAY
// ============================================================

void handleNewDay() {

  DateTime now =
    rtc.now();

  if (
    now.year() != currentYear ||
    now.month() != currentMonth ||
    now.day() != currentDay
  ) {

    currentYear =
      now.year();

    currentMonth =
      now.month();

    currentDay =
      now.day();

    lastScheduleMinute =
      -1;

    for (int i = 0; i < SERVO_COUNT; i++) {

      chamber[i].scheduleHandledToday =
        false;

      if (
        !chamber[i].active
      ) {

        chamber[i].status =
          STATUS_IDLE;

        chamber[i].automatic =
          false;

        chamber[i].waitingForConfirmation =
          false;

        chamber[i].waitingToClose =
          false;

        chamber[i].doseStartedAt =
          0;

        chamber[i].confirmationAt =
          0;
      }
    }

    preferences.begin(
      "daily",
      false
    );

    preferences.putUInt(
      "date",
      todayKey()
    );

    for (int i = 0; i < SERVO_COUNT; i++) {

      String h =
        "h" + String(i);

      String s =
        "s" + String(i);

      preferences.putBool(
        h.c_str(),
        false
      );

      preferences.putUChar(
        s.c_str(),
        STATUS_IDLE
      );
    }

    preferences.end();

    Serial.println(
      "NEW DAY - STATUS RESET"
    );
  }
}


// ============================================================
// NORMAL LCD
// ============================================================

void updateNormalLCD() {

  if (
    millis() <=
    lcdMessageUntil
  ) {
    return;
  }

  DateTime now =
    rtc.now();

  lcd.setCursor(0, 0);

  String clockLine =
    "Time " +
    twoDigit(now.hour()) +
    ":" +
    twoDigit(now.minute()) +
    ":" +
    twoDigit(now.second());

  clockLine +=
    "           ";

  lcd.print(
    clockLine.substring(0, 16)
  );

  int currentMinutes =
    now.hour() * 60 +
    now.minute();

  int nextChamber =
    -1;

  int bestDiff =
    1441;

  for (int i = 0; i < SERVO_COUNT; i++) {

    if (
      chamber[i].scheduleHandledToday
    ) {
      continue;
    }

    int diff =
      scheduleMinutes[i] -
      currentMinutes;

    if (diff < 0) {
      diff += 1440;
    }

    if (
      diff < bestDiff
    ) {

      bestDiff =
        diff;

      nextChamber =
        i;
    }
  }

  lcd.setCursor(0, 1);

  if (
    nextChamber >= 0
  ) {

    String line =
      "CH" +
      String(nextChamber + 1) +
      " " +
      formatMinutes(
        scheduleMinutes[nextChamber]
      );

    line +=
      "            ";

    lcd.print(
      line.substring(0, 16)
    );

  } else {

    lcd.print(
      "No Schedule     "
    );
  }
}


// ============================================================
// STATUS JSON
// ============================================================

String createStatusJSON() {

  DateTime now =
    rtc.now();

  String json =
    "{";

  json +=
    "\"device\":\"" +
    String(DEVICE_ID) +
    "\",";

  json +=
    "\"online\":true,";

  json +=
    "\"ip\":\"" +
    WiFi.localIP().toString() +
    "\",";

  json +=
    "\"rssi\":" +
    String(WiFi.RSSI()) +
    ",";

  json +=
    "\"time\":\"" +
    twoDigit(now.hour()) +
    ":" +
    twoDigit(now.minute()) +
    ":" +
    twoDigit(now.second()) +
    "\",";

  json +=
    "\"date\":\"" +
    String(now.year()) +
    "-" +
    twoDigit(now.month()) +
    "-" +
    twoDigit(now.day()) +
    "\",";

  json +=
    "\"timezone\":\"IST\",";

  json +=
    "\"utcOffset\":\"+05:30\",";

  json +=
    "\"wifi\":" +
    String(
      WiFi.status() == WL_CONNECTED
        ? "true"
        : "false"
    ) +
    ",";

  json +=
    "\"chambers\":[";

  for (int i = 0; i < SERVO_COUNT; i++) {

    if (i > 0) {
      json += ",";
    }

    json += "{";

    json +=
      "\"chamber\":" +
      String(i + 1) +
      ",";

    json +=
      "\"buttonGPIO\":" +
      String(buttonPin[i]) +
      ",";

    json +=
      "\"quantity\":" +
      String(medicineQty[i]) +
      ",";

    json +=
      "\"schedule\":\"" +
      formatMinutes(
        scheduleMinutes[i]
      ) +
      "\",";

    json +=
      "\"active\":" +
      String(
        chamber[i].active
          ? "true"
          : "false"
      ) +
      ",";

    json +=
      "\"automatic\":" +
      String(
        chamber[i].automatic
          ? "true"
          : "false"
      ) +
      ",";

    json +=
      "\"status\":\"" +
      statusText(
        chamber[i].status
      ) +
      "\",";

    json +=
      "\"scheduleHandled\":" +
      String(
        chamber[i].scheduleHandledToday
          ? "true"
          : "false"
      ) +
      ",";

    json +=
      "\"waiting\":" +
      String(
        chamber[i].waitingForConfirmation
          ? "true"
          : "false"
      ) +
      ",";

    int countdown = 0;

    if (
      chamber[i].waitingToClose
    ) {

      unsigned long elapsed =
        millis() -
        chamber[i].confirmationAt;

      if (
        elapsed <
        CLOSE_DELAY_MS
      ) {

        countdown =
          (
            CLOSE_DELAY_MS -
            elapsed +
            999
          ) / 1000;
      }
    }

    json +=
      "\"countdown\":" +
      String(countdown);

    json +=
      "}";
  }

  json +=
    "]";

  json +=
    "}";

  return json;
}


// ============================================================
// /status
// ============================================================

void handleStatus() {

  enableCORS();

  server.send(
    200,
    "application/json",
    createStatusJSON()
  );
}


// ============================================================
// /api/status
// ============================================================

void handleAPIStatus() {

  enableCORS();

  server.send(
    200,
    "application/json",
    createStatusJSON()
  );
}


// ============================================================
// DISPENSE
// ============================================================

void handleDispense() {

  enableCORS();

  if (
    !server.hasArg(
      "chamber"
    )
  ) {

    server.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Missing chamber\"}"
    );

    return;
  }

  int chamberNumber =
    server.arg(
      "chamber"
    ).toInt();

  if (
    chamberNumber < 1 ||
    chamberNumber > 4
  ) {

    server.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Chamber must be 1-4\"}"
    );

    return;
  }

  int index =
    chamberNumber - 1;

  // If automatic chamber is waiting,
  // dashboard command confirms dose.
  if (
    chamber[index].active &&
    chamber[index].automatic &&
    chamber[index].waitingForConfirmation
  ) {

    confirmDose(index);

  } else {

    // Dashboard manual dispense
    if (
      chamber[index].active
    ) {

      server.send(
        409,
        "application/json",
        "{\"ok\":false,\"error\":\"Chamber already active\"}"
      );

      return;
    }

    if (
      !openChamber(
        index,
        false
      )
    ) {

      server.send(
        409,
        "application/json",
        "{\"ok\":false,\"error\":\"Unable to open chamber or chamber empty\"}"
      );

      return;
    }

    // Dashboard-triggered manual dispense:
    // count the dose immediately.
    if (
      medicineQty[index] > 0
    ) {

      medicineQty[index]--;

      chamber[index].status =
        STATUS_TAKEN;

      chamber[index].scheduleHandledToday =
        true;

      saveDailyStatus(index);
    }
  }

  String response =
    "{";

  response +=
    "\"ok\":true,";

  response +=
    "\"message\":\"Medicine dispensed\",";

  response +=
    "\"chamber\":" +
    String(chamberNumber) +
    ",";

  response +=
    "\"remaining\":" +
    String(
      medicineQty[index]
    );

  response +=
    "}";

  server.send(
    200,
    "application/json",
    response
  );
}


// ============================================================
// API SET SCHEDULE
// /api/set?chamber=1&time=08:00
// ============================================================

void handleAPISet() {

  enableCORS();

  if (
    !server.hasArg("chamber") ||
    !server.hasArg("time")
  ) {

    server.send(
      400,
      "text/plain",
      "Missing chamber or time"
    );

    return;
  }

  int chamberNumber =
    server.arg(
      "chamber"
    ).toInt();

  String time =
    server.arg(
      "time"
    );

  if (
    chamberNumber < 1 ||
    chamberNumber > 4
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid chamber"
    );

    return;
  }

  if (
    time.length() != 5
  ) {

    server.send(
      400,
      "text/plain",
      "Use HH:MM"
    );

    return;
  }

  int colon =
    time.indexOf(':');

  if (
    colon != 2
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid time format"
    );

    return;
  }

  int hour =
    time.substring(
      0,
      colon
    ).toInt();

  int minute =
    time.substring(
      colon + 1
    ).toInt();

  if (
    hour < 0 ||
    hour > 23 ||
    minute < 0 ||
    minute > 59
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid time"
    );

    return;
  }

  int index =
    chamberNumber - 1;

  scheduleMinutes[index] =
    hour * 60 + minute;

  chamber[index].scheduleHandledToday =
    false;

  if (
    !chamber[index].active
  ) {

    chamber[index].status =
      STATUS_IDLE;
  }

  saveSchedules();

  saveDailyStatus(index);

  lastScheduleMinute =
    -1;

  lcdMessageUntil =
    0;

  updateNormalLCD();

  String response =
    "Schedule saved: CH";

  response +=
    String(chamberNumber);

  response +=
    " = ";

  response +=
    formatMinutes(
      scheduleMinutes[index]
    );

  Serial.println(
    response
  );

  server.send(
    200,
    "text/plain",
    response
  );
}


// ============================================================
// RTC GET
// /api/rtc
// ============================================================

void handleAPIRTC() {

  enableCORS();

  DateTime now =
    rtc.now();

  String response =
    twoDigit(now.hour());

  response += ":";

  response +=
    twoDigit(now.minute());

  response += ":";

  response +=
    twoDigit(now.second());

  server.send(
    200,
    "text/plain",
    response
  );
}


// ============================================================
// RTC SET
// /api/rtc-set?time=14:30:00
// ============================================================

void handleAPIRTCSet() {

  enableCORS();

  if (
    !server.hasArg("time")
  ) {

    server.send(
      400,
      "text/plain",
      "Missing time"
    );

    return;
  }

  String time =
    server.arg(
      "time"
    );

  if (
    time.length() != 5 &&
    time.length() != 8
  ) {

    server.send(
      400,
      "text/plain",
      "Use HH:MM or HH:MM:SS"
    );

    return;
  }

  int firstColon =
    time.indexOf(':');

  int secondColon =
    time.indexOf(
      ':',
      firstColon + 1
    );

  if (
    firstColon != 2
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid time format"
    );

    return;
  }

  int hour =
    time.substring(
      0,
      firstColon
    ).toInt();

  int minute =
    time.substring(
      firstColon + 1,
      secondColon > 0
        ? secondColon
        : time.length()
    ).toInt();

  int second = 0;

  if (
    secondColon > 0
  ) {

    second =
      time.substring(
        secondColon + 1
      ).toInt();
  }

  if (
    hour < 0 ||
    hour > 23 ||
    minute < 0 ||
    minute > 59 ||
    second < 0 ||
    second > 59
  ) {

    server.send(
      400,
      "text/plain",
      "Invalid time"
    );

    return;
  }

  DateTime oldTime =
    rtc.now();

  DateTime newTime(
    oldTime.year(),
    oldTime.month(),
    oldTime.day(),
    hour,
    minute,
    second
  );

  rtc.adjust(
    newTime
  );

  currentYear =
    newTime.year();

  currentMonth =
    newTime.month();

  currentDay =
    newTime.day();

  lastScheduleMinute =
    -1;

  lcdMessageUntil =
    millis() + 3000;

  lcd.clear();

  lcd.setCursor(0, 0);

  lcd.print("RTC UPDATED");

  lcd.setCursor(0, 1);

  lcd.print(
    twoDigit(hour) +
    ":" +
    twoDigit(minute) +
    ":" +
    twoDigit(second)
  );

  Serial.printf(
    "RTC SET MANUALLY: %02d:%02d:%02d IST\n",
    hour,
    minute,
    second
  );

  String response =
    "RTC updated to ";

  response +=
    twoDigit(hour);

  response += ":";

  response +=
    twoDigit(minute);

  response += ":";

  response +=
    twoDigit(second);

  server.send(
    200,
    "text/plain",
    response
  );
}


// ============================================================
// RTC NTP SYNC
// /api/rtc-sync
// ============================================================

void handleAPIRTCSync() {

  enableCORS();

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    server.send(
      503,
      "text/plain",
      "WiFi not connected"
    );

    return;
  }

  if (
    syncRTCFromNTP()
  ) {

    lcdMessageUntil =
      millis() + 3000;

    showLCD(
      "NTP SYNC",
      "Successful"
    );

    server.send(
      200,
      "text/plain",
      "RTC synchronized with NTP"
    );

  } else {

    server.send(
      500,
      "text/plain",
      "NTP synchronization failed"
    );
  }
}


// ============================================================
// OPTIONS
// ============================================================

void handleOptions() {

  enableCORS();

  server.send(
    204,
    "text/plain",
    ""
  );
}


// ============================================================
// ROOT
// ============================================================

void handleRoot() {

  enableCORS();

  String html;

  html +=
    "<!DOCTYPE html>";

  html +=
    "<html><head>";

  html +=
    "<meta name='viewport' content='width=device-width,initial-scale=1'>";

  html +=
    "<title>MEDINOSA ESP32</title>";

  html +=
    "<style>"
    "body{font-family:Arial;text-align:center;background:#f2f4f7;padding:20px}"
    ".box{background:white;padding:25px;border-radius:15px;max-width:700px;margin:auto}"
    "button{padding:12px 20px;margin:5px;border:0;border-radius:8px;background:#315ca8;color:white}"
    "pre{text-align:left;background:#172033;color:white;padding:15px;border-radius:10px;overflow:auto}"
    "</style>";

  html +=
    "</head><body>";

  html +=
    "<div class='box'>";

  html +=
    "<h1>MEDINOSA</h1>";

  html +=
    "<h3>ESP32 Medicine Dispenser</h3>";

  html +=
    "<p><b>Device:</b> ";

  html +=
    DEVICE_ID;

  html +=
    "</p>";

  html +=
    "<p><b>IP:</b> ";

  html +=
    WiFi.localIP().toString();

  html +=
    "</p>";

  html +=
    "<button onclick='load()'>Refresh Status</button>";

  html +=
    "<pre id='data'>Loading...</pre>";

  html +=
    "</div>";

  html +=
    "<script>"
    "function load(){"
    "fetch('/status?_='+Date.now(),{cache:'no-store'})"
    ".then(r=>r.json())"
    ".then(d=>document.getElementById('data').innerText=JSON.stringify(d,null,2))"
    ".catch(e=>document.getElementById('data').innerText=e)"
    "}"
    "load();"
    "setInterval(load,2000);"
    "</script>";

  html +=
    "</body></html>";

  server.send(
    200,
    "text/html",
    html
  );
}


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(
    115200
  );

  delay(1000);

  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "      MEDINOSA ESP32 STARTING"
  );

  Serial.println(
    "=========================================="
  );

  // Initialize hardware
  initializeHardware();

  // Initialize RTC
  if (
    !initializeRTC()
  ) {

    showLCD(
      "RTC ERROR",
      "Check DS3231"
    );
  }

  // Initialize chamber states
  initializeChambers();

  // Load schedules
  loadSchedules();

  // Load today's status
  loadDailyStatus();

  // Date tracking
  DateTime now =
    rtc.now();

  currentYear =
    now.year();

  currentMonth =
    now.month();

  currentDay =
    now.day();

  // WiFi
  connectWiFi();

  // ----------------------------------------------------------
  // ROUTES
  // ----------------------------------------------------------

  server.on(
    "/",
    HTTP_GET,
    handleRoot
  );

  server.on(
    "/status",
    HTTP_GET,
    handleStatus
  );

  server.on(
    "/api/status",
    HTTP_GET,
    handleAPIStatus
  );

  server.on(
    "/dispense",
    HTTP_GET,
    handleDispense
  );

  server.on(
    "/dispense",
    HTTP_POST,
    handleDispense
  );

  server.on(
    "/api/set",
    HTTP_GET,
    handleAPISet
  );

  server.on(
    "/api/rtc",
    HTTP_GET,
    handleAPIRTC
  );

  server.on(
    "/api/rtc-set",
    HTTP_GET,
    handleAPIRTCSet
  );

  server.on(
    "/api/rtc-sync",
    HTTP_GET,
    handleAPIRTCSync
  );

  server.on(
    "/status",
    HTTP_OPTIONS,
    handleOptions
  );

  server.on(
    "/api/status",
    HTTP_OPTIONS,
    handleOptions
  );

  server.on(
    "/dispense",
    HTTP_OPTIONS,
    handleOptions
  );

  server.on(
    "/api/set",
    HTTP_OPTIONS,
    handleOptions
  );

  server.on(
    "/api/rtc",
    HTTP_OPTIONS,
    handleOptions
  );

  server.on(
    "/api/rtc-set",
    HTTP_OPTIONS,
    handleOptions
  );

  server.begin();

  Serial.println();
  Serial.println(
    "=========================================="
  );

  Serial.println(
    "MEDINOSA SERVER STARTED"
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "Open: http://"
    );

    Serial.print(
      WiFi.localIP()
    );

    Serial.println(
      "/status"
    );
  }

  Serial.println(
    "=========================================="
  );

  showLCD(
    "MEDINOSA READY",
    "System Online"
  );
}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // Handle HTTP requests
  server.handleClient();


  // ----------------------------------------------------------
  // WIFI RECONNECT
  // ----------------------------------------------------------

  if (
    millis() -
    lastWiFiCheck >
    10000UL
  ) {

    lastWiFiCheck =
      millis();

    if (
      WiFi.status() !=
      WL_CONNECTED
    ) {

      Serial.println(
        "WiFi disconnected. Reconnecting..."
      );

      WiFi.disconnect();

      WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
      );
    }
  }


  // ----------------------------------------------------------
  // NEW DAY
  // ----------------------------------------------------------

  handleNewDay();


  // ----------------------------------------------------------
  // AUTOMATIC SCHEDULE
  // ----------------------------------------------------------

  checkSchedules();


  // ----------------------------------------------------------
  // BUTTONS
  // ----------------------------------------------------------

  processButtons();


  // ----------------------------------------------------------
  // CHAMBER TIMERS
  // ----------------------------------------------------------

  for (
    int i = 0;
    i < SERVO_COUNT;
    i++
  ) {

    processChamber(i);
  }


  // ----------------------------------------------------------
  // LCD
  // ----------------------------------------------------------

  updateNormalLCD();
}