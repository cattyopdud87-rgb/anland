#!/usr/bin/env bash
# Foreground Wayfire session with isolated Anland wlroots 0.20 backend.
# Usage: wayfire-anland [-c config_file] [wayfire options...]
set -uo pipefail

WAYFIRE_PREFIX=${WAYFIRE_ANLAND_PREFIX:-/usr/lib/wayfire-anland}
BIN=${WAYFIRE_BINARY:-/usr/bin/wayfire}

die() { printf 'wayfire-anland: %s\n' "$*" >&2; exit 1; }

[[ -x "$BIN" ]] || die "wayfire binary not found at $BIN; install wayfire first"

# Isolate custom wlroots 0.20 + libdisplay_producer
export LD_LIBRARY_PATH="$WAYFIRE_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export WLR_BACKENDS=anland
export ANLAND=1
export ANLAND_SKIP_IMPLICIT_SYNC_WAIT=1
export FD_DEV_FEATURES=${FD_DEV_FEATURES:-enable_tp_ubwc_flag_hint=1}
export WLR_RENDER_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export ANLAND_DRM_DEVICE="$WLR_RENDER_DRM_DEVICE"
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=Wayfire

export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null || true

[[ -d "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]] || die "runtime directory must exist and be writable"
[[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die "render node unavailable: $ANLAND_DRM_DEVICE"

# Default config resolution
config=""
if [[ -n "${WAYFIRE_CONFIG_FILE:-}" ]]; then
    config="$WAYFIRE_CONFIG_FILE"
elif [[ -f "$HOME/.config/wayfire.ini" ]]; then
    config="$HOME/.config/wayfire.ini"
elif [[ -f /etc/wayfire/wayfire.ini ]]; then
    config="/etc/wayfire/wayfire.ini"
fi

unset WAYLAND_DISPLAY DISPLAY

exec 9>"$XDG_RUNTIME_DIR/anland-wayfire-session.lock" || die "cannot open session lock"
flock -n 9 || die "another producer owns the session"

for name in Hyprland mango kwin_wayland gnome-shell niri gamescope wayfire; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; not taking it over"; fi
done

args=()
if [[ -n "$config" ]]; then
    args+=(-c "$config")
fi

env --default-signal=INT,QUIT "$BIN" "${args[@]}" "$@" <&0 9>&- &
child=$!
forward() { kill -TERM "$child" 2>/dev/null || true; }
trap forward INT QUIT TERM HUP
rc=0
while :; do
    wait "$child" && { rc=0; break; }
    rc=$?
    kill -0 "$child" 2>/dev/null || break
done
trap - INT QUIT TERM HUP
exit "$rc"
