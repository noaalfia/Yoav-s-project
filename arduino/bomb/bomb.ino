/*
  ===================================================================
   הפצצה המכנית — "שעון עם מחט"
   זרוע עם סיכה מתקדמת ב"תקתוקים" (דרך תמסורת גלגלי שיניים) עד שהיא פוגעת בבלון.
   צג 7-סגמנט מראה כמה זמן נשאר. הדרך היחידה לעצור: לפתור את חידת שפת הסימנים באתר.
  ===================================================================
   רכיבים:
   - Arduino Uno + כבל USB
   - מנוע DC צהוב (TT, 3–6V, עם תיבת הילוכים מובנית)       (המנוע)
   - דרייבר מנוע L298N + מארז 4 סוללות AA                   (מפעיל את המנוע)
   - 2 גלגלי שיניים: 12 שיניים (על המנוע) ו-36 שיניים (על ציר הזרוע)  (התמסורת)
   - צג 4 ספרות 7-סגמנט: TM1637 (פינים CLK/DIO) או HT16K33 (פינים SDA/SCL)
   - חיישן מרחק אולטרסוני HC-SR04                          (חיישן 1: מזהה שחקן ומפעיל את הפצצה)
   - זמזם פסיבי, לד אדום, לד ירוק, 2 נגדים 220Ω

   חיבורים (ראו גם wiring.png):
   L298N:   ENA -> 10 (להוריד את הג'מפר מ-ENA!),  IN1 -> 8,  IN2 -> 9
            OUT1 + OUT2 -> שני החוטים של המנוע
            12V -> פלוס של מארז הסוללות,  GND -> מינוס של הסוללות **וגם** GND של הארדואינו
            5V  -> 5V של הארדואינו (ולהוריד את ג'מפר 5V-EN: עם 6V הווסת של הכרטיס לא נותן 5V יציב)
   צג:      CLK (או SCL) -> A5,   DIO (או SDA) -> A4,   VCC -> 5V,   GND -> GND
   HC-SR04: VCC -> 5V,  TRIG -> 6,  ECHO -> 7,  GND -> GND
   זמזם:     +  -> 3,   - -> GND
   לד אדום:  רגל ארוכה -> 4 דרך נגד 220Ω,  רגל קצרה -> GND
   לד ירוק:  רגל ארוכה -> 5 דרך נגד 220Ω,  רגל קצרה -> GND
   פין 2:    שמור לחיישן השני

   ספריות להתקנה (Sketch -> Include Library -> Manage Libraries):
   - צג TM1637:  "TM1637" של Avishay Orpaz
   - צג HT16K33: "Adafruit LED Backpack" (מתקין לבד גם את Adafruit GFX)

   תקשורת עם האתר (Serial 9600):
   מקבל:  S = התחל,  D = נטרול (החידה נפתרה),  R = איפוס והחזרת הזרוע להתחלה
   שולח:  IDLE, ARMED, P:<אחוז>, BOOM, DEFUSED
  ===================================================================
*/

// ---------- איזה צג יש לך? ----------
// פינים שכתוב עליהם CLK ו-DIO  -> השאירו 0
// פינים שכתוב עליהם SDA ו-SCL  -> שנו ל-1
#define USE_HT16K33 0

#if USE_HT16K33
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include "Adafruit_LEDBackpack.h"
  Adafruit_7segment display7 = Adafruit_7segment();
#else
  #include <TM1637Display.h>
  TM1637Display display7(A5, A4);  // CLK, DIO
#endif

// ---------- הגדרות שאפשר לשנות ----------
const int  TIME_SECONDS   = 60;    // כמה זמן יש לשחקן
const int  PULSE_EVERY_MS = 500;   // כל כמה זמן יש "תקתוק" של המנוע
const int  PULSE_MS       = 40;    // כמה זמן המנוע עובד בכל תקתוק  <- את זה מכיילים!
const int  MOTOR_POWER    = 200;   // עוצמת המנוע בזמן תקתוק (0–255)
const int  FINAL_PUSH_MS  = 600;   // כשהזמן נגמר: דחיפה אחרונה כדי שהסיכה בטוח תפגע
const bool FORWARD_IS_IN1 = true;  // אם הזרוע זזה לכיוון ההפוך, שנו ל-false
const int  ARM_DISTANCE   = 60;    // ס"מ: שחקן קרוב מזה מפעיל את הפצצה
const int  ARM_HOLD_MS    = 1500;  // כמה זמן השחקן צריך לעמוד מול הפצצה

// ---------- פינים ----------
const int ENA_PIN = 10, IN1_PIN = 8, IN2_PIN = 9;
const int BUZZER_PIN = 3, RED_PIN = 4, GREEN_PIN = 5;
const int TRIG_PIN = 6, ECHO_PIN = 7;
// פין 2 שמור לחיישן השני

enum State { IDLE, ARMED, BOOM, DEFUSED, HOMING };
State state = IDLE;

unsigned long armedAtMs = 0;       // מתי הפצצה הופעלה
unsigned long lastPulseMs = 0;
unsigned long forwardMs = 0;       // כמה זמן בסך הכול המנוע דחף קדימה (בשביל החזרה)
unsigned long lastReportMs = 0;
unsigned long lastBeepMs = 0;
unsigned long nearSinceMs = 0;
int shownSeconds = -1;             // מה מוצג כרגע על הצג (כדי לא לכתוב שוב ושוב)

void setup() {
  Serial.begin(9600);
  pinMode(ENA_PIN, OUTPUT);
  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  motorStop();

#if USE_HT16K33
  display7.begin(0x70);
  display7.setBrightness(15);
#else
  display7.setBrightness(7);
#endif

  setState(IDLE);
}

