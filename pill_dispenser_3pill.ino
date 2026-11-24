/*
 * ============================================================
 *  Smart Pill Dispenser v3 — ESP32 code
 *  (Arduino framework, ESP32 DevKit)
 * ============================================================
 *  WHAT THIS MACHINE DOES (3-PILL version, Sir's instruction 2026-11-10)
 *  - 3 towers, one pill type in each tower (tower 1 = Panadol, ...)
 *  - Each tower holds 6 pouches: 5 stacked in the tube + 1 on the gate.
 *  - Each pill is ONE sealed pouch (small plastic sheet, 1 pill).
 *  - The family member sets up to 4 dose times a day in the Blynk
 *    phone app, and for each dose how many pouches (0-3) come out
 *    of each box.
 *  - At a dose time the machine buzzes, shows the dose on the
 *    screen, and drops the pouches into the front tray.
 *  - It counts how many pouches are left in each box, warns when a
 *    box is low, and tells the family if a dose was not taken.
 *
 *  BEFORE YOU UPLOAD — FILL IN THESE 5 THINGS (just below):
 *    1. BLYNK_TEMPLATE_ID    - from the Blynk console (your template)
 *    2. BLYNK_TEMPLATE_NAME  - your template name
 *    3. BLYNK_AUTH_TOKEN     - from your Blynk device
 *    4. ssid                 - your WiFi name
 *    5. pass                 - your WiFi password
 *  See pill_dispenser_blynk_setup.md for the step-by-step.
 *
 *  HARDWARE (pins are fixed — do not change them):
 *    Servos (SG90, tower 1..3): GPIO 13, 12, 14
 *    NO LCD in this build (removed 2026-11-13). Status goes to Serial
 *      and the Blynk app (V90) only — there is no screen. The LCD
 *      version of this file is pill_dispenser_3pill_WITH_LCD.ino;
 *      reconnect the LCD for the demo screen and use that one.
 *    I2C bus (clock only now): SDA 25, SCL 27. The 30-pin breadboard
 *      has no access to the LEFT side below D21, so ALL wiring is on
 *      the RIGHT side only - D13/D12/D14 servos, D25 SDA, D27 SCL,
 *      D26 buzzer, D34 IR sensor. Left side is unused.
 *    I2C PIN FIX 2026-11-13: rtc.begin() can switch the I2C bus back
 *      to the ESP32 defaults (21/22), which hides the clock even when
 *      it is wired correctly on 25/27. This code calls Wire.begin(25,
 *      27) again after rtc.begin(), and checks the clock directly at
 *      0x68, so the clock is found whenever it answers on 25/27.
 *    DS3231 clock module: same I2C bus (25/27)
 *    Buzzer (active): GPIO 26 (moved 23 -> 26 for the same reason)
 *    IR sensor in the tray: GPIO 34 (LOW = a pouch is sitting there)
 *    Test button: NONE FITTED - checkButton() is disabled in code, so no
 *      10k resistor and no D35-to-3V3 jumper are needed. Test dispense
 *      is via the Blynk app buttons only.
 *
 *  LIBRARIES (Arduino IDE > Sketch > Include Library > Manage Libraries):
 *    ESP32Servo, RTClib, Blynk (by Volodymyr Shymanskyy)  (no LCD library — no LCD fitted)
 *    Board: ESP32 DevKit (install "esp32 by Espressif Systems")
 *
 *  NOTE: This file was written on a machine without the Arduino
 *  toolchain, so it has NOT been compile-tested there. If the
 *  Arduino IDE reports an error, it is most likely a missing
 *  library or the wrong board choice — install the four libraries
 *  above and pick the ESP32 DevKit board, then try again.
 */

// ---------- PASTE YOUR OWN VALUES HERE ----------
#define BLYNK_TEMPLATE_ID   "TMPL626DLZnlz"    // <-- your template ID from the Blynk console
#define BLYNK_TEMPLATE_NAME "Dawabox"    // <-- your template name
#define BLYNK_AUTH_TOKEN    "YourAuthTokenHere" // <-- your device auth token from Blynk

