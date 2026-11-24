// בונה את web/models/project-signs.json: הסט שנטען אוטומטית בכל דפדפן חדש (גם אצל המורה).
// הרצה:  node tools/build-project-set.js <bomb-signs.json>
//
// לוקח קובץ גיבוי ("שמירה לקובץ") ומנקה אותו:
//  - מוריד כפילויות של הסט המוכן (אם נטען כמה פעמים)
//  - מוריד דוגמאות שבהן נקלטו שתי ידיים
//  - שומר את ההקלטות האישיות רק לאותיות שבהן הן תואמות לאות ה-ASL (לפחות 70%)
//  - לכל האותיות: הסט המוכן של ASL פעם אחת

const fs = require("fs");
const path = require("path");
const src = process.argv[2];
if (!src) { console.error("usage: node tools/build-project-set.js <bomb-signs.json>"); process.exit(1); }

const backup = JSON.parse(fs.readFileSync(src, "utf8"));
const asl = require("../web/models/asl-signs.json").signs;
const OUT = path.join(__dirname, "..", "web", "models", "project-signs.json");
const AGREE = 0.7;

const r3 = (x) => Math.round(x * 1000) / 1000;
const key = (v) => v.slice(0, 64).map(r3).join(",");
const dist = (a, b) => { let s = 0; for (let i = 0; i < 64; i++) { const t = a[i] - b[i]; s += t * t; } return Math.sqrt(s); };

const pool = [];
for (const [L, vs] of Object.entries(asl)) for (const v of vs) pool.push({ L, v });
const aslKeys = new Set(pool.map((p) => key(p.v)));

function aslGuess(v) {
  const top = pool.map((p) => ({ L: p.L, d: dist(v, p.v) })).sort((a, b) => a.d - b.d).slice(0, 5);
  const votes = {};
  for (const t of top) votes[t.L] = (votes[t.L] || 0) + 1;
  return Object.entries(votes).sort((a, b) => b[1] - a[1])[0][0];
}

const signs = {};
const report = [];
for (const L of Object.keys(asl)) signs[L] = asl[L].map((v) => v.slice(0, 64));
for (const [L, vs] of Object.entries(backup.signs || {})) {
  const mine = vs.filter((v) => !aslKeys.has(key(v)) && v[127] !== 3);
  if (!mine.length) continue;
  const agree = mine.filter((v) => aslGuess(v) === L).length / mine.length;
  const keep = !asl[L] || agree >= AGREE; // סימן שאין לו גרסה ב-ASL — נשמר תמיד
  if (keep) signs[L] = (signs[L] || []).concat(mine.map((v) => v.slice(0, 64).map(r3)));
  report.push(`${L}: ${mine.length} yours, ${Math.round(agree * 100)}% match ASL -> ${keep ? "kept" : "dropped"}`);
}

const out = {
  oneHand: true,
  word: backup.word && backup.word.length ? backup.word : ["B", "O", "X"],
  threshold: backup.threshold || 2.5,
  timeLimit: backup.timeLimit || 60,
  signs,
};
fs.writeFileSync(OUT, JSON.stringify(out));
console.log(report.join("\n"));
console.log(`wrote ${OUT}: ${Object.keys(signs).length} signs, ${(fs.statSync(OUT).size / 1e6).toFixed(2)} MB`);
