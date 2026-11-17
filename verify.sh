#!/usr/bin/env bash
# Build and compare every port against the Python reference.
set -u
cd "$(dirname "$0")"
mkdir -p out
PY=${PYTHON:-python3}

run() {           # name, command...
  local name=$1; shift
  if "$@" > "out/$name.txt" 2> "out/$name.err"; then
    if diff -q py/expected.txt "out/$name.txt" > /dev/null; then
      printf '%-8s ok   %s lines\n' "$name" "$(wc -l < "out/$name.txt")"
      return 0
    fi
    printf '%-8s DIFF %s\n' "$name" "$(diff py/expected.txt "out/$name.txt" | head -4)"
    return 1
  fi
  printf '%-8s FAIL see out/%s.err\n' "$name" "$name"
  return 1
}

skip() { printf '%-8s skip (%s)\n' "$1" "$2"; }

have() { command -v "$1" > /dev/null 2>&1; }

echo "== reference =="
run python "$PY" py/ref.py || exit 1
cp py/expected.txt py/expected.txt 2>/dev/null || true

echo "== ports =="
have node && run node node js/decay.mjs || skip node "no node"
have gcc  && { gcc -O2 -w -o out/decay_c c/decay.c -lm && run c out/decay_c; } || skip c "no gcc"
have g++  && { g++ -std=c++17 -O2 -w -o out/decay_cpp cpp/decay.cpp && run cpp out/decay_cpp; } || skip cpp "no g++"
if have go; then
  (cd go && go run . ../data > /dev/null 2>&1) # warm module cache if present
  go run go/main.go > out/go.txt 2> out/go.err && { diff -q py/expected.txt out/go.txt > /dev/null && echo "go       ok   $(wc -l < out/go.txt) lines" || { echo "go       DIFF"; diff py/expected.txt out/go.txt | head -4; }; } || echo "go       FAIL see out/go.err"
else skip go "no go"; fi
if have javac; then
  javac -d out/classes java/DecayRag.java 2> out/java.err || { echo "java     FAIL see out/java.err"; }
  LANG=${LANG:-C.UTF-8} java -Dfile.encoding=UTF-8 -Dsun.jnu.encoding=UTF-8 -cp out/classes DecayRag > out/java.txt 2>> out/java.err \
    && { diff -q py/expected.txt out/java.txt > /dev/null && echo "java     ok   $(wc -l < out/java.txt) lines" || { echo "java     DIFF"; diff py/expected.txt out/java.txt | head -4; }; }
else skip java "no javac"; fi

echo "== digest =="
cat out/*.txt 2>/dev/null | grep -v '^$' | sort -u | sha256sum 2>/dev/null || true