#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>

// ---------- WiFi (paste your own) ----------
char ssid[] = "YourWiFiName";      // <-- your WiFi name
char pass[] = "YourWiFiPassword";  // <-- your WiFi password

// ---------- Pins (fixed — these match the 3-pill wiring) ----------
const uint8_t SERVO_PINS[3] = {13, 12, 14};  // 3 servos, one per tower (pins 13, 12, 14)
const uint8_t I2C_SDA_PIN = 25;
const uint8_t I2C_SCL_PIN = 27;
const uint8_t BUZZER_PIN = 26;
const uint8_t IR_PIN = 34;   // tray sensor: LOW = a hand is in front of it.
                             // The IR ONLY detects the HAND reaching in to
                             // pick up the pouch. It does NOT detect pouches
                             // (they are too small / out of range), and it is
                             // NEVER used to confirm that a pouch dropped.
const uint8_t BTN_PIN = 35;  // test button: LOW = pressed

// ---------- Settings ----------
// v9 (2026-11-09, night): NO SLIDER. A bent 2mm wire from the servo horn
// drives a small pushrod that kicks the bottom pouch forward off the gate.
// The horn radius is 8mm; the 15->105 degree command (90 deg swing) gives
// about 9mm ideal horizontal horn-tip displacement — LESS than the ~15mm
// needed to tip a pouch off the gate, so a 90-degree kick may NOT fully
// dispense. If a pouch does not tip, raise SERVO_KICK back up; if the
// pushrod slams too hard, lower it. Test by hand before trusting it.
const int SERVO_HOME = 15;   // pushrod fully back (rest position)
const int SERVO_KICK = 105;  // pushrod fully forward (90 deg from home)
const int ROCK_WAIT_MS = 400;   // wait after moving forward, and after returning
const int LOW_LIMIT = 3;        // a box is "low" when 3 or fewer pouches are left
const unsigned long MISSED_AFTER_MS = 10UL * 60UL * 1000UL; // 10 minutes
const int MAX_DOSES = 4;
const int NBOX = 3;  // 3-PILL version (Sir, 2026-11-10): 3 towers, 6 pills each
const int MAX_QTY = 3;       // most pouches one box gives in one dose
const int MAX_LOADED = 6;    // physical capacity: 5 stacked + 1 on the gate

// ---------- Objects ----------
RTC_DS3231 rtc;
Preferences prefs;

// ---------- Dose plan (set in the Blynk app, saved in the ESP32 memory) ----------
struct Dose {
  bool enabled;       // true once a time has been set for this dose
  uint8_t hour;
  uint8_t minute;
  uint8_t days;       // bit 0 = Monday ... bit 6 = Sunday. 0 = every day.
  uint8_t qty[NBOX];  // pouches from each box, 0..3
};
Dose doses[MAX_DOSES];
long lastFiredKey[MAX_DOSES];  // persisted: stops a reboot re-firing a dose in the same minute

int loadedCount[NBOX];     // pouches loaded at the last refill
int remainingCount[NBOX];  // pouches left right now
bool lowNotified[NBOX];    // true once we already warned about this box

bool doseRunning = false;
bool rtcReady = false;   // true only if the DS3231 clock answered on I2C.
                         // When false (clock not wired / not found) the
                         // sketch keeps running: Wi-Fi/Blynk and the
                         // Blynk test button still work, but timed doses
                         // stay off because there is no clock to run them.
bool missedCheckPending = false;
unsigned long doseDoneMillis = 0;
unsigned long lastWifiRetry = 0;

// Test button (debounce)
int buttonState = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 50;

// ============================================================
//  Small helpers
// ============================================================

// Short beep (the buzzer is ACTIVE: HIGH = sound on, LOW = off)
void beepShort() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);
  delay(100);
}

