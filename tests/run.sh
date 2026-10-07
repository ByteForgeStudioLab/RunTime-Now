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

shopt -s nullglob
echo "cases:"
cd "$ROOT/tests/cases"
for f in *.js *.ts *.mjs *.cjs; do
  name="${f%.*}"
  raw=$("$RTN" "$f" 2>&1 < /dev/null)
  code=$?
  out=$(printf "%s\n" "$raw" | normalize)
  want_code=0
  [ -f "$name.exit" ] && want_code=$(cat "$name.exit")
  if [ "$code" != "$want_code" ]; then fail "$f" "exit code $code, expected $want_code"; continue; fi
  expect "$f" "$name.out" "$out"
done

echo "packages:"
cd "$ROOT/tests/fixtures/project"  # node_modules, package.json exports/imports, CommonJS
for f in app.mjs main.cjs; do
  raw=$("$RTN" "$f" 2>&1 < /dev/null)
  code=$?
  out=$(printf "%s\n" "$raw" | normalize)
  if [ "$code" != 0 ]; then fail "$f" "exit code $code"$'\n'"$out"; continue; fi
  expect "project/$f" "$f.out" "$out"
done

echo "rtn test / init:"
cd "$ROOT/tests/fixtures/testrunner"
raw=$("$RTN" test 2>&1 < /dev/null)
code=$?
out=$(printf "%s\n" "$raw" | normalize | sed -E 's/\[[0-9.]+m?s\]/[time]/g; s/rtn test v[0-9.]+/rtn test vX/')
if [ "$code" != 1 ]; then fail "rtn test" "exit code $code, expected 1"; else expect "rtn test" "expected.out" "$out"; fi
INIT_DIR="$(mktemp -d)"
if "$RTN" init "$INIT_DIR/app" > /dev/null 2>&1 && (cd "$INIT_DIR/app" && "$RTN" index.ts | grep -q "Hello, world!" && "$RTN" test > /dev/null 2>&1); then
  ok "rtn init creates a project that runs and passes its tests"
else
  fail "rtn init creates a project that runs and passes its tests"
fi
if "$RTN" init "$INIT_DIR/js" --js > /dev/null 2>&1 && [ ! -e "$INIT_DIR/js/index.ts" ] && [ -f "$INIT_DIR/js/jsconfig.json" ] &&
  (cd "$INIT_DIR/js" && "$RTN" index.js | grep -q "Hello, world!" && "$RTN" test > /dev/null 2>&1); then
  ok "rtn init --js creates a JavaScript project that runs and passes its tests"
else
  fail "rtn init --js creates a JavaScript project that runs and passes its tests"
fi
rm -rf "$INIT_DIR"

echo ".env files:"
cd "$ROOT/tests/fixtures/dotenv"
out=$(env -u NODE_ENV KEEP=1 PLAIN=from-shell "$RTN" show.js 2>&1 < /dev/null)
want=$'PLAIN "from-shell"\nSINGLE "literal $PLAIN \\\\n"\nDOUBLE "line1\\nline2 from-shell"\nMULTI "a\\nb"\nURL "http://x/#not-comment"\nFROM_LOCAL "from-local"\nDEFAULTED "fallback"\nEXPAND "from-shell-!"\nESCAPED "$PLAIN"\nEMPTY ""\nMODE undefined\nKEEP "1"'
if [ "$out" = "$want" ]; then ok ".env, .env.local: quotes, comments, expansion; the environment wins"; else fail ".env, .env.local: quotes, comments, expansion; the environment wins" "$(diff <(printf "%s\n" "$out") <(printf "%s\n" "$want"))"; fi
if NODE_ENV=production "$RTN" show.js 2>&1 | grep -q 'MODE "prod"'; then ok ".env.\$NODE_ENV"; else fail ".env.\$NODE_ENV"; fi
if env -u NODE_ENV "$RTN" --env-file custom.env show.js 2>&1 | grep -qx 'MODE "from-file"' &&
  env -u NODE_ENV "$RTN" --env-file custom.env show.js 2>&1 | grep -qx 'PLAIN undefined'; then
  ok "--env-file replaces the default files"
else
  fail "--env-file replaces the default files"
