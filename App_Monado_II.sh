#!/usr/bin/env bash
# A desktop/Quiver entrypoint: visible local setup, then the ordinary launcher.
# No game executable or game input is distributed, and Play stays a user action.
set -euo pipefail
monado_app_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

monado_app_error() {
    local message=$1
    printf '%s\n' "$message" >&2
    # Desktop launchers commonly discard stderr. Use an already-installed dialog
    # when possible; never install a terminal/dialog tool or invoke sudo.
    if [[ ! -t 2 && ( -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ) ]]; then
        if command -v kdialog >/dev/null 2>&1; then
            kdialog --title 'Monado II' --error "$message" || true
        elif command -v zenity >/dev/null 2>&1; then
            zenity --error --title='Monado II' --text="$message" || true
        fi
    fi
}

monado_app_play() {
    local result
    if "$monado_app_root/Play_Monado_II.sh"; then
        return 0
    else
        result=$?
    fi
    monado_app_error "Monado II could not open its launcher (exit $result). Open a terminal in this folder and run ./Play_Monado_II.sh for details. See docs/BUILD_LINUX.md."
    return "$result"
}

monado_app_terminal() {
    local result
    if "$@"; then
        return 0
    else
        result=$?
    fi
    monado_app_error "Monado II setup did not complete (exit $result). Open a terminal in this folder and run ./Build_Monado_II.sh. See docs/BUILD_LINUX.md."
    return "$result"
}

monado_app_setup() {
    local result
    printf '%s\n' 'Monado II — local setup' \
        'Use your own extracted DE 2.0.0 game files in game_files/exefs and game_files/romfs.' \
        'Missing prerequisites/input are reported below. Setup does not install system packages.' \
        'A successful build opens the settings launcher; it never starts the game automatically.' ''
    cd -- "$monado_app_root"
    if "$monado_app_root/Build_Monado_II.sh"; then
        monado_app_play
        return $?
    else
        result=$?
    fi
    printf '\n%s\n' "Setup stopped (exit $result); the game was not launched." \
        'Fix the reported issue, then open App_Monado_II.sh again.' \
        'Full instructions: docs/BUILD_LINUX.md'
    # Keep actionable first-build/input errors visible. Interrupting setup exits
    # promptly instead of asking the user to acknowledge a cancelled operation.
    if [[ -t 0 && -t 1 && $result != 130 && $result != 143 ]]; then
        read -r -p 'Press Enter to close this setup window. ' _ || true
    fi
    return "$result"
}

if [[ $# == 1 && $1 == --terminal-setup ]]; then
    monado_app_setup
    exit $?
fi
if [[ $# != 0 ]]; then
    monado_app_error 'Usage: ./App_Monado_II.sh. Use ./Build_Monado_II.sh for build options and ./Play_Monado_II.sh for launcher options.'
    exit 64
fi

# The checked Play entrypoint diagnoses a malformed or linked existing image;
# do not silently replace it by rebuilding.
if [[ -e $monado_app_root/Monado-II-x86_64.AppImage || -L $monado_app_root/Monado-II-x86_64.AppImage ]]; then
    monado_app_play
    exit $?
fi
if [[ -t 0 && -t 1 ]]; then
    monado_app_setup
    exit $?
fi

if [[ -n ${DISPLAY:-} || -n ${WAYLAND_DISPLAY:-} ]]; then
    # Pass every path as its own argument, including installation paths with
    # spaces. No shell command string/eval or environment command override.
    if command -v x-terminal-emulator >/dev/null 2>&1; then
        monado_app_terminal x-terminal-emulator -e "$BASH" "$monado_app_root/App_Monado_II.sh" --terminal-setup
        exit $?
    elif command -v konsole >/dev/null 2>&1; then
        monado_app_terminal konsole --nofork -e "$BASH" "$monado_app_root/App_Monado_II.sh" --terminal-setup
        exit $?
    elif command -v gnome-terminal >/dev/null 2>&1; then
        monado_app_terminal gnome-terminal --wait -- "$BASH" "$monado_app_root/App_Monado_II.sh" --terminal-setup
        exit $?
    elif command -v xterm >/dev/null 2>&1; then
        monado_app_terminal xterm -e "$BASH" "$monado_app_root/App_Monado_II.sh" --terminal-setup
        exit $?
    fi
fi
monado_app_error 'Monado II needs a terminal for its first local build. Open a terminal in this folder and run ./Build_Monado_II.sh, then ./Play_Monado_II.sh. See docs/BUILD_LINUX.md for prerequisites and game_files placement. Desktop setup supports x-terminal-emulator, Konsole, GNOME Terminal or xterm.'
exit 1