// Long error beep
void beepLong() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(800);
  digitalWrite(BUZZER_PIN, LOW);
}

// Show a short status text in the Blynk app (V90) and on Serial
void setStatus(const char* text) {
  Serial.println(text);
  if (Blynk.connected()) Blynk.virtualWrite(V90, text);
}

// Send the remaining pouch count of every box to the app (V80-V89)
void sendRemainingToBlynk() {
  if (!Blynk.connected()) return;
  for (int b = 0; b < NBOX; b++) {
    Blynk.virtualWrite(80 + b, remainingCount[b]);
  }
}

// ---------- Save / load (so a power cut does not erase the plan) ----------
void saveDoses() {
  prefs.begin("pill", false);
  for (int d = 0; d < MAX_DOSES; d++) {
    char k[8];
    snprintf(k, sizeof(k), "dh%d", d); prefs.putUChar(k, doses[d].hour);
    snprintf(k, sizeof(k), "dm%d", d); prefs.putUChar(k, doses[d].minute);
    snprintf(k, sizeof(k), "dd%d", d); prefs.putUChar(k, doses[d].days);
    snprintf(k, sizeof(k), "de%d", d); prefs.putBool(k, doses[d].enabled);
    snprintf(k, sizeof(k), "lf%d", d); prefs.putLong(k, lastFiredKey[d]);
    for (int b = 0; b < NBOX; b++) {
      snprintf(k, sizeof(k), "q%d%d", d, b); prefs.putUChar(k, doses[d].qty[b]);
    }
  }
  prefs.end();
}

void loadDoses() {
  prefs.begin("pill", true);
  for (int d = 0; d < MAX_DOSES; d++) {
    char k[8];
    snprintf(k, sizeof(k), "dh%d", d); doses[d].hour = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "dm%d", d); doses[d].minute = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "dd%d", d); doses[d].days = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "de%d", d); doses[d].enabled = prefs.getBool(k, false);
    snprintf(k, sizeof(k), "lf%d", d); lastFiredKey[d] = prefs.getLong(k, -1);
    for (int b = 0; b < NBOX; b++) {
      snprintf(k, sizeof(k), "q%d%d", d, b); doses[d].qty[b] = prefs.getUChar(k, 0);
    }
  }
  prefs.end();
}

void saveCounts() {
  prefs.begin("pill", false);
  for (int b = 0; b < NBOX; b++) {
    char k[8];
    snprintf(k, sizeof(k), "ld%d", b); prefs.putUChar(k, loadedCount[b]);
    snprintf(k, sizeof(k), "rm%d", b); prefs.putUChar(k, remainingCount[b]);
    snprintf(k, sizeof(k), "lw%d", b); prefs.putBool(k, lowNotified[b]);
  }
  prefs.end();
}

void loadCounts() {
  prefs.begin("pill", true);
  for (int b = 0; b < NBOX; b++) {
    char k[8];
    snprintf(k, sizeof(k), "ld%d", b); loadedCount[b] = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "rm%d", b); remainingCount[b] = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "lw%d", b); lowNotified[b] = prefs.getBool(k, false);
  }
  prefs.end();
}

// ============================================================
//  Servo / dispensing
// ============================================================

// Kick ONE pouch from one box: swing the servo 90 degrees forward
// (the wire should drive the pushrod about 9mm — less than the ~15mm
// needed to tip a pouch off the gate, so watch whether it fully dispenses),
// carries no weight, so it returns freely — no slider-return problem.
void dispenseOnePouch(int box) {
  Servo s;
  s.setPeriodHertz(50);
  s.attach(SERVO_PINS[box]);
  s.write(SERVO_HOME);
  delay(200);               // let the servo settle at home
  s.write(SERVO_KICK);      // 90 degrees forward — pushrod kicks the pouch
  delay(ROCK_WAIT_MS);      // 400 ms
  s.write(SERVO_HOME);      // back to the start
  delay(ROCK_WAIT_MS);      // 400 ms
  s.detach();               // stop holding: saves power, stops buzzing
}

