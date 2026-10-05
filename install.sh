#!/usr/bin/env bash
# RunTime-Now (rtn) installer
#
#   curl -fsSL https://byteforgestudiolab.github.io/RunTime-Now/install | bash
#   (same file: https://raw.githubusercontent.com/ByteForgeStudioLab/RunTime-Now/main/install.sh)
#
#   ... | bash -s v1.5.0            install a specific version
#   ./install.sh --binary build/rtn  install a binary you built yourself
#
# Environment:
#   RTN_INSTALL       install directory            (default: ~/.rtn)
#   RTN_RELEASES_URL  where releases are downloaded (default: GitHub releases)
set -euo pipefail

REPO="${RTN_REPO:-ByteForgeStudioLab/RunTime-Now}"
RELEASES="${RTN_RELEASES_URL:-https://github.com/$REPO/releases}"
RELEASES="${RELEASES%/}"
INSTALL_DIR="${RTN_INSTALL:-$HOME/.rtn}"
BIN_DIR="$INSTALL_DIR/bin"

# Colors: the terminal's own text color plus one blue accent. FANCY = animate
# (a real terminal; not in CI, not with NO_COLOR or TERM=dumb).
FANCY=0
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-}" != dumb ]; then
  BOLD=$'\033[1m' DIM=$'\033[2m' RED=$'\033[31m' RESET=$'\033[0m'
  case "${COLORTERM:-}" in
    *truecolor*|*24bit*) BLUE=$'\033[38;2;59;130;246m' ;;
    *) BLUE=$'\033[38;5;33m' ;;
  esac
  [ -z "${CI:-}" ] && FANCY=1
else
  BOLD='' DIM='' RED='' BLUE='' RESET=''
fi
info()  { printf '%s\n' "$*"; }
step() {  # label detail
  if [ "$FANCY" = 1 ]; then printf '\r\033[2K  %s✓%s %-20s%s%s%s\n' "$BLUE" "$RESET" "$1" "$DIM" "$2" "$RESET"
  else printf '  %s%s%s %s\n' "$DIM" "$1" "$RESET" "$2"; fi
}
error() {
  [ "$FANCY" = 1 ] && printf '\r\033[2K\033[?25h'
  printf '%serror%s: %s\n' "$RED" "$RESET" "$*" >&2; exit 1
}

usage() {
  cat <<EOF
Install RunTime-Now (rtn).

Usage: install.sh [version] [options]

  version              e.g. 1.5.0 or v1.5.0 (default: latest release)
  --binary <file>      install a local rtn binary instead of downloading one
  --no-modify-path     don't add rtn to PATH in your shell config
  -h, --help           show this help
EOF
}

VERSION=""
LOCAL_BINARY=""
MODIFY_PATH=1
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --binary) [ $# -ge 2 ] || error "--binary needs a file"; LOCAL_BINARY="$2"; shift ;;
    --no-modify-path) MODIFY_PATH=0 ;;
    v[0-9]*|[0-9]*) VERSION="${1#v}" ;;
    *) error "unknown argument: $1 (see --help)" ;;
  esac
  shift
done

# --- platform ---------------------------------------------------------------
case "$(uname -s)" in
  Linux) os=linux ;;
  Darwin) error "macOS is not supported yet — build from source: https://github.com/$REPO" ;;
  *) error "unsupported operating system: $(uname -s)" ;;
esac
case "$(uname -m)" in
  x86_64|amd64) arch=x64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) error "unsupported CPU architecture: $(uname -m)" ;;
