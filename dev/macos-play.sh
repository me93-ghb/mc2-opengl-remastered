#!/bin/bash
# Build the latest code if anything changed, then start the game in a window.
# Used by ~/Applications/MC2.app (see dev/macos-make-app.sh); also works from a
# terminal. Extra args pass through to mc2, e.g.  dev/macos-play.sh -mission e3demo
#
# Env: dev/mc2app.env (or $MC2_ENV_FILE). Build output: /tmp/mc2-build.log.
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_LOG=/tmp/mc2-build.log
# Finder/Dock launches get a bare PATH; cmake and make live in Homebrew/Xcode.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

alert() {  # alert <message> <buttons...>; prints the button pressed
    local msg="$1"; shift
    local buttons; buttons=$(printf '"%s",' "$@"); buttons="{${buttons%,}}"
    osascript -e "button returned of (display alert \"MechCommander 2\" message \"$msg\" buttons $buttons default button 1)" 2>/dev/null
}

if ! cmake --build "$REPO/build-mac" --target mc2 -j"$(sysctl -n hw.ncpu)" > "$BUILD_LOG" 2>&1; then
    if [ -x "$REPO/build-mac/mc2" ]; then
        choice=$(alert "The latest code failed to build. The log is in $BUILD_LOG." "Play last build" "Show log" "Quit")
    else
        choice=$(alert "The game failed to build. The log is in $BUILD_LOG." "Show log" "Quit")
    fi
    case "$choice" in
        "Play last build") ;;
        "Show log") open -e "$BUILD_LOG"; exit 1 ;;
        *) exit 1 ;;
    esac
fi

cd "$REPO/run" || exit 1
ENV_FILE="${MC2_ENV_FILE:-$REPO/dev/mc2app.env}"
[ -f "$ENV_FILE" ] && . "$ENV_FILE"
export MC2_MACOS_WINDOW=1
# One log per session so it does not grow forever.
exec ../dev/macos-run.sh "$@" > "${MC2_RUN_LOG:-/tmp/mc2app.log}" 2>&1