// Drop "qty" pouches from one box. The servo kick IS the dispense
// (open-loop). The IR sensor is NOT used here at all: it only detects
// PICKUP (the hand taking the pouch), never the drop. So a dispense
// never "fails" because the sensor did not see the pouch — if the
// servo kicked, the pouch was kicked. Returns true when the servo
// kicked. The real proof a pouch landed is seeing it in the tray.
bool dispenseBox(int box, int qty) {
  for (int i = 0; i < qty; i++) {
    dispenseOnePouch(box);
    if (remainingCount[box] > 0) {
      remainingCount[box]--;
      saveCounts();
    }
  }
  return true;
}

// Warn once when a box gets low (3 or fewer pouches left)
void checkLow(int box) {
  if (remainingCount[box] <= LOW_LIMIT && !lowNotified[box]) {
    lowNotified[box] = true;
    saveCounts();
    String msg = "Box " + String(box + 1) + " running low, refill soon";
    if (Blynk.connected()) Blynk.logEvent("box_low", msg.c_str());
    setStatus("BOX LOW");
    delay(2000);
  } else if (remainingCount[box] > LOW_LIMIT) {
    lowNotified[box] = false;
  }
}

// ============================================================
//  Dose run
// ============================================================

bool doseHasPills(int d) {
  for (int b = 0; b < NBOX; b++) {
    if (doses[d].qty[b] > 0) return true;
  }
  return false;
}

void runDose(int d) {
  doseRunning = true;
  bool anyDispensed = false;

  // 3 short beeps: "your pills are coming"
  beepShort();
  beepShort();
  beepShort();


  for (int b = 0; b < NBOX; b++) {
    int q = doses[d].qty[b];
    if (q <= 0) continue;


    // The servo kick IS the dispense (open-loop — the IR is not used
    // to confirm the drop). dispenseBox returns true when it kicked.
    if (dispenseBox(b, q)) anyDispensed = true;
    checkLow(b);
    if (Blynk.connected()) Blynk.run();
  }

  setStatus("DOSE DONE");

  // Start the 10-minute hand check whenever at least one pouch was
  // kicked out. The IR watches for the hand reaching in to take the
  // pouch. If a hand is detected within 10 minutes, the dose was taken.
  if (anyDispensed) {
    doseDoneMillis = millis();
    missedCheckPending = true;
  }

  sendRemainingToBlynk();
  delay(1500);
  updateClockDisplay();
  doseRunning = false;
}

// ============================================================
//  Time and dose matching
// ============================================================

// Does this dose run on this weekday? days: bit 0 = Monday ... bit 6 = Sunday.
// 0 means no days were picked in the app, which we treat as "every day".
bool dayMatches(uint8_t days, const DateTime& now) {
  if (days == 0) return true;
  int dow = now.dayOfTheWeek();   // 0 = Sunday ... 6 = Saturday
  int mon0 = (dow + 6) % 7;       // 0 = Monday ... 6 = Sunday
  return (days & (1 << mon0)) != 0;
}

// Check once a minute: is a dose due right now?
void checkDoses() {
  if (doseRunning) return;
  if (!rtcReady) return;   // no clock, no timed doses
  DateTime now = rtc.now();
  // Unique key for this minute (minutes since 1970-01-01). RTClib's
  // DateTime has no dayOfTheYear(), so use unixtime() instead.
  long key = (long)(now.unixtime() / 60);
  for (int d = 0; d < MAX_DOSES; d++) {
    if (!doses[d].enabled) continue;
    if (!doseHasPills(d)) continue;   // a dose with no pills is skipped
    if (now.hour() != doses[d].hour || now.minute() != doses[d].minute) continue;
    if (lastFiredKey[d] == key) continue;  // already fired this minute
    if (!dayMatches(doses[d].days, now)) continue;
    lastFiredKey[d] = key;
    saveDoses();
    runDose(d);
  }
}

