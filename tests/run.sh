#!/usr/bin/env bash
# RunTime-Now test suite.
#
#   tests/run.sh                 # uses build/rtn
#   tests/run.sh path/to/rtn
#   tests/run.sh --update        # rewrite expected outputs (review the diff!)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RTN="$ROOT/build/rtn"
UPDATE=0
for arg in "$@"; do
  case "$arg" in
    --update) UPDATE=1 ;;
    *) RTN="$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")" ;;
  esac
done
[ -x "$RTN" ] || { echo "rtn binary not found: $RTN (build it first: cmake --build build)"; exit 1; }

PASS=0
FAIL=0
FAILED=()
ok()   { PASS=$((PASS + 1)); printf "  \033[32mok\033[0m    %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); FAILED+=("$1"); printf "  \033[31mFAIL\033[0m  %s\n" "$1"; [ -n "${2:-}" ] && printf "%s\n" "$2" | head -20 | sed 's/^/        /'; }
normalize() { sed -e "s#$ROOT#<root>#g"; }

# Compares output with an expected file (or writes it with --update).
expect() {  # name expected_file actual_output
  if [ "$UPDATE" = 1 ]; then printf "%s\n" "$3" > "$2"; ok "$1 (updated)"; return; fi
  if [ ! -f "$2" ]; then fail "$1" "missing $2 (run with --update)"; return; fi
  local d
  if d=$(diff <(printf "%s\n" "$3") "$2"); then ok "$1"; else fail "$1" "$d"; fi
}

echo "cases:"
cd "$ROOT/tests/cases"
for f in *.js *.ts; do
  name="${f%.*}"
  raw=$("$RTN" "$f" 2>&1 < /dev/null)
  code=$?
  out=$(printf "%s\n" "$raw" | normalize)
  want_code=0
  [ -f "$name.exit" ] && want_code=$(cat "$name.exit")
  if [ "$code" != "$want_code" ]; then fail "$f" "exit code $code, expected $want_code"; continue; fi
  expect "$f" "$name.out" "$out"
done

echo "strip:"
cd "$ROOT/tests/strip"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
for f in *.ts; do
  name="${f%.ts}"
  "$RTN" strip "$f" > "$TMP" 2>&1  # a file, not $(...): keeps trailing newlines
  if [ "$UPDATE" = 1 ]; then cp "$TMP" "$name.expected.js"; ok "$f (updated)"; continue; fi
  if [ "$(tr -dc '\n' < "$TMP" | wc -c)" != "$(tr -dc '\n' < "$f" | wc -c)" ]; then
    fail "$f" "line count changed"; continue
  fi
  if d=$(diff "$TMP" "$name.expected.js"); then ok "$f"; else fail "$f" "$d"; fi
done

echo "cli:"
cd "$ROOT"
[[ "$("$RTN" --version)" =~ ^rtn\ [0-9]+\.[0-9]+\.[0-9]+ ]] && ok "--version" || fail "--version"
[ "$("$RTN" -e 'console.log(process.argv.slice(1).join(","))' a b)" = "a,b" ] && ok "-e with args" || fail "-e with args"
[ "$(echo 'console.log(6 * 7)' | "$RTN")" = "42" ] && ok "script from piped stdin" || fail "script from piped stdin"
[ "$(echo 'console.log("dash")' | "$RTN" -)" = "dash" ] && ok "rtn - (stdin)" || fail "rtn - (stdin)"
"$RTN" /no/such/file.js > /dev/null 2>&1 && fail "missing file -> exit 1" || ok "missing file -> exit 1"
"$RTN" --bogus > /dev/null 2>&1 && fail "unknown option -> exit 1" || ok "unknown option -> exit 1"

echo "repl:"
out=$("$RTN" -i < "$ROOT/tests/repl.input" 2>&1 | normalize)
expect "repl session" "$ROOT/tests/repl.out" "$out"

if command -v python3 > /dev/null; then
  echo
  if python3 "$ROOT/tests/http_test.py" "$RTN"; then ok "http suite"; else fail "http suite"; fi
  if command -v curl > /dev/null && command -v tar > /dev/null && command -v sha256sum > /dev/null; then
    echo
    if bash "$ROOT/tests/upgrade_test.sh" "$RTN"; then ok "install/upgrade suite"; else fail "install/upgrade suite"; fi
  fi
else
  echo "(python3 not found: skipping HTTP and install/upgrade tests)"
fi

echo
if [ "$FAIL" = 0 ]; then
  printf "\033[32mAll %d tests passed.\033[0m\n" "$PASS"
else
  printf "\033[31m%d failed\033[0m, %d passed: %s\n" "$FAIL" "$PASS" "${FAILED[*]}"
  exit 1
fi
