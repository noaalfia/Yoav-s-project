// בונה את web/models/asl-signs.json מתוך נקודות הידיים שחולצו מהתמונות.
// הרצה:  node tools/build-asl.js
//
// מקור התמונות: "American Sign Languages 50x50 images", Zenodo record 7563598, רישיון CC BY 4.0
// https://zenodo.org/records/7563598
// הנקודות חולצו עם tools/extract-landmarks.html (אותו מודל MediaPipe שהאתר משתמש בו).
//
// חשוב: handVec כאן חייב להיות זהה ל-handVec ב-web/app.js.

const fs = require("fs");
const path = require("path");

const RAW = path.join(__dirname, "asl-raw-landmarks.json");
const OUT = path.join(__dirname, "..", "web", "models", "asl-signs.json");
const PER_LETTER = 80;
const SKIP = new Set(["J", "Z"]); // אותיות עם תנועה — אי אפשר לזהות מתמונה קפואה

function handVec(pts, mirror) {
  const v = new Array(64).fill(0);
  const [w, m] = [pts[0], pts[9]];
  const s = Math.hypot(m[0] - w[0], m[1] - w[1], m[2] - w[2]) || 1;
  const sx = mirror ? -1 : 1;
  pts.forEach((p, j) => {
    v[j * 3] = (sx * (p[0] - w[0])) / s;
    v[j * 3 + 1] = (p[1] - w[1]) / s;
    v[j * 3 + 2] = (p[2] - w[2]) / s;
  });
  v[63] = 3;
  return v.map((x) => Math.round(x * 1000) / 1000);
}

const raw = JSON.parse(fs.readFileSync(RAW, "utf8"));
const signs = {};
for (const [letter, samples] of Object.entries(raw)) {
  if (SKIP.has(letter)) continue;
  signs[letter] = samples.slice(0, PER_LETTER).map((s) => handVec(s.pts, s.side === "Left"));
}
const out = {
  source: "Zenodo 7563598 (CC BY 4.0) — https://zenodo.org/records/7563598",
  oneHand: true, // כל וקטור הוא 64 מספרים של יד אחת; האתר משלים לאורך 128
  signs,
};
fs.writeFileSync(OUT, JSON.stringify(out));
console.log(`wrote ${OUT}: ${Object.keys(signs).length} letters, ${(fs.statSync(OUT).size / 1e6).toFixed(2)} MB`);
