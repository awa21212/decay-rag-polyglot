# -*- coding: utf-8 -*-
"""Reference implementation of docs/SPEC.md (Python 3.10+)."""
import json
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, "data", "corpus")
QUERIES = os.path.join(ROOT, "data", "queries.txt")

DIM = 4096
HALF_LIFE = 30.0
DATE_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})")


def keep(cp):
    return (48 <= cp <= 57) or (65 <= cp <= 90) or (97 <= cp <= 122) or (0x4E00 <= cp <= 0x9FFF)


def bigrams(text):
    kept = [ord(ch) for ch in text if keep(ord(ch))]
    return [(kept[i], kept[i + 1]) for i in range(len(kept) - 1)]


def fnv1a(cp1, cp2):
    h = 14695981039346656037
    for b in struct.pack("<II", cp1, cp2):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def vector(grams):
    v = [0.0] * DIM
    for cp1, cp2 in grams:
        h = fnv1a(cp1, cp2)
        v[h % DIM] += -1.0 if (h >> 63) & 1 else 1.0
    norm = sum(x * x for x in v) ** 0.5
    if norm > 0:
        v = [x / norm for x in v]
    return v


def date_to_epoch(y, m, d):
    # days_from_civil (Hinnant), identical in every port
    y -= 1 if m <= 2 else 0
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    days = era * 146097 + doe - 719468
    return days * 86400


def parse_ts(name, text, mtime):
    m = DATE_RE.search(name)
    if m:
        return date_to_epoch(int(m.group(1)), int(m.group(2)), int(m.group(3)))
    m = DATE_RE.search(text)
    if m:
        return date_to_epoch(int(m.group(1)), int(m.group(2)), int(m.group(3)))
    return int(mtime // 86400 * 86400)


def chunks_of(path):
    raw = open(path, encoding="utf-8").read().replace("\r\n", "\n")
    parts = [p.strip() for p in raw.split("\n\n")]
    return [p for p in parts if p]


def read_queries():
    lines = [l.strip() for l in open(QUERIES, encoding="utf-8").read().splitlines()]
    lines = [l for l in lines if l]
    now = DATE_RE.search(lines[0]).group(0)
    y, m, d = (int(x) for x in now.split("-"))
    return date_to_epoch(y, m, d), lines[1:]


def build():
    docs = []
    for name in sorted(os.listdir(CORPUS)):
        if not name.endswith(".md"):
            continue
        path = os.path.join(CORPUS, name)
        mtime = os.path.getmtime(path)
        for i, text in enumerate(chunks_of(path)):
            grams = bigrams(text)
            docs.append({
                "id": "%s#%d" % (name, i),
                "ts": parse_ts(name, text, mtime),
                "grams": grams,
                "vec": vector(grams),
                "set": {fnv1a(a, b) for a, b in grams},
            })
    return docs


def search(docs, q, now, k=3):
    qg = bigrams(q)
    qset = {fnv1a(a, b) for a, b in qg}
    qv = vector(qg)
    out = []
    for d in docs:
        cos = max(0.0, sum(a * b for a, b in zip(qv, d["vec"])))
        lex = len(qset & d["set"]) / max(1, len(qset))
        sim = 0.72 * cos + 0.28 * lex
        age = max(0.0, (now - d["ts"]) / 86400.0)
        decay = 0.5 ** (age / HALF_LIFE)
        out.append((0.75 * sim + 0.25 * decay, d["id"]))
    out.sort(key=lambda t: (-t[0], t[1]))
    return out[:k]


def main():
    now, queries = read_queries()
    docs = build()
    for q in queries:
        parts = ['{"chunk":"%s","score":%.6f}' % (cid, s) for s, cid in search(docs, q, now)]
        print('{"query":"%s","top":[%s]}' % (q, ",".join(parts)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
