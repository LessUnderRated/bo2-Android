#!/bin/bash
# icon_margin.sh <png> — shrinks the game's icon to 85 % on a black square, in place: the exe's orange bars run almost
# edge to edge, which looks cramped once macOS rounds the corners.
N=$(sips -g pixelWidth "$1" | awk '/pixelWidth/{print $2}')
sips -Z $((N * 85 / 100)) "$1" >/dev/null && sips --padToHeightWidth "$N" "$N" --padColor 000000 "$1" >/dev/null 2>&1
