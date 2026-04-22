#!/bin/bash
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BOILER="$REPO_DIR/boilerplate"

echo "Launching demo with 2 terminals..."

# Terminal 1 → supervisor
gnome-terminal --title="Supervisor" -- bash -lc "
cd '$BOILER';
echo 'Starting supervisor...';
sudo ./engine supervisor ../rootfs-alpha;
exec bash
" &

sleep 2

# Terminal 2 → demo script
gnome-terminal --title="Demo" -- bash -lc "
cd '$REPO_DIR';
echo 'Running demo...';
sudo bash ./demo.sh;
exec bash
" &

sleep 2

# Tile windows side-by-side (best effort)
wmctrl -l

WIN1=$(wmctrl -l | grep Supervisor | awk '{print $1}' | head -n1)
WIN2=$(wmctrl -l | grep Demo | awk '{print $1}' | head -n1)

if [ -n "$WIN1" ] && [ -n "$WIN2" ]; then
    read SW SH < <(xdpyinfo | awk '/dimensions:/{split($2,a,"x"); print a[1],a[2]}')
    HALF=$((SW / 2))

    wmctrl -i -r "$WIN1" -e "0,0,0,$HALF,$SH"
    wmctrl -i -r "$WIN2" -e "0,$HALF,0,$HALF,$SH"
fi

echo "Setup complete."
echo "Screenshots will be saved in: screenshots/"
