/*
  הפצצה: צג סופר אחורה, ומנוע צהוב מזיז זרוע עם סיכה ב"תקתוקים" לעבר בלון.
  פתרון החידה באתר שולח 'D' ועוצר הכול. מדריך מלא + שרטוט: BUILD.md, wiring.png

  חיבורים:
    L298N   ENA->10  IN1->8  IN2->9  OUT1/OUT2->מנוע  12V->+ סוללות  GND->- סוללות וגם GND ארדואינו
            5V->5V ארדואינו. להוריד את הג'מפרים מ-ENA ומ-5V-EN
    צג      CLK/SCL->A5  DIO/SDA->A4  VCC->5V  GND->GND
    HC-SR04 TRIG->6  ECHO->7  VCC->5V  GND->GND
    זמזם->3   לד אדום->4   לד ירוק->5 (לדים דרך נגד 220Ω)   פין 2 שמור לחיישן השני

  Serial 9600.  מקבל: S=התחל D=נטרול R=איפוס.  שולח: IDLE ARMED P:<אחוז> BOOM DEFUSED
*/

// סוג הצג: פינים CLK/DIO -> 0 (ספריית "TM1637" של Avishay Orpaz)
//          פינים SDA/SCL -> 1 (ספריית "Adafruit LED Backpack")
#define USE_HT16K33 0

#if USE_HT16K33
  #include <Adafruit_LEDBackpack.h>
  Adafruit_7segment disp;
#else
  #include <TM1637Display.h>
  TM1637Display disp(A5, A4);
#endif

// ---- כיול ----
const int  TIME_SECONDS   = 60;   // זמן לשחקן
const int  PULSE_MS       = 40;   // כמה זמן המנוע עובד בכל תקתוק. מכיילים עד שהזרוע מגיעה לבלון ב-00:00
const int  PULSE_EVERY_MS = 500;  // כל כמה זמן תקתוק
const int  MOTOR_POWER    = 200;  // 0–255
const int  FINAL_PUSH_MS  = 600;  // דחיפה אחרונה כשהזמן נגמר
const bool FLIP_MOTOR     = false; // הזרוע זזה הפוך? שנו ל-true
const int  ARM_CM         = 60;   // שחקן קרוב מזה מפעיל את הפצצה

const int ENA = 10, IN1 = 8, IN2 = 9, TRIG = 6, ECHO = 7, BUZZER = 3, RED = 4, GREEN = 5;

enum State { IDLE, ARMED, BOOM, DEFUSED } state;
unsigned long armedAt, lastPulse, lastBeep, lastReport, nearSince, pushedMs;
int shown = -1;  // השניות שמוצגות כרגע על הצג

void setup() {
  Serial.begin(9600);
  int outs[] = {ENA, IN1, IN2, TRIG, BUZZER, RED, GREEN};
  for (int p : outs) pinMode(p, OUTPUT);
#if USE_HT16K33
  disp.begin(0x70);
#else
  disp.setBrightness(7);
#endif
  setState(IDLE);
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'S' && state == IDLE) setState(ARMED);
    if (c == 'D' && state == ARMED) setState(DEFUSED);
    if (c == 'R') { motor(-1, pushedMs); pushedMs = 0; setState(IDLE); }  // הזרוע חוזרת לאחור אותו זמן שזזה קדימה
  }

  if (state == IDLE) waitForPlayer();
  if (state == ARMED) tick();
  if (state == BOOM) { if ((millis() / 400) % 2) show(0); else clearDisplay(); }  // 00:00 מהבהב
}

void setState(State s) {
  state = s;
  digitalWrite(RED, s == ARMED || s == BOOM);
  digitalWrite(GREEN, s == DEFUSED);
  const char* names[] = {"IDLE", "ARMED", "BOOM", "DEFUSED"};
  Serial.println(names[s]);

  if (s == IDLE) { show(TIME_SECONDS); nearSince = millis(); }  // אחרי איפוס השחקן צריך לעמוד שוב 1.5 שניות
  if (s == ARMED) armedAt = lastPulse = millis();
  if (s == BOOM) for (int f = 1500; f > 200; f -= 20) { tone(BUZZER, f, 8); delay(8); }
  int notes[] = {523, 659, 784, 1047};
  if (s == DEFUSED) for (int n : notes) { tone(BUZZER, n, 150); delay(180); }
}

// חיישן 1: מישהו עומד מול הפצצה 1.5 שניות -> מתחילים
void waitForPlayer() {
  digitalWrite(TRIG, HIGH); delayMicroseconds(10); digitalWrite(TRIG, LOW);
  long cm = pulseIn(ECHO, HIGH, 30000) / 58;  // 0 = אין הד
  if (cm == 0 || cm > ARM_CM) nearSince = millis();
  else if (millis() - nearSince > 1500) setState(ARMED);
  delay(60);
}

// הזמן רץ: תקתוק מנוע, צג, צפצוף ודיווח לאתר
void tick() {
  unsigned long elapsed = millis() - armedAt, total = TIME_SECONDS * 1000UL;
  if (elapsed >= total) { motor(1, FINAL_PUSH_MS); setState(BOOM); return; }

  if (millis() - lastPulse >= PULSE_EVERY_MS) { lastPulse = millis(); motor(1, PULSE_MS); }
  show((total - elapsed + 999) / 1000);

  int percent = elapsed * 100 / total;
  if (millis() - lastReport > 250) { lastReport = millis(); Serial.print("P:"); Serial.println(percent); }
  if (millis() - lastBeep > (unsigned long)map(percent, 0, 100, 1000, 120)) { lastBeep = millis(); tone(BUZZER, 2000, 40); }
}

// מזיז את המנוע לכיוון dir (1 קדימה, -1 אחורה) למשך ms, ועוצר
void motor(int dir, unsigned long ms) {
  bool fwd = (dir > 0) != FLIP_MOTOR;
  digitalWrite(IN1, fwd);
  digitalWrite(IN2, !fwd);
  analogWrite(ENA, MOTOR_POWER);
  delay(ms);
  analogWrite(ENA, 0);
  if (dir > 0) pushedMs += ms;
}

// מציג שניות כ-MM:SS (כותב לצג רק כשהמספר משתנה)
void show(int sec) {
  if (sec == shown) return;
  shown = sec;
  int mmss = sec / 60 * 100 + sec % 60;
#if USE_HT16K33
  int d[] = {mmss / 1000, mmss / 100 % 10, 0, mmss / 10 % 10, mmss % 10};
  for (int i = 0; i < 5; i++) if (i != 2) disp.writeDigitNum(i, d[i]);  // מקום 2 = הנקודתיים
  disp.drawColon(true);
  disp.writeDisplay();
#else
  disp.showNumberDecEx(mmss, 0b01000000, true);  // 0b01000000 = הנקודתיים
#endif
}

void clearDisplay() {
  shown = -1;
  disp.clear();
#if USE_HT16K33
  disp.writeDisplay();
#endif
}
