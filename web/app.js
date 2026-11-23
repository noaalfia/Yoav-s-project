// =====================================================================
//  נטרול הפצצה — זיהוי שפת סימנים במצלמה + שליחת פקודה לארדואינו
//
//  איך זה עובד:
//  1. MediaPipe מוצא בכל פריים 21 נקודות על כל יד.
//  2. הופכים את הנקודות ל"וקטור" מנורמל (לא תלוי במיקום/גודל היד בתמונה).
//  3. במסך האימון שומרים הרבה וקטורים לכל סימן.
//  4. בזמן המשחק משווים את היד לדוגמאות השמורות (KNN — השכנים הקרובים).
//  5. כשכל הסימנים של המילה בוצעו לפי הסדר — שולחים "D" לארדואינו, והמנוע נעצר.
// =====================================================================

import { FilesetResolver, HandLandmarker, DrawingUtils } from "./lib/vision_bundle.mjs";

// ---------- הגדרות קבועות ----------
const K = 5;                 // כמה שכנים קרובים בודקים
const MIN_VOTES = 4;         // כמה מתוך K צריכים להסכים
const HOLD_MS = 600;         // כמה זמן להחזיק סימן כדי שייחשב
const RELEASE_MS = 250;      // כמה זמן "הפסקה" צריך בין שני סימנים
const RECORD_MS = 3000;      // משך הקלטה באימון
const STORAGE_KEY = "bomb-signs-v1";

// ---------- מצב שמור ----------
const defaults = { signs: {}, asl: [], word: [], threshold: 1.0, timeLimit: 60, strict: false };
// asl = רשימת אותיות שהדוגמאות שלהן הגיעו מהסט המוכן (ולא מהקלטה שלך)
let db = loadDb();

function loadDb() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) return { ...defaults, ...JSON.parse(raw) };
  } catch (e) { /* אין גישה לאחסון — מתחילים ריק */ }
  return structuredClone(defaults);
}
function saveDb() {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(db)); }
  catch (e) { console.warn("לא הצלחתי לשמור בדפדפן", e); }
}

// ---------- אלמנטים ----------
const $ = (id) => document.getElementById(id);
const video = $("video"), canvas = $("overlay"), ctx = canvas.getContext("2d");

// =====================================================================
//  מצלמה + MediaPipe
// =====================================================================
let landmarker = null;
let drawer = null;

async function initVision() {
  // 1. קודם מצלמה — כדי שתמיד תראו תמונה, גם אם המודל נכשל
  try {
    $("camStatus").textContent = "מבקש גישה למצלמה… לחצו 'אפשר' בחלון שנפתח";
    const stream = await navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 480 } });
    video.srcObject = stream;
    await video.play();
  } catch (err) {
    console.error(err);
    $("camStatus").textContent = err.name === "NotAllowedError"
      ? "אין הרשאה למצלמה. לחצו על סמל המנעול/המצלמה בשורת הכתובת ← אפשר מצלמה, ורעננו."
      : err.name === "NotFoundError"
      ? "לא נמצאה מצלמה במחשב."
      : err.name === "NotReadableError"
      ? "המצלמה תפוסה על ידי תוכנה אחרת (Zoom, Teams, לשונית אחרת). סגרו אותה ורעננו."
      : "שגיאת מצלמה: " + err.message;
    return;
  }

  // 2. אחר כך המודל. קודם GPU, ואם נכשל — מעבד רגיל
  $("camStatus").textContent = "המצלמה פועלת. טוען מודל זיהוי ידיים…";
  try {
    const files = await FilesetResolver.forVisionTasks("./lib/wasm");
    const make = (delegate) => HandLandmarker.createFromOptions(files, {
      baseOptions: { modelAssetPath: "./models/hand_landmarker.task", delegate },
      runningMode: "VIDEO",
      numHands: 2,
    });
    try { landmarker = await make("GPU"); }
    catch (e) { console.warn("GPU נכשל, עובר ל-CPU", e); landmarker = await make("CPU"); }
    drawer = new DrawingUtils(ctx);
    $("camStatus").textContent = "המצלמה והזיהוי פועלים ✔";
    requestAnimationFrame(loop);
  } catch (err) {
    console.error(err);
    $("camStatus").textContent = "המצלמה פועלת, אבל המודל לא נטען: " + err.message;
  }
}