fi
if "$RTN" --no-env-file show.js 2>&1 | grep -qx 'FROM_LOCAL undefined'; then ok "--no-env-file"; else fail "--no-env-file"; fi
if ! "$RTN" --env-file missing.env show.js > /dev/null 2>&1; then ok "--env-file with a missing file fails"; else fail "--env-file with a missing file fails"; fi

echo "--watch:"
WATCH_DIR="$(mktemp -d)"
wait_for() {  # pattern file: up to 10 s
  for _ in $(seq 100); do grep -q "$1" "$2" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
echo 'import { msg } from "./lib.ts"; console.log("value:", msg); setInterval(() => {}, 1000);' > "$WATCH_DIR/app.ts"
echo 'export const msg = "one";' > "$WATCH_DIR/lib.ts"
(cd "$WATCH_DIR" && exec "$RTN" --watch app.ts > out.log 2>&1 < /dev/null) &
WATCH_PID=$!
if wait_for "value: one" "$WATCH_DIR/out.log" && sleep 0.2 && echo 'export const msg = "two";' > "$WATCH_DIR/lib.ts" &&
  wait_for "value: two" "$WATCH_DIR/out.log" && grep -q "restarting · lib.ts changed" "$WATCH_DIR/out.log"; then
  ok "--watch restarts when an imported file changes"
else
  fail "--watch restarts when an imported file changes" "$(cat "$WATCH_DIR/out.log")"
fi
kill -TERM "$WATCH_PID" 2>/dev/null
wait "$WATCH_PID" 2>/dev/null
if pgrep -f "$RTN app.ts" > /dev/null 2>&1; then
  fail "--watch stops the program on SIGTERM"
  pkill -f "$RTN app.ts"
else
  ok "--watch stops the program on SIGTERM"
fi
rm -rf "$WATCH_DIR"

echo "rtn run (package.json scripts):"
cd "$ROOT/tests/fixtures/scripts"
check_run() {  # name expected_output expected_exit args...
  local name="$1" want="$2" want_code="$3"; shift 3
  local out code
  out=$("$RTN" run "$@" 2>&1 < /dev/null)
  code=$?
  if [ "$code" != "$want_code" ]; then fail "$name" "exit code $code, expected $want_code"$'\n'"$out"
  elif [ "$out" != "$want" ]; then fail "$name" "$(diff <(printf "%s\n" "$out") <(printf "%s\n" "$want"))"
  else ok "$name"; fi
}
check_run "rtn run: pre/post scripts and npm_package_* variables" \
  $'$ echo pre\npre\n$ echo hello from $npm_package_name@$npm_package_version\nhello from scripts-fixture@1.2.3\n$ echo post\npost' 0 hello
check_run "rtn run: arguments are passed on, quoted" $'$ printf \'[%s]\' \'a b\' \'it\'\\\'\'s\'\n[a b][it\'s]' 0 args -- "a b" "it's"
check_run "rtn run: node_modules/.bin is on PATH" $'$ fixture-tool\nfixture-tool ran' 0 tool
check_run "rtn run: rtn itself is on PATH" $'$ rtn -e \'console.log(1 + 1)\'\n2' 0 self
check_run "rtn run: the script's exit code" $'$ exit 7' 7 fail
check_run "rtn run: unknown script" $'error: Script not found "nope"\nAvailable: prehello, hello, posthello, args, tool, self, fail' 1 nope
if "$RTN" run 2>&1 < /dev/null | grep -q "fixture-tool"; then ok "rtn run: lists the scripts"; else fail "rtn run: lists the scripts"; fi

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
  echo
  if python3 "$ROOT/tests/fetch_test.py" "$RTN"; then ok "fetch suite"; else fail "fetch suite"; fi
  if command -v curl > /dev/null && command -v tar > /dev/null && command -v sha256sum > /dev/null; then
    echo
    if bash "$ROOT/tests/upgrade_test.sh" "$RTN"; then ok "install/upgrade suite"; else fail "install/upgrade suite"; fi
  fi
else
  echo "(python3 not found: skipping HTTP, fetch and install/upgrade tests)"
fi

echo
if [ "$FAIL" = 0 ]; then
  printf "\033[32mAll %d tests passed.\033[0m\n" "$PASS"
else
  printf "\033[31m%d failed\033[0m, %d passed: %s\n" "$FAIL" "$PASS" "${FAILED[*]}"
  exit 1
fi