esac
TARGET="rtn-$os-$arch"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"; [ "$FANCY" = 1 ] && printf "\033[?25h"' EXIT
trap 'exit 130' INT TERM

download() {  # url file
  if command -v curl > /dev/null; then
    curl -fsSL --retry 2 -o "$2" "$1"
  elif command -v wget > /dev/null; then
    wget -q -O "$2" "$1"
  else
    error "curl or wget is required"
  fi
}

# "1.7 MB" from a byte count (integer math only).
human() {
  if [ "$1" -lt 1048576 ]; then printf '%d KB' $(( $1 / 1024 ))
  else printf '%d.%d MB' $(( $1 / 1048576 )) $(( $1 % 1048576 * 10 / 1048576 )); fi
}

# Like download(), but on a terminal shows a live progress bar while curl runs.
download_with_progress() {  # url file label
  if [ "$FANCY" != 1 ] || ! command -v curl > /dev/null; then download "$1" "$2"; return; fi
  curl -fsSL --retry 2 --connect-timeout 15 -D "$2.headers" -o "$2" "$1" 2> /dev/null &
  local pid=$! frame=0 got total width=30 filled pct bar i shown=0
  local frames=(⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏)
  printf '\033[?25l'
  draw() {  # spinner-or-check
    got=$(stat -c %s "$2" 2> /dev/null || echo 0)
    total=$(awk 'toupper($1) ~ /^HTTP\// { t = 0 } tolower($1) == "content-length:" { t = $2 + 0 } END { print t + 0 }' "$2.headers" 2> /dev/null || echo 0)
    if [ "$total" -gt 0 ]; then
      pct=$(( got * 100 / total )); [ "$pct" -gt 100 ] && pct=100
      [ "$1" = done ] && pct=100
      shown=$(( shown + (pct - shown + 2) / 3 )); [ "$1" = done ] && shown=100   # glide toward the real value
      filled=$(( shown * width / 100 ))
      bar="$BLUE"; for ((i = 0; i < filled; i++)); do bar+="━"; done
      bar+="$RESET$DIM"; for ((i = filled; i < width; i++)); do bar+="━"; done; bar+="$RESET"
      local stats; stats="$(printf '%s%3d%%%s  %s %s/ %s%s' "$BOLD" "$shown" "$RESET" "$(human $(( total * shown / 100 )))" "$DIM" "$(human "$total")" "$RESET")"
    else
      bar="$DIM"; for ((i = 0; i < width; i++)); do bar+="━"; done; bar+="$RESET"
      local stats; stats="$(human "$got")"
    fi
    local icon="${BLUE}${frames[frame % 10]}${RESET}"
    [ "$1" = done ] && icon="${BLUE}✓${RESET}"
    printf '\r\033[2K  %s %-20s%s  %s' "$icon" "$3" "$bar" "$stats"
  }
  while kill -0 "$pid" 2> /dev/null; do
    draw run "$2" "$3"
    frame=$(( frame + 1 ))
    sleep 0.06
  done
  wait "$pid"; local rc=$?
  if [ "$rc" = 0 ]; then draw done "$2" "$3"; sleep 0.25; fi
  printf '\r\033[2K\033[?25h'
  return "$rc"
}

sha256() {
  if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum > /dev/null; then shasum -a 256 "$1" | cut -d' ' -f1
  else error "sha256sum (coreutils) is required to verify the download"
  fi
}

# --- get the binary ---------------------------------------------------------
if [ -n "$LOCAL_BINARY" ]; then
  [ -f "$LOCAL_BINARY" ] || error "no such file: $LOCAL_BINARY"
  info "${BOLD}Installing rtn${RESET} from ${LOCAL_BINARY}"
  cp "$LOCAL_BINARY" "$TMP/rtn"
else
  if [ -z "$VERSION" ]; then
    download "$RELEASES/latest/download/VERSION" "$TMP/VERSION" \
      || error "could not find the latest release at $RELEASES"
    VERSION="$(tr -d ' \r\n' < "$TMP/VERSION")"
    VERSION="${VERSION#v}"
  fi
  base="$RELEASES/download/v$VERSION"
  if [ "$FANCY" = 1 ]; then
    printf '\n  %s●%s %sRunTime-Now%s  %sinstall%s\n\n' "$BLUE" "$RESET" "$BOLD" "$RESET" "$DIM" "$RESET"
    step "Version" "v$VERSION ($os-$arch)"
  else
    info "${BOLD}Installing rtn ${BLUE}v$VERSION${RESET} ${DIM}($os-$arch)${RESET}"
    step "downloading" "$base/$TARGET.tar.gz"
  fi
  download_with_progress "$base/$TARGET.tar.gz" "$TMP/$TARGET.tar.gz" "Downloading" \
    || error "download failed — does version $VERSION exist with a $os-$arch build?"
  [ "$FANCY" = 1 ] && step "Downloaded" "$TARGET.tar.gz · $(human "$(stat -c %s "$TMP/$TARGET.tar.gz")")"
  download "$base/SHA256SUMS" "$TMP/SHA256SUMS" || error "could not download SHA256SUMS"
  expected="$(awk -v f="$TARGET.tar.gz" '$2 == f || $2 == "*"f { print $1 }' "$TMP/SHA256SUMS")"
  actual="$(sha256 "$TMP/$TARGET.tar.gz")"
  [ -n "$expected" ] && [ "$expected" = "$actual" ] \
    || error "checksum mismatch for $TARGET.tar.gz — the download is corrupted. Nothing was installed."
  if [ "$FANCY" = 1 ]; then step "Verified" "sha256 ${actual:0:16}…"; else step "verified" "sha256 ${actual:0:16}…"; fi
  tar -xzf "$TMP/$TARGET.tar.gz" -C "$TMP"
  mv "$TMP/$TARGET/rtn" "$TMP/rtn"
fi

chmod 755 "$TMP/rtn"
"$TMP/rtn" --version > /dev/null 2>&1 || error "the rtn binary does not run on this system"

# --- install (atomic replace, so a running rtn is never half-written) --------
mkdir -p "$BIN_DIR"
cp "$TMP/rtn" "$BIN_DIR/.rtn.new"
mv -f "$BIN_DIR/.rtn.new" "$BIN_DIR/rtn"
installed="$("$BIN_DIR/rtn" --version)"
tilde_bin="${BIN_DIR/#$HOME/\~}"
if [ "$FANCY" = 1 ]; then
  step "Installed" "${tilde_bin:-$BIN_DIR}/rtn  ($installed)"
else
  info "${BLUE}✓${RESET} ${installed} installed to ${BOLD}${tilde_bin:-$BIN_DIR}/rtn${RESET}"
fi

# --- PATH -------------------------------------------------------------------
case ":$PATH:" in
  *":$BIN_DIR:"*) on_path=1 ;;
  *) on_path=0 ;;