// הופך את נקודות הידיים לוקטור מספרים קבוע באורך 128.
// יד אחת: תמיד במקום 0, ואם זו יד שמאל — הופכים אותה (מראה) כדי שתיראה כמו יד ימין.
//         ככה אפשר לסמן בכל יד, וגם הסט המוכן של ASL מתאים לשתי הידיים.
// שתי ידיים: מקום 0 = שמאל, מקום 1 = ימין.
// (אם משנים כאן משהו — צריך לייצר מחדש את models/asl-signs.json, ראו tools/build-asl.js)
function handVec(vec, off, pts, mirror) {
  const wrist = pts[0];
  const mid = pts[9]; // בסיס האצבע האמצעית — משמש כ"סרגל" לגודל היד
  const scale = Math.hypot(mid.x - wrist.x, mid.y - wrist.y, mid.z - wrist.z) || 1;
  const sx = mirror ? -1 : 1;
  pts.forEach((p, j) => {
    vec[off + j * 3]     = sx * (p.x - wrist.x) / scale;
    vec[off + j * 3 + 1] = (p.y - wrist.y) / scale;
    vec[off + j * 3 + 2] = (p.z - wrist.z) / scale;
  });
  vec[off + 63] = 3; // "דגל" שהיד הזאת קיימת — מבדיל בין סימן ביד אחת לשתי ידיים
}

function toFeatures(result) {
  const hands = result.landmarks || [];
  if (hands.length === 0) return null;
  const sideOf = (i) => result.handedness?.[i]?.[0]?.categoryName || "Right";
  const vec = new Array(2 * 64).fill(0);
  if (hands.length === 1) {
    handVec(vec, 0, hands[0], sideOf(0) === "Left");
  } else {
    hands.slice(0, 2).forEach((pts, i) => handVec(vec, sideOf(i) === "Right" ? 64 : 0, pts, false));
  }
  return vec;
}

function dist(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) { const d = a[i] - b[i]; s += d * d; }
  return Math.sqrt(s);
}

// KNN: מחזיר את הסימן הכי סביר, כמה שכנים הסכימו, ומה המרחק הממוצע
function classify(vec) {
  const all = [];
  for (const [name, samples] of Object.entries(db.signs)) {
    for (const s of samples) all.push({ name, d: dist(vec, s) });
  }
  if (all.length < K) return null;
  all.sort((a, b) => a.d - b.d);
  const top = all.slice(0, K);
  const votes = {};
  for (const t of top) votes[t.name] = (votes[t.name] || 0) + 1;
  const [name, count] = Object.entries(votes).sort((a, b) => b[1] - a[1])[0];
  const winners = top.filter((t) => t.name === name);
  const meanD = winners.reduce((s, t) => s + t.d, 0) / winners.length;
  const ok = count >= MIN_VOTES && meanD <= db.threshold;
  return { name, votes: count, dist: meanD, ok };
}

// =====================================================================
//  לולאה ראשית — רצה על כל פריים
// =====================================================================
let lastVideoTime = -1;
let recording = null; // { name, until, samples: [] }

function loop() {
  if (video.currentTime !== lastVideoTime && video.readyState >= 2) {
    lastVideoTime = video.currentTime;
    const result = landmarker.detectForVideo(video, performance.now());
    draw(result);
    const vec = toFeatures(result);

    if (recording) handleRecording(vec);

    let guess = null;
    if (vec) guess = classify(vec);
    showLive(guess);
    game.onFrame(guess && guess.ok ? guess.name : null);
  }
  requestAnimationFrame(loop);
}

function draw(result) {
  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  for (const pts of result.landmarks || []) {
    drawer.drawConnectors(pts, HandLandmarker.HAND_CONNECTIONS, { color: "#4d9fff", lineWidth: 4 });
    drawer.drawLandmarks(pts, { color: "#ffcc4d", radius: 4 });
  }
}

function showLive(g) {
  $("liveLabel").textContent = g ? (g.ok ? g.name : `(${g.name}?)`) : "—";
  $("liveConf").textContent = g ? `${g.votes}/${K}` : "—";
  $("liveDist").textContent = g ? g.dist.toFixed(2) : "—";
}