void loop() {
  readSerial();

  switch (state) {
    case IDLE:    checkPlayer();       break;
    case ARMED:   runTowardBalloon();  break;
    case HOMING:  runHome();           break;
    case BOOM:    blinkZero();         break;
    case DEFUSED: break;  // הצג נשאר עם הזמן שנשאר. מחכים ל-R
  }
}

// ---------------- מצבים ----------------

void setState(State s) {
  state = s;
  digitalWrite(RED_PIN, s == ARMED || s == BOOM);
  digitalWrite(GREEN_PIN, s == DEFUSED);
  noTone(BUZZER_PIN);

  switch (s) {
    case IDLE:
      motorStop();
      showTime(TIME_SECONDS, true);
      Serial.println("IDLE");
      break;
    case ARMED:
      armedAtMs = millis();
      lastPulseMs = millis();
      Serial.println("ARMED");
      break;
    case BOOM:
      motorStop();
      showTime(0, true);
      Serial.println("BOOM");
      boomSound();
      break;
    case DEFUSED:
      motorStop();
      Serial.println("DEFUSED");
      defuseSound();
      break;
    case HOMING:
      motorStop();
      break;
  }
}

// חיישן 1: מחכים שמישהו יעמוד מול הפצצה
void checkPlayer() {
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
  delay(60);  // HC-SR04 צריך הפסקה קצרה בין מדידות
}

// הזמן רץ, הצג סופר אחורה, והמנוע מזיז את הזרוע ב"תקתוקים"
void runTowardBalloon() {
  unsigned long elapsed = millis() - armedAtMs;
  long totalMs = (long)TIME_SECONDS * 1000L;

  if ((long)elapsed >= totalMs) {
    // הזמן נגמר: דחיפה אחרונה כדי שהסיכה בטוח תפגע בבלון
    motorForward();
    delay(FINAL_PUSH_MS);
    forwardMs += FINAL_PUSH_MS;
    setState(BOOM);
    return;
  }

  // תקתוק: המנוע עובד PULSE_MS, ואז נח עד התקתוק הבא
  if (millis() - lastPulseMs >= (unsigned long)PULSE_EVERY_MS) {
    lastPulseMs = millis();
    motorForward();
    delay(PULSE_MS);
    motorStop();
    forwardMs += PULSE_MS;
  }

  // הצג: כמה שניות נשארו
  int secondsLeft = (totalMs - (long)elapsed + 999) / 1000;
  showTime(secondsLeft, false);

  // דיווח לאתר + צפצוף שמאיץ ככל שהזמן אוזל
  int percent = elapsed * 100L / totalMs;
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

// חזרה לנקודת ההתחלה: המנוע עובד אחורה בדיוק אותו זמן שעבד קדימה
void runHome() {
  if (forwardMs > 0) {
    motorBackward();
    delay(forwardMs);
    motorStop();
    forwardMs = 0;
  }
  setState(IDLE);
}

// בזמן "בום": הצג מהבהב 00:00
void blinkZero() {
  static unsigned long last = 0;
  static bool on = true;
  if (millis() - last > 400) {
    last = millis();
    on = !on;
    if (on) showTime(0, true);
    else clearDisplay();
  }
}

// ---------------- מנוע (L298N) ----------------

void motorForward() {
  digitalWrite(IN1_PIN, FORWARD_IS_IN1 ? HIGH : LOW);
  digitalWrite(IN2_PIN, FORWARD_IS_IN1 ? LOW : HIGH);
  analogWrite(ENA_PIN, MOTOR_POWER);
}

void motorBackward() {
  digitalWrite(IN1_PIN, FORWARD_IS_IN1 ? LOW : HIGH);
  digitalWrite(IN2_PIN, FORWARD_IS_IN1 ? HIGH : LOW);
  analogWrite(ENA_PIN, MOTOR_POWER);
}

void motorStop() {
  analogWrite(ENA_PIN, 0);
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, LOW);
}

// ---------------- צג ----------------

// מציג דקות:שניות, למשל 01:00 או 00:42
void showTime(int seconds, bool force) {
  if (seconds < 0) seconds = 0;
  if (!force && seconds == shownSeconds) return;
  shownSeconds = seconds;
  int mmss = (seconds / 60) * 100 + (seconds % 60);
#if USE_HT16K33
  display7.writeDigitNum(0, (mmss / 1000) % 10);
  display7.writeDigitNum(1, (mmss / 100) % 10);
  display7.drawColon(true);
  display7.writeDigitNum(3, (mmss / 10) % 10);
  display7.writeDigitNum(4, mmss % 10);
  display7.writeDisplay();
#else
  display7.showNumberDecEx(mmss, 0b01000000, true);  // 0b01000000 = הנקודתיים באמצע
#endif
}

void clearDisplay() {
  shownSeconds = -1;
#if USE_HT16K33
  display7.clear();
  display7.writeDisplay();
#else
  display7.clear();
#endif
}

// ---------------- עזר ----------------

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S' && state == IDLE) setState(ARMED);
    else if (c == 'D' && state == ARMED) setState(DEFUSED);
    else if (c == 'R') setState(HOMING);
  }
}

int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, 30000);  // עד ~5 מטר
  if (us == 0) return -1;
  return us / 58;
}

void boomSound() {
  for (int f = 1500; f > 200; f -= 20) { tone(BUZZER_PIN, f, 8); delay(8); }
  tone(BUZZER_PIN, 150, 800);
}

void defuseSound() {
  int notes[] = {523, 659, 784, 1047};
  for (int n : notes) { tone(BUZZER_PIN, n, 150); delay(180); }
}
