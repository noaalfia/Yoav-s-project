/*
  ===================================================================
   הפצצה המכנית — מחט על עגלה שמתקדמת לעבר בלון
  ===================================================================
   רכיבים:
   - Arduino Uno
   - מנוע צעד 28BYJ-48 + דרייבר ULN2003   (התנועה המכנית)
   - חיישן מרחק אולטרסוני HC-SR04          (חיישן 1: מזהה שחקן שעומד מול הפצצה)
   - מתג גבול (Limit switch / מיקרו-סוויץ')  (חיישן 2: מזהה שהעגלה הגיעה לבלון)
   - זמזם פסיבי (Passive buzzer), לד אדום, לד ירוק

   חיבורים:
   ULN2003  IN1..IN4  ->  8, 9, 10, 11
   HC-SR04  TRIG -> 6,  ECHO -> 7,  VCC -> 5V,  GND -> GND
   מתג גבול: רגל אחת -> 2, רגל שנייה -> GND   (משתמשים ב-INPUT_PULLUP)
   זמזם    -> 3,  רגל שנייה -> GND
   לד אדום -> 4 (דרך נגד 220Ω),  לד ירוק -> 5 (דרך נגד 220Ω)

   תקשורת עם האתר (Serial 9600):
   מקבל:  S = התחל,  D = נטרול (החידה נפתרה),  R = איפוס וחזרה להתחלה
   שולח:  IDLE, ARMED, P:<אחוז>, BOOM, DEFUSED
  ===================================================================
*/

#include <Stepper.h>

// ---------- הגדרות שאפשר לשנות ----------
const long TRAVEL_STEPS   = 6000;  // כמה צעדים עד הבלון (למדוד במציאות!)
const int  TIME_SECONDS   = 60;    // כמה זמן יש לשחקן
const int  ARM_DISTANCE   = 60;    // ס"מ — שחקן קרוב מזה מפעיל את הפצצה
const int  ARM_HOLD_MS    = 1500;  // כמה זמן השחקן צריך לעמוד כדי שיופעל
const bool AUTO_ARM       = true;  // false = רק האתר מתחיל את המשחק

// ---------- פינים ----------
const int TRIG_PIN = 6, ECHO_PIN = 7;
const int LIMIT_PIN = 2;
const int BUZZER_PIN = 3, RED_PIN = 4, GREEN_PIN = 5;

// 28BYJ-48: 2048 צעדים לסיבוב. סדר הפינים 8,10,9,11 הוא לא טעות — ככה הספרייה צריכה.
Stepper stepper(2048, 8, 10, 9, 11);

enum State { IDLE, ARMED, BOOM, DEFUSED, HOMING };
State state = IDLE;

long position = 0;                 // כמה צעדים העגלה התקדמה מההתחלה
unsigned long stepIntervalUs;      // כל כמה מיקרו-שניות לעשות צעד
unsigned long lastStepUs = 0;
unsigned long lastReportMs = 0;
unsigned long nearSinceMs = 0;
unsigned long lastBeepMs = 0;

void setup() {
  Serial.begin(9600);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LIMIT_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);

  stepper.setSpeed(14);  // סל"ד מקסימלי (28BYJ-48 לא אוהב יותר מ-~15)
  stepIntervalUs = (unsigned long)TIME_SECONDS * 1000000UL / TRAVEL_STEPS;
  if (stepIntervalUs < 2200) stepIntervalUs = 2200;  // מהר מזה המנוע מחליק

  setState(IDLE);
}

void loop() {
  readSerial();

  switch (state) {
    case IDLE:    checkPlayer();     break;
    case ARMED:   runTowardBalloon(); break;
    case HOMING:  runHome();         break;
    case BOOM:
    case DEFUSED: break;  // מחכים ל-R
  }
}

// ---------------- מצבים ----------------

void setState(State s) {
  state = s;
  digitalWrite(RED_PIN, s == ARMED || s == BOOM);
  digitalWrite(GREEN_PIN, s == DEFUSED);
  digitalWrite(BUZZER_PIN, LOW);

  switch (s) {
    case IDLE:    motorOff(); Serial.println("IDLE"); break;
    case ARMED:   Serial.println("ARMED"); break;
    case BOOM:    motorOff(); Serial.println("BOOM"); boomSound(); break;
    case DEFUSED: motorOff(); Serial.println("DEFUSED"); defuseSound(); break;
    case HOMING:  break;
  }
}

// חיישן 1: מחכים שמישהו יעמוד מול הפצצה
void checkPlayer() {
  if (!AUTO_ARM) return;
  int cm = readDistanceCm();
  if (cm > 0 && cm < ARM_DISTANCE) {
    if (nearSinceMs == 0) nearSinceMs = millis();
    if (millis() - nearSinceMs > ARM_HOLD_MS) {
      nearSinceMs = 0;
      setState(ARMED);
    }
  } else {
    nearSinceMs = 0;
  }
  delay(60);  // HC-SR04 צריך הפסקה קטנה בין מדידות
}

// התנועה המכנית: העגלה עם המחט מתקדמת צעד אחרי צעד
void runTowardBalloon() {
  // חיישן 2: העגלה נגעה במתג הגבול = הגענו לבלון
  if (digitalRead(LIMIT_PIN) == LOW || position >= TRAVEL_STEPS) {
    setState(BOOM);
    return;
  }

  unsigned long now = micros();
  if (now - lastStepUs >= stepIntervalUs) {
    lastStepUs = now;
    stepper.step(1);
    position++;
  }

  // דיווח לאתר + צפצוף שמאיץ ככל שמתקרבים
  int percent = position * 100L / TRAVEL_STEPS;
  if (millis() - lastReportMs > 250) {
    lastReportMs = millis();
    Serial.print("P:");
    Serial.println(percent);
  }
  unsigned long beepEvery = map(percent, 0, 100, 1000, 120);
  if (millis() - lastBeepMs > beepEvery) {
    lastBeepMs = millis();
    tone(BUZZER_PIN, 2000, 40);
  }
}

// חזרה לנקודת ההתחלה אחרי איפוס
void runHome() {
  if (position <= 0) {
    position = 0;
    setState(IDLE);
    return;
  }
  stepper.step(-1);
  position--;
}

// ---------------- עזר ----------------

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S' && state == IDLE) setState(ARMED);
    else if (c == 'D' && state == ARMED) setState(DEFUSED);
    else if (c == 'R') { digitalWrite(RED_PIN, LOW); digitalWrite(GREEN_PIN, LOW); state = HOMING; }
  }
}

int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, 30000);  // מקסימום ~5 מטר
  if (us == 0) return -1;
  return us / 58;
}

// מכבה את סלילי המנוע כדי שלא יתחמם כשהוא עומד
void motorOff() {
  for (int p = 8; p <= 11; p++) digitalWrite(p, LOW);
}

void boomSound() {
  for (int f = 1500; f > 200; f -= 20) { tone(BUZZER_PIN, f, 8); delay(8); }
  tone(BUZZER_PIN, 150, 800);
}

void defuseSound() {
  int notes[] = {523, 659, 784, 1047};
  for (int n : notes) { tone(BUZZER_PIN, n, 150); delay(180); }
}
