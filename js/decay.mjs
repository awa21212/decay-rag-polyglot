// decay-rag portable spec — Node.js port (ESM, Node 18+)
import fs from "node:fs";
import path from "node:path";

const DIM = 4096;
const HALF_LIFE = 30.0;

const keepCp = (cp) =>
  (cp >= 48 && cp <= 57) ||
  (cp >= 65 && cp <= 90) ||
  (cp >= 97 && cp <= 122) ||
  (cp >= 0x4e00 && cp <= 0x9fff);

function fnv1a(a, b) {
  let h = 0xcbf29ce484222325n;
  const prime = 0x100000001b3n;
  const mask = 0xffffffffffffffffn;
  const bytes = [];
  for (const v of [a, b]) {
    for (let i = 0; i < 4; i++) bytes.push(Number((v >>> (8 * i)) & 0xff));
  }
  for (const b8 of bytes) {
    h ^= BigInt(b8);
    h = (h * prime) & mask;
  }
  return h;
}

function build(text) {
  const cps = [];
  for (const ch of text) {
    const cp = ch.codePointAt(0);
    if (keepCp(cp)) cps.push(cp);
  }
  const vec = new Float64Array(DIM);
  const grams = [];
  for (let i = 0; i + 1 < cps.length; i++) {
    const h = fnv1a(cps[i], cps[i + 1]);
    grams.push(h);
    const bucket = Number(h % 4096n);
    vec[bucket] += (h >> 63n) & 1n ? -1 : 1;
  }
  let norm = 0;
  for (let i = 0; i < DIM; i++) norm += vec[i] * vec[i];
  norm = Math.sqrt(norm);
  if (norm > 0) for (let i = 0; i < DIM; i++) vec[i] /= norm;
  grams.sort((x, y) => (x < y ? -1 : x > y ? 1 : 0));
  return { vec, grams };
}

function daysFromCivil(y, m, d) {
  y -= m <= 2 ? 1 : 0;
  const era = Math.floor((y >= 0 ? y : y - 399) / 400);
  const yoe = y - era * 400;
  const doy = Math.floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
  const doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
  return era * 146097 + doe - 719468;
}

function parseDate(s) {
  for (let i = 0; i + 10 <= s.length; i++) {
    const seg = s.slice(i, i + 10);
    if (!/^\d{4}-\d{2}-\d{2}$/.test(seg)) continue;
    const [y, m, d] = seg.split("-").map(Number);
    return daysFromCivil(y, m, d) * 86400;
  }
  return -1;
}

const root = process.argv[2] ?? ".";
const qtext = fs.readFileSync(path.join(root, "data", "queries.txt"), "utf8");
const lines = qtext.split(/\r?\n/);
const now = parseDate(lines[0]);
const queries = lines.slice(1).map((l) => l.trim()).filter(Boolean);

const corpusDir = path.join(root, "data", "corpus");
const files = fs
  .readdirSync(corpusDir)
  .filter((n) => n.endsWith(".md"))
  .sort();

const chunks = [];
for (const name of files) {
  const full = path.join(corpusDir, name);
  const raw = fs.readFileSync(full, "utf8").replace(/\r\n/g, "\n");
  const mtime = Math.floor(fs.statSync(full).mtimeMs / 1000);
  const mts = Math.floor(mtime / 86400) * 86400;
  let idx = 0;
  for (const piece of raw.split("\n\n")) {
    const text = piece.trim().replace(/^[ \t\r\n]+|[ \t\r\n]+$/g, "");
    if (!text) continue;
    let ts = parseDate(name);
    if (ts < 0) ts = parseDate(text);
    if (ts < 0) ts = mts;
    const { vec, grams } = build(text);
    chunks.push({ id: `${name}#${idx++}`, ts, vec, grams });
  }
}

for (const q of queries) {
  const { vec: qv, grams: qg } = build(q);
  const scored = chunks.map((c) => {
    let cos = 0;
    for (let k = 0; k < DIM; k++) cos += qv[k] * c.vec[k];
    if (cos < 0) cos = 0;
    let i = 0;
    let j = 0;
    let n = 0;
    while (i < qg.length && j < c.grams.length) {
      if (qg[i] === c.grams[j]) {
        n++;
        i++;
        j++;
      } else if (qg[i] < c.grams[j]) i++;
      else j++;
    }
    const lex = n / Math.max(1, qg.length);
    const sim = 0.72 * cos + 0.28 * lex;
    const age = Math.max(0, (now - c.ts) / 86400);
    const decay = Math.pow(0.5, age / HALF_LIFE);
    return { id: c.id, score: 0.75 * sim + 0.25 * decay };
  });
  scored.sort((a, b) => (b.score - a.score) || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const top = scored.slice(0, 3).map((s) => `{"chunk":"${s.id}","score":${s.score.toFixed(6)}}`);
  console.log(`{"query":"${q}","top":[${top.join(",")}]}`);
}
