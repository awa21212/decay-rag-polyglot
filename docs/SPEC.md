# Portable spec: time-aware hybrid retrieval

Everything in this repo implements **exactly** the algorithm below. The point is
bit-for-bit agreement across languages, not the best possible retrieval.

## 1. Corpus and chunking

- Corpus = every `*.md` directly inside `data/corpus/`, read as UTF-8.
- Split file text on a blank line (`\n\n`, after normalising CRLF to LF).
- Each paragraph group is one chunk, in file order, index starting at 0.
  Trailing empty groups are dropped.
- Chunk id = `<file name>#<index>`.

## 2. Chunk timestamp

Applied in order, first hit wins:

1. `YYYY-MM-DD` found in the **file name**.
2. `YYYY-MM-DD` found in the **chunk text** (first match).
3. The file's mtime (UTC seconds, truncated to whole days).

The value is interpreted as `00:00:00Z` on that date, stored as epoch seconds.

## 3. Reference "now"

Read `data/queries.txt`: the first line is `now YYYY-MM-DD`, every following
non-empty line is a query. Every language reads the same file, so decay is
deterministic and nobody has to agree on a JSON parser.

## 4. Text normalisation and bigrams

- Keep a character if it is ASCII alphanumeric, or its code point is in
  `U+4E00..U+9FFF` (CJK Unified Ideographs). Drop everything else.
- `bigrams(s)` = every adjacent pair of kept code points, as `(cp1, cp2)`.
  A string with fewer than 2 kept characters has no bigrams.
- A bigram is identified by its 64-bit hash (section 5); sets are hash sets.

## 5. Hashing (FNV-1a, 64-bit)

```
off = 14695981039346656037        # 0xcbf29ce484222325
prime = 1099511628211             # 0x100000001b3
h = off
for each of the 8 bytes b of (cp1 as u32 LE, cp2 as u32 LE):
    h = (h XOR b) * prime  (mod 2^64)
```

- bucket `i = h mod 4096`
- sign `s = -1 if (h >> 63) == 1 else +1`
- vector: `v[i] += s` over all bigrams, then L2-normalise (zero vector stays zero).

## 6. Similarity

```
cos = max(0, dot(q_vec, d_vec))          # both L2-normalised
lex = |q_bigrams ∩ d_bigrams| / max(1, |q_bigrams|)
sim = 0.72 * cos + 0.28 * lex
```

## 7. Time decay and final score

```
age_days = max(0, (now - ts) / 86400)
decay    = 0.5 ^ (age_days / HALF_LIFE)     # HALF_LIFE = 30
final    = 0.75 * sim + 0.25 * decay
```

## 8. Ranking and output

Sort by `final` descending; ties broken by chunk id ascending (byte order).
Take `k = 3`. Print JSON:

```json
{"query":"<q>","top":[{"chunk":"file#0","score":0.123456}, ...]}
```

Scores are rounded to 6 decimals (`%.6f`). All arithmetic is IEEE-754 double.