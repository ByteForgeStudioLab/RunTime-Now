#!/usr/bin/env bash
# Tests install.sh and `rtn upgrade` against a fake GitHub-style release server.
# Everything happens in a temporary HOME, so your real shell config is untouched.
#
#   tests/upgrade_test.sh [path/to/rtn]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RTN="${1:-$ROOT/build/rtn}"
PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf "  ok    %s\n" "$1"; }
fail() { FAIL=$((FAIL + 1)); printf "  FAIL  %s\n" "$1"; [ -n "${2:-}" ] && printf "%s\n" "$2" | sed 's/^/        /'; }
check() { if eval "$2"; then ok "$1"; else fail "$1" "${3:-}"; fi; }

case "$(uname -m)" in x86_64|amd64) ARCH=x64 ;; aarch64|arm64) ARCH=arm64 ;; *) echo "skip: unsupported arch"; exit 0 ;; esac
NAME="rtn-linux-$ARCH"
CURRENT="$("$RTN" --version | awk '{print $2}')"

WORK="$(mktemp -d)"
SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2> /dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

# --- fake releases ------------------------------------------------------------
REL="$WORK/releases"
make_release() {  # version binary-file [corrupt]
  local dir="$REL/download/v$1"
  mkdir -p "$dir/stage/$NAME"
  cp "$2" "$dir/stage/$NAME/rtn"
  chmod 755 "$dir/stage/$NAME/rtn"
  tar -C "$dir/stage" -czf "$dir/$NAME.tar.gz" "$NAME"
  rm -rf "$dir/stage"
  (cd "$dir" && sha256sum "$NAME.tar.gz" > SHA256SUMS)
  if [ "${3:-}" = corrupt ]; then sed -i 's/^[0-9a-f]*/'"$(printf '0%.0s' $(seq 64))"'/' "$dir/SHA256SUMS"; fi
}
fake_binary() {  # version -> path of a tiny stand-in "rtn" that reports that version
  local f="$WORK/fake-$1"
  printf '#!/bin/sh\necho "rtn %s (QuickJS-ng test)"\n' "$1" > "$f"
  chmod 755 "$f"
  echo "$f"
}
make_release "$CURRENT" "$RTN"
make_release 9.9.9 "$(fake_binary 9.9.9)"
make_release 9.9.8 "$(fake_binary 9.9.8)" corrupt
mkdir -p "$REL/latest/download"
echo "9.9.9" > "$REL/latest/download/VERSION"

PORT=$((39000 + $$ % 1000))
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$REL" > /dev/null 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do curl -fs -o /dev/null "http://127.0.0.1:$PORT/latest/download/VERSION" && break; sleep 0.1; done

export HOME="$WORK/home" SHELL=/bin/bash RTN_RELEASES_URL="http://127.0.0.1:$PORT"
mkdir -p "$HOME"
BIN="$HOME/.rtn/bin/rtn"

echo "install.sh:"
out=$(bash "$ROOT/install.sh" "$CURRENT" 2>&1)
check "installs a pinned version to ~/.rtn/bin" '[ "$("$BIN" --version | awk "{print \$2}")" = "$CURRENT" ]' "$out"
check "adds rtn to PATH in ~/.bashrc" 'grep -q "RTN_INSTALL" "$HOME/.bashrc"'
bash "$ROOT/install.sh" "$CURRENT" > /dev/null 2>&1
check "running it twice doesn't duplicate the PATH lines" '[ "$(grep -c RTN_INSTALL= "$HOME/.bashrc")" = 1 ]'
check "the installed rtn really works" '[ "$("$BIN" -e "console.log(1 + 1)")" = 2 ]'

echo "rtn upgrade:"
out=$("$BIN" upgrade --check 2>&1)
check "--check reports the new version" '[[ "$out" == *"9.9.9"* ]]' "$out"
out=$("$BIN" upgrade --version 9.9.8 2>&1); code=$?
check "corrupted download is rejected (checksum)" '[ $code -ne 0 ] && [[ "$out" == *"checksum mismatch"* ]]' "$out"
check "...and the binary is left untouched" '[ "$("$BIN" --version | awk "{print \$2}")" = "$CURRENT" ]'
out=$("$BIN" upgrade --version 7.7.7 2>&1); code=$?
check "unknown version fails cleanly" '[ $code -ne 0 ] && [[ "$out" == *"download failed"* ]]' "$out"
out=$("$BIN" update -r 2>&1); code=$?
check "rtn update -r upgrades to the latest release" '[ $code -eq 0 ] && [ "$("$BIN" --version | awk "{print \$2}")" = 9.9.9 ]' "$out"
cp "$RTN" "$BIN"   # back to the real binary
out=$("$BIN" upgrade --version "$CURRENT" 2>&1)
check "same version -> nothing to do" '[[ "$out" == *"already on version"* ]]' "$out"
out=$("$ROOT/build/rtn" upgrade --version 9.9.9 2>&1); code=$?
check "refuses to overwrite a source build" '[ $code -ne 0 ] && [[ "$out" == *"built from source"* ]]' "$out"

echo "install.sh (latest / local):"
rm -rf "$HOME/.rtn"
bash "$ROOT/install.sh" > /dev/null 2>&1
check "no version -> latest release" '[ "$("$BIN" --version | awk "{print \$2}")" = 9.9.9 ]'
bash "$ROOT/install.sh" --binary "$RTN" > /dev/null 2>&1
check "--binary installs a local build" '[ "$("$BIN" --version | awk "{print \$2}")" = "$CURRENT" ]'
bash "$ROOT/install.sh" 7.7.7 > /dev/null 2>&1 && fail "missing version -> error" || ok "missing version -> error"

echo "upgrade: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