esac

config=""
if [ "$MODIFY_PATH" = 1 ] && [ "$on_path" = 0 ]; then
  shell_name="$(basename "${SHELL:-bash}")"
  case "$shell_name" in
    fish)
      config="$HOME/.config/fish/conf.d/rtn.fish"
      mkdir -p "$(dirname "$config")"
      if [ ! -f "$config" ]; then
        printf '# rtn\nset -gx RTN_INSTALL "%s"\nfish_add_path "%s"\n' "$INSTALL_DIR" "$BIN_DIR" > "$config"
      fi
      ;;
    zsh|bash)
      config="$HOME/.${shell_name}rc"
      if ! grep -qs 'RTN_INSTALL' "$config"; then
        printf '\n# rtn\nexport RTN_INSTALL="%s"\nexport PATH="$RTN_INSTALL/bin:$PATH"\n' "$INSTALL_DIR" >> "$config"
      fi
      ;;
    *) config="" ;;
  esac
fi

echo
if [ "$on_path" = 1 ]; then
  info "Run ${BLUE}rtn --help${RESET} to get started."
elif [ -n "$config" ]; then
  info "Added ${tilde_bin} to PATH in ${config/#$HOME/\~}"
  info "To get started, open a new terminal or run:"
  echo
  info "  ${BLUE}exec \$SHELL${RESET}"
  info "  ${BLUE}rtn --help${RESET}"
else
  info "Add rtn to your PATH manually, e.g. in your shell config:"
  echo
  info "  ${BLUE}export PATH=\"$BIN_DIR:\$PATH\"${RESET}"
fi
echo
info "${DIM}Upgrade later with: rtn upgrade   •   Uninstall: rm -rf ${INSTALL_DIR/#$HOME/\~}${RESET}"