// =====================================================================
//  אימון
// =====================================================================
function startRecording() {
  const name = $("signName").value.trim();
  if (!name) { $("signName").focus(); return; }
  if (!landmarker) return;
  $("recordBtn").disabled = true;
  let n = 3;
  const big = $("bigCountdown");
  big.textContent = n;
  const t = setInterval(() => {
    n--;
    if (n > 0) { big.textContent = n; return; }
    clearInterval(t);
    big.textContent = "🔴";
    recording = { name, until: performance.now() + RECORD_MS, samples: [] };
  }, 700);
}

function handleRecording(vec) {
  if (vec) recording.samples.push(vec);
  if (performance.now() < recording.until) return;
  const { name, samples } = recording;
  recording = null;
  $("bigCountdown").textContent = "";
  $("recordBtn").disabled = false;
  if (samples.length === 0) { alert("לא זוהתה יד בזמן ההקלטה. נסו שוב."); return; }
  if (db.asl.includes(name)) {
    // ההקלטה שלך מחליפה את הדוגמאות המוכנות של האות הזאת
    db.signs[name] = samples;
    db.asl = db.asl.filter((x) => x !== name);
  } else {
    db.signs[name] = (db.signs[name] || []).concat(samples);
  }
  saveDb();
  renderSignList();
  validateWord();
}

function renderSignList() {
  const ul = $("signList");
  ul.innerHTML = "";
  const names = Object.keys(db.signs);
  if (names.length === 0) {
    ul.innerHTML = '<li><span>עוד לא הוקלטו סימנים</span></li>';
    return;
  }
  for (const name of names) {
    const li = document.createElement("li");
    li.innerHTML = `<b></b><span>${db.signs[name].length} דוגמאות · ${db.asl.includes(name) ? "מוכן (ASL)" : "ההקלטה שלך ✔"}</span>`;
    li.querySelector("b").textContent = name;
    const del = document.createElement("button");
    del.textContent = "מחק";
    del.onclick = () => {
      if (!confirm(`למחוק את הסימן "${name}"?`)) return;
      delete db.signs[name];
      saveDb(); renderSignList(); validateWord();
    };
    li.append(del);
    ul.append(li);
  }
}

// מתקן את המילה: "a" → "A" אם קיים סימן A, ו-"BOX" בלי פסיקים → B,O,X
function fixWord(tokens) {
  const keys = Object.keys(db.signs);
  const find = (t) => db.signs[t] ? t : keys.find((k) => k.toLowerCase() === t.toLowerCase());
  const out = [];
  for (const t of tokens) {
    if (find(t)) { out.push(find(t)); continue; }
    const chars = [...t.replace(/\s+/g, "")];
    if (chars.length > 1 && chars.every((c) => find(c))) out.push(...chars.map(find));
    else out.push(t);
  }
  return out;
}

function validateWord() {
  db.word = fixWord(db.word);
  saveDb();
  const missing = db.word.filter((w) => !db.signs[w]);
  $("wordError").textContent = db.word.length === 0
    ? "עוד לא הוגדרה מילה"
    : missing.length ? `סימנים שלא הוקלטו: ${missing.join(", ")}` : "";
  game.reset();
}