// Find the next dose after "now" (today first, else the first one tomorrow).
// Returns false if no dose is set at all.
bool nextDose(const DateTime& now, int* outH, int* outM) {
  int nowMin = now.hour() * 60 + now.minute();
  int bestToday = 9999, bestTomorrow = 9999;
  int bh = 0, bm = 0, th = 0, tm = 0;
  DateTime tomorrow = now + TimeSpan(1, 0, 0, 0);
  for (int d = 0; d < MAX_DOSES; d++) {
    if (!doses[d].enabled || !doseHasPills(d)) continue;
    int m = doses[d].hour * 60 + doses[d].minute;
    if (dayMatches(doses[d].days, now) && m > nowMin && m < bestToday) {
      bestToday = m; bh = doses[d].hour; bm = doses[d].minute;
    }
    if (dayMatches(doses[d].days, tomorrow) && m < bestTomorrow) {
      bestTomorrow = m; th = doses[d].hour; tm = doses[d].minute;
    }
  }
  if (bestToday != 9999) { *outH = bh; *outM = bm; return true; }
  if (bestTomorrow != 9999) { *outH = th; *outM = tm; return true; }
  return false;
}

// ============================================================
//  Status display (no LCD fitted — updateClockDisplay is a no-op)
// ============================================================

void updateClockDisplay() {
  // No LCD fitted (removed 2026-11-13) — nothing to update on a screen.
  // Status goes to Serial and the Blynk app (V90) via setStatus().
  // When the LCD is reconnected, restore the display code here
  // (see pill_dispenser_3pill_WITH_LCD.ino).
}

// ============================================================
//  Missed dose check (10 minutes after a dose)
// ============================================================

void checkMissedDose() {
  if (!missedCheckPending) return;
  // IR is ONLY a hand detector: it senses the man reaching in to pick
  // up the pouch. The sensor does NOT detect pouches, so a LOW reading
  // means a HAND, not a pouch. If a hand is detected within 10 minutes
  // of the dose, the dose was taken — it worked.
  // Guard: ignore the first 2 s after the dose, so the pouch dropping
  // itself cannot fake a hand.
  if (millis() - doseDoneMillis > 2000UL && digitalRead(IR_PIN) == LOW) {
    missedCheckPending = false;
    if (Blynk.connected()) {
      Blynk.logEvent("dose_taken", "Dose taken - hand detected");
    }
    setStatus("DOSE TAKEN");
    delay(2000);
    updateClockDisplay();
    return;
  }
  if (millis() - doseDoneMillis < MISSED_AFTER_MS) return;
  // 10 minutes are up and no hand was detected — the dose was missed.
  missedCheckPending = false;
  if (Blynk.connected()) {
    Blynk.logEvent("dose_missed", "Dose not taken - no hand detected in 10 minutes");
  }
  setStatus("DOSE NOT TAKEN");
  delay(3000);
  updateClockDisplay();
}

// ============================================================
//  Test button (press = 1 pouch from box 1)
// ============================================================

// Test dispense: kick one pouch from one box. "TEST OK" means the
// servo kicked — it does NOT mean a pouch was seen in the tray, because
// the IR is not used to confirm the drop. The real proof is seeing
// the pouch land in the tray with your own eyes.
void testDispenseBox(int box) {
  dispenseBox(box, 1);
  setStatus("TEST OK");
  checkLow(box);
  sendRemainingToBlynk();
  delay(2000);
  updateClockDisplay();
}

void checkButton() {
  // NO PHYSICAL BUTTON FITTED (right-side-only wiring: the left side is
  // blocked, so there is no 3V3 jumper for D35 and no pull-up resistor).
  // Return at once so the floating D35 pin can never trigger a dispense.
  // Test dispense is via the Blynk app buttons only.
  return;
  int reading = digitalRead(BTN_PIN);
  if (reading != lastButtonState) lastDebounceTime = millis();
  if (millis() - lastDebounceTime > DEBOUNCE_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW && !doseRunning) {  // pressed
        testDispenseBox(0);
      }
    }
  }
  lastButtonState = reading;
}

