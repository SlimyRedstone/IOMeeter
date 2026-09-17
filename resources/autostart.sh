#!/bin/sh
#
# Started by the desktop's own autostart, and the trigger the systemd unit is
# missing.
#
# IOMeeter.service is WantedBy=graphical-session.target, which only GNOME, KDE
# and the compositors that launch their session under systemd ever reach.
# Cinnamon, XFCE, MATE, LXQt and the tiling window managers do not, so the unit
# sits enabled and never starts. XDG autostart is the one mechanism every
# desktop implements, so that is what gets used to reach the unit.
#
# Two jobs, in order:
#
#   1. Push the session's display into the user manager. A systemd user unit
#      inherits the manager's environment, not the session's, so without this
#      IOMeeter starts with no DISPLAY and GLFW cannot open a window.
#   2. Start the unit, so its WorkingDirectory and its restart policy still
#      apply and there is one description of how IOMeeter runs rather than two.
#
# Falling back to running the binary directly when there is no user manager to
# talk to, which is a container or a distribution without systemd.
#
# Harmless where the unit already started on its own: systemctl start on an
# active unit does nothing, and instance.c would hand a second copy over to the
# first in any case.

set -u

SERVICE=IOMeeter.service
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# "Start on boot", from the configuration rather than from whether this file is
# installed. The desktop runs this script at every login either way, so the
# flag is what decides whether it goes any further -- clearing the checkbox
# does not have to reach into ~/.config/autostart, and turning it back on does
# not have to put anything back.
#
# Read with grep rather than a JSON parser: the key is written by
# config_to_text() on one line of its own, and a shell script is not the place
# to learn to parse JSON.
CONFIG="$HERE/config.json"

if [ -f "$CONFIG" ] &&
   grep -Eq '"start_on_boot"[[:space:]]*:[[:space:]]*false' "$CONFIG"; then
    exit 0
fi

if command -v systemctl >/dev/null 2>&1 &&
   systemctl --user show-environment >/dev/null 2>&1; then

    systemctl --user import-environment \
        DISPLAY XAUTHORITY WAYLAND_DISPLAY XDG_SESSION_TYPE 2>/dev/null

    # --no-block: the desktop is still starting the rest of the session and
    # must not wait on this one.
    if systemctl --user start --no-block "$SERVICE" 2>/dev/null; then
        exit 0
    fi
fi

exec "$HERE/IOMeeter"