// =====================================================================
//  המשחק
// =====================================================================
const game = {
  state: "idle",      // idle | running | won | lost
  index: 0,
  endsAt: 0,
  remoteProgress: null, // אחוז התקדמות שמגיע מהארדואינו (אם מחובר)
  candidate: null, candidateSince: 0,
  released: true, releaseSince: 0, lastAccepted: null,

  reset() {
    this.state = "idle";
    this.index = 0;
    this.remoteProgress = null;
    this.candidate = null;
    this.released = true;
    this.lastAccepted = null;
    setMsg("", "");
    renderWord();
    updateTimer();
  },

  start() {
    if (db.word.length === 0) {
      setMsg("עוד לא הוגדרה מילה. כתבו אותה במסך האימון, בסעיף 2", "lose");
      return;
    }
    const missing = db.word.filter((w) => !db.signs[w]);
    if (missing.length) {
      setMsg(Object.keys(db.signs).length === 0
        ? "האתר עוד לא מכיר אף סימן. במסך האימון לחצו \"טעינת אותיות ASL\" או הקליטו סימנים"
        : `האתר לא מכיר את: ${missing.join(", ")}. הקליטו אותו במסך האימון או טענו את אותיות ASL`, "lose");
      return;
    }
    this.reset();
    this.state = "running";
    this.endsAt = performance.now() + db.timeLimit * 1000;
    serial.send("S"); // אם הארדואינו מחובר — מתחיל להזיז את המחט
  },

  onFrame(label) {
    if (this.state === "running" && !serial.connected && performance.now() >= this.endsAt) {
      this.lose();
    }
    updateTimer();
    if (this.state !== "running") return;
    const now = performance.now();

    // צריך "לשחרר" (להוריד יד / לשנות סימן) בין סימן לסימן
    if (!this.released) {
      if (label !== this.lastAccepted) {
        if (!this.releaseSince) this.releaseSince = now;
        if (now - this.releaseSince >= RELEASE_MS) this.released = true;
      } else {
        this.releaseSince = 0;
      }
      return;
    }

    if (label !== this.candidate) {
      this.candidate = label;
      this.candidateSince = now;
      return;
    }
    if (!label || now - this.candidateSince < HOLD_MS) return;

    // סימן הוחזק מספיק זמן
    this.lastAccepted = label;
    this.released = false;
    this.releaseSince = 0;
    this.candidate = null;

    if (label === db.word[this.index]) {
      this.index++;
      flash("good");
      renderWord();
      if (this.index >= db.word.length) this.win();
    } else {
      flash("bad");
      if (db.strict) { this.index = 0; renderWord(); setMsg("טעות! מתחילים מההתחלה", "lose"); }
    }
  },

  win() {
    this.state = "won";
    serial.send("D");
    setMsg("✔ הפצצה נוטרלה!", "win");
  },

  lose() {
    if (this.state === "lost") return;
    this.state = "lost";
    setMsg("💥 בום! הזמן נגמר", "lose");
  },
};

function renderWord() {
  const view = $("wordView");
  view.innerHTML = "";
  db.word.forEach((w, i) => {
    const d = document.createElement("div");
    d.className = "slot" + (i < game.index ? " done" : i === game.index && game.state === "running" ? " current" : "");
    d.textContent = i < game.index || game.state !== "running" ? w : "?";
    view.append(d);
  });
}

function updateTimer() {
  let frac = 0, text = "";
  if (serial.connected && game.remoteProgress !== null) {
    frac = game.remoteProgress / 100;
    text = `מחט: ${game.remoteProgress}%`;
  } else if (game.state === "running") {
    const left = Math.max(0, game.endsAt - performance.now());
    frac = 1 - left / (db.timeLimit * 1000);
    text = (left / 1000).toFixed(1) + " שנ'";
  }
  $("timerFill").style.width = (frac * 100).toFixed(1) + "%";
  $("timerText").textContent = text;
}

function setMsg(text, cls) {
  const el = $("gameMsg");
  el.textContent = text;
  el.className = "game-msg " + cls;
}

function flash(kind) {
  const f = $("flash");
  f.className = "flash " + kind;
  setTimeout(() => (f.className = "flash"), 350);
}

// =====================================================================
//  חיבור לארדואינו (Web Serial — עובד בכרום / אדג')
//  פרוטוקול:  אתר → ארדואינו:  S=התחל  D=נטרול  R=איפוס
//             ארדואינו → אתר:  ARMED, P:<אחוז>, BOOM, DEFUSED, IDLE
// =====================================================================
const serial = {
  port: null, writer: null, connected: false,

  async connect() {
    if (!("serial" in navigator)) {
      alert("הדפדפן לא תומך בחיבור לארדואינו. השתמשו ב-Chrome או Edge.");
      return;
    }
    try {
      this.port = await navigator.serial.requestPort();
      await this.port.open({ baudRate: 9600 });
      const enc = new TextEncoderStream();
      enc.readable.pipeTo(this.port.writable);
      this.writer = enc.writable.getWriter();
      this.connected = true;
      $("serialStatus").textContent = "ארדואינו: מחובר";
      $("serialStatus").className = "pill on";
      this.readLoop();
    } catch (e) {
      console.error(e);
    }
  },

  async readLoop() {
    const dec = new TextDecoderStream();
    this.port.readable.pipeTo(dec.writable);
    const reader = dec.readable.getReader();
    let buf = "";
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += value;
        let nl;
        while ((nl = buf.indexOf("\n")) >= 0) {
          this.onLine(buf.slice(0, nl).trim());
          buf = buf.slice(nl + 1);
        }
      }
    } catch (e) { console.error(e); }
    this.connected = false;
    $("serialStatus").textContent = "ארדואינו: התנתק";
    $("serialStatus").className = "pill off";
  },

  onLine(line) {
    console.log("arduino:", line);
    if (line === "ARMED" && game.state !== "running") {
      // החיישן האולטרסוני זיהה שחקן — המשחק מתחיל אוטומטית
      game.reset();
      game.state = "running";
      game.endsAt = Infinity;
      game.remoteProgress = 0;
    } else if (line.startsWith("P:")) {
      game.remoteProgress = parseInt(line.slice(2), 10) || 0;
    } else if (line === "BOOM") {
      game.lose();
    } else if (line === "IDLE") {
      game.reset();
    }
  },

  send(cmd) {
    if (this.writer) this.writer.write(cmd + "\n");
  },
};

