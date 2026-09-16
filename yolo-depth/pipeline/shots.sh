#!/usr/bin/env bash
# Browse the PNG snapshots depth_cam writes, on the board's own HDMI output.
#
# Run this ON THE BOARD. The board has no general image viewer (no eog, feh,
# imv, ...), but Weston ships weston-image and GStreamer has pngdec, so this
# wraps whichever suits: weston-image opens a window at the image's own size,
# GStreamer can scale it to fill the panel.
#
# Usage:
#   ./shots.sh                  # step through every shot, newest last
#   ./shots.sh -f               # fullscreen (GStreamer path)
#   ./shots.sh -d <dir>         # somewhere other than /dev/shm
#   ./shots.sh -t <seconds>     # seconds per image before advancing (default 0
#                               # = wait for Enter)
#   ./shots.sh -l               # just list what is there, with sizes and times
#
# Exit code 0 on success, non-zero on failure.
set -euo pipefail

DIR=/dev/shm
FULLSCREEN=0
DWELL=0
LIST_ONLY=0

while getopts "fd:t:lh" opt; do
	case "$opt" in
	f) FULLSCREEN=1 ;;
	d) DIR="$OPTARG" ;;
	t) DWELL="$OPTARG" ;;
	l) LIST_ONLY=1 ;;
	h) sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
	*) echo "try -h" >&2; exit 2 ;;
	esac
done

export XDG_RUNTIME_DIR=/run/user/root
export WAYLAND_DISPLAY=wayland-1

# Sorted so shot-002 follows shot-001 rather than lexical surprises later on.
mapfile -t SHOTS < <(find "$DIR" -maxdepth 1 -name 'shot-*.png' -print | sort)

if [ "${#SHOTS[@]}" -eq 0 ]; then
	echo "no shot-*.png in $DIR -- take some with depth_cam's 's' key" >&2
	exit 1
fi

if [ "$LIST_ONLY" -eq 1 ]; then
	ls -lh --time-style=+%H:%M:%S "${SHOTS[@]}" | awk '{printf "%-34s %6s  %s\n", $NF, $5, $6}'
	exit 0
fi

echo "${#SHOTS[@]} shot(s) in $DIR"
[ "$DWELL" = 0 ] && echo "Enter = next, Ctrl-C = quit"

for f in "${SHOTS[@]}"; do
	echo "--> $(basename "$f")"
	if [ "$FULLSCREEN" -eq 1 ]; then
		# imagefreeze turns the single decoded frame into a live stream, which
		# is what waylandsink needs to keep a surface on screen.
		gst-launch-1.0 -q filesrc location="$f" ! pngdec ! imagefreeze ! \
			videoconvert ! waylandsink fullscreen=true >/dev/null 2>&1 &
	else
		weston-image "$f" >/dev/null 2>&1 &
	fi
	viewer=$!

	if [ "$DWELL" = 0 ]; then
		read -r _ </dev/tty || true
	else
		sleep "$DWELL"
	fi
	kill "$viewer" 2>/dev/null || true
	wait "$viewer" 2>/dev/null || true
done