// ============================================================
//  Blynk — values coming FROM the app
// ============================================================

// Dose times come from the DawaBox app as plain minutes since midnight
// on V0-V3 (e.g. 480 = 08:00). The app is the way to set dose times —
// no Time Input widget needed. Days = every day (0).
void setDoseTimeMinutes(int d, int minutes) {
  if (d < 0 || d >= MAX_DOSES) return;
  if (minutes < 0) minutes = 0;
  if (minutes > 1439) minutes = 1439;
  // Guard: a sync of a never-set pin arrives as 0. Do not let that
  // enable a midnight dose the user never asked for. (Once a dose is
  // enabled, 00:00 can be set normally.)
  if (minutes == 0 && !doses[d].enabled) return;
  doses[d].hour = minutes / 60;
  doses[d].minute = minutes % 60;
  doses[d].days = 0;        // every day
  doses[d].enabled = true;
  saveDoses();
  Serial.print("Dose ");
  Serial.print(d + 1);
  Serial.print(" set to ");
  Serial.print(doses[d].hour);
  Serial.print(":");
  Serial.println(doses[d].minute);
  updateClockDisplay();
}

BLYNK_WRITE(V0) { setDoseTimeMinutes(0, param.asInt()); }
BLYNK_WRITE(V1) { setDoseTimeMinutes(1, param.asInt()); }
BLYNK_WRITE(V2) { setDoseTimeMinutes(2, param.asInt()); }
BLYNK_WRITE(V3) { setDoseTimeMinutes(3, param.asInt()); }

// Set the quantity for one box in one dose (0-3). Called by the
// default handler below for V10-V49.
void setQty(int dose, int box, int value) {
  if (dose < 0 || dose >= MAX_DOSES || box < 0 || box >= NBOX) return;
  if (value < 0) value = 0;
  if (value > MAX_QTY) value = MAX_QTY;
  doses[dose].qty[box] = (uint8_t)value;
  saveDoses();
  updateClockDisplay();
}

// Set the loaded count for one box (done at refill, V50-V59).
// Refill means: remaining = what you just loaded.
void setLoaded(int box, int value) {
  if (box < 0 || box >= NBOX) return;
  if (value < 0) value = 0;
  if (value > MAX_LOADED) value = MAX_LOADED;
  loadedCount[box] = value;
  remainingCount[box] = value;
  lowNotified[box] = false;   // a refill clears the low warning
  saveCounts();
  sendRemainingToBlynk();
  Serial.print("Box ");
  Serial.print(box + 1);
  Serial.print(" refilled to ");
  Serial.println(value);
}

// One handler for all the number widgets: V10-V49 = dose quantities,
// V50-V59 = loaded counts at refill.
// (BLYNK_WRITE_DEFAULT catches every virtual pin that has no handler
// of its own. If your Blynk library version does not support it,
// replace this with one BLYNK_WRITE(Vn) handler per pin, each calling
// setQty() or setLoaded() the same way.)
BLYNK_WRITE_DEFAULT() {
  int pin = request.pin;
  int value = param.asInt();
  if (pin >= 10 && pin <= 49) {
    int dose = (pin - 10) / 10;
    int box = (pin - 10) % 10;
    setQty(dose, box, value);
  } else if (pin >= 50 && pin <= 59) {
    setLoaded(pin - 50, value);
  }
}

// Test buttons in the app (V60/V61/V62): dispense 1 pouch from box 1/2/3.
BLYNK_WRITE(V60) {
  if (param.asInt() == 1 && !doseRunning) testDispenseBox(0);
}
BLYNK_WRITE(V61) {
  if (param.asInt() == 1 && !doseRunning) testDispenseBox(1);
}
BLYNK_WRITE(V62) {
  if (param.asInt() == 1 && !doseRunning) testDispenseBox(2);
}