// =====================================================================
//  כפתורים
// =====================================================================
document.querySelectorAll(".tab").forEach((btn) => {
  btn.onclick = () => {
    document.querySelectorAll(".tab").forEach((b) => b.classList.toggle("active", b === btn));
    $("tab-game").classList.toggle("hidden", btn.dataset.tab !== "game");
    $("tab-train").classList.toggle("hidden", btn.dataset.tab !== "train");
  };
});

// טעינת הסט המוכן של ASL (נוסף לסימנים שכבר הוקלטו, לא מוחק אותם)
$("loadAslBtn").onclick = async () => {
  $("aslStatus").textContent = "טוען…";
  try {
    const pack = await (await fetch("./models/asl-signs.json")).json();
    for (const [letter, vecs] of Object.entries(pack.signs)) {
      // אות שהקלטת בעצמך לא נדרסת. אות מוכנה שנטענה קודם מתחלפת בגרסה המעודכנת
      if (db.signs[letter] && !db.asl.includes(letter)) continue;
      db.signs[letter] = vecs.map((v) => (pack.oneHand ? v.concat(new Array(64).fill(0)) : v));
      if (!db.asl.includes(letter)) db.asl.push(letter);
    }
    if (db.word.length === 0) {
      db.word = ["B", "O", "X"];
      $("wordInput").value = db.word.join(",");
    }
    if (db.threshold < 2.5) {
      db.threshold = 2.5; // יד של אדם אחר רחוקה יותר מהדוגמאות — רף סלחני יותר
      $("threshold").value = db.threshold;
      $("thresholdVal").textContent = db.threshold;
    }
    saveDb(); renderSignList(); validateWord();
    $("aslStatus").textContent = `נטענו ${Object.keys(pack.signs).length} אותיות ✔`;
  } catch (e) {
    console.error(e);
    $("aslStatus").textContent = "הטעינה נכשלה. ודאו שהאתר רץ דרך שרת ולא מקובץ.";
  }
};

$("connectBtn").onclick = () => serial.connect();
$("startBtn").onclick = () => game.start();
$("resetBtn").onclick = () => { serial.send("R"); game.reset(); };
$("recordBtn").onclick = startRecording;

$("wordInput").value = db.word.join(",");
$("wordInput").oninput = () => {
  db.word = $("wordInput").value.split(",").map((s) => s.trim()).filter(Boolean);
  saveDb(); validateWord();
};

$("timeLimit").value = db.timeLimit;
$("timeLimit").onchange = () => { db.timeLimit = Math.max(10, +$("timeLimit").value || 60); saveDb(); };

$("threshold").value = db.threshold;
$("thresholdVal").textContent = db.threshold;
$("threshold").oninput = () => {
  db.threshold = +$("threshold").value;
  $("thresholdVal").textContent = db.threshold;
  saveDb();
};

$("strictMode").checked = db.strict;
$("strictMode").onchange = () => { db.strict = $("strictMode").checked; saveDb(); };

$("exportBtn").onclick = () => {
  const blob = new Blob([JSON.stringify(db)], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "bomb-signs.json";
  a.click();
};
$("importInput").onchange = async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  try {
    db = { ...defaults, ...JSON.parse(await file.text()) };
    saveDb();
    location.reload();
  } catch { alert("הקובץ לא תקין"); }
};
$("clearBtn").onclick = () => {
  if (!confirm("למחוק את כל הסימנים וההגדרות?")) return;
  db = structuredClone(defaults);
  saveDb();
  location.reload();
};

renderSignList();
validateWord();
initVision();