// Runs when the ESP32 connects to Blynk. We send the remaining counts
// and ask the app to re-send the dose times and quantities, so the
// ESP32 and the app agree after a reconnect.
BLYNK_CONNECTED() {
  sendRemainingToBlynk();
  Blynk.syncVirtual(V0, V1, V2, V3);
  for (int pin = 10; pin <= 49; pin++) Blynk.syncVirtual(pin);
  // NOTE: we do NOT sync V50-V59 (the loaded counts). Those live on
  // the ESP32 and survive a power cut. Syncing them would reset the
  // "remaining" count to whatever the app last saved.
}

// ============================================================
//  Setup and main loop
// ============================================================

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(IR_PIN, INPUT);
  // NOTE: GPIO 35 has NO internal pull-up on the ESP32, so INPUT_PULLUP
  // would do nothing here. The button needs a real 10k resistor from
  // GPIO 35 to 3.3V (see the wiring doc). That is why this is INPUT.
  pinMode(BTN_PIN, INPUT);

  // No LCD fitted (removed 2026-11-13) — status goes to Serial and
  // the Blynk app (V90) only.
  Serial.println("PILL DISPENSER starting...");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Start the clock library, then force the I2C pins back to 25/27
  // (rtc.begin() can switch them to the ESP32 defaults). Then check
  // for the clock directly at 0x68, so rtcReady is true whenever the
  // clock actually answers on 25/27. (Proven 2026-11-13: the clock
  // answers and ticks on 25/27 when the pins are forced back.)
  rtc.begin();
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.beginTransmission(0x68);
  rtcReady = (Wire.endTransmission() == 0);
  if (!rtcReady) {
    Serial.println("RTC not found - check the I2C wiring!");
    // Do NOT call rtc.lostPower()/rtc.adjust() here: with no clock
    // on the I2C bus those calls can hang the sketch before Wi-Fi/Blynk
    // ever start. Doses stay off (no clock), but Wi-Fi, Blynk and the
    // Blynk test button still work below.
  } else if (rtc.lostPower()) {
    // The clock battery was empty or the module is new: start the
    // clock at the time this code was compiled, then set the right
    // time in the app or by re-uploading. Doses will be wrong until
    // the clock is right.
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("RTC lost power - clock set to compile time!");
  }

  loadDoses();
  loadCounts();
  // lastFiredKey[] was restored from Preferences by loadDoses().

  // WiFi first (10 second try), then Blynk. If there is no WiFi, the
  // machine still works: doses run from the clock and the saved plan.
  // Blynk.run() in the loop will connect whenever WiFi comes back.
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("No WiFi - running offline, doses still run from the clock");
  }
  Blynk.config(BLYNK_AUTH_TOKEN);

  updateClockDisplay();
  setStatus("READY");
  sendRemainingToBlynk();
}

void loop() {
  // Keep WiFi and Blynk alive. Blynk.run() connects by itself when
  // WiFi is up, and does nothing harmful when it is not.
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetry > 30000UL) {
      lastWifiRetry = millis();
      WiFi.begin(ssid, pass);
    }
  } else {
    Blynk.run();
  }

  // Once a second (and not while a dose is running): refresh the clock
  // on the screen and check if a dose is due. Skipped entirely when
  // there is no clock, so a missing RTC can never stall the loop.
  static int lastSecond = -1;
  if (!rtcReady) {
    static unsigned long lastShow = 0;
    if (millis() - lastShow > 1000UL) {
      lastShow = millis();
      updateClockDisplay();
    }
    checkMissedDose();
    checkButton();
    return;
  }
  DateTime now = rtc.now();
  if (!doseRunning && now.second() != lastSecond) {
    lastSecond = now.second();
    updateClockDisplay();
    // Check near the top of each minute. Keep at least 2 minutes
    // between dose times, so one dose can finish before the next is due.
    if (now.second() < 5) checkDoses();
  }

  checkMissedDose();
  checkButton();
}
