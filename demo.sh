#!/bin/bash
# demo.sh - Full demo sequence with AUTO screenshots

set -e
YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE="$REPO_DIR/boilerplate/engine"
ROOTFS="$REPO_DIR/rootfs-alpha"

# 🔥 AUTO SCREENSHOT FUNCTIO
screenshot() {
    USER_HOME=$(eval echo "~$SUDO_USER")
    SHOT_DIR="$USER_HOME/Downloads/OS-Jackfruit/OS-Jackfruit/screenshots"
    FILE="$SHOT_DIR/s${1}.png"

    mkdir -p "$SHOT_DIR"
    chown -R "$SUDO_USER:$SUDO_USER" "$SHOT_DIR"

    # wait for terminal to settle
    sleep 2

    # take screenshot silently
    sudo -u "$SUDO_USER" env DISPLAY="$DISPLAY" \
        XDG_RUNTIME_DIR="/run/user/$(id -u "$SUDO_USER")" \
        gnome-screenshot -f "$FILE" >/dev/null 2>&1
}

[ "$EUID" -ne 0 ] && { echo "Run with sudo"; exit 1; }

echo -e "${GREEN}=== OS-Jackfruit Demo ===${NC}"
echo ""

echo "MAKE SURE supervisor is running in another terminal:"
echo "sudo $ENGINE supervisor $ROOTFS"
echo "Starting demo in 5 seconds..."
sleep 5

# ── Screenshot 1 ──────────────────────────────────────
echo ""
echo "=== Kernel module status ==="
dmesg | grep -i monitor | tail -5
ls -l /dev/container_monitor 2>/dev/null || echo "(device not found)"
screenshot 1 "Module loaded + device exists"

# ── Start containers ─────────────────────────────────
echo ""
echo "=== Starting containers ==="
$ENGINE start alpha - /bin/echo hello-from-alpha
sleep 1
$ENGINE start beta - /bin/echo hello-from-beta
sleep 3

# ── Screenshot 2 ─────────────────────────────────────
echo ""
echo "=== engine ps ==="
$ENGINE ps
screenshot 2 "alpha + beta running"

# ── Screenshot 3 ─────────────────────────────────────
echo ""
echo "=== Logs ==="
sleep 3
cat /tmp/container_logs/alpha.log
screenshot 3 "alpha log output"

# ── Screenshot 4 ─────────────────────────────────────
echo ""
echo "=== Stop alpha ==="
$ENGINE stop alpha
sleep 1
$ENGINE ps
screenshot 4 "alpha stopped"

# ── Screenshot 5 & 6 ────────────────────────────────
echo ""
echo "=== Memory limits ==="
$ENGINE start hog - soft=50 hard=100 /memory_hog 200 10 200

echo "Waiting 30s for soft limit..."
sleep 30
dmesg | grep -i monitor | tail -10
screenshot 5 "soft limit warning"

echo "Waiting 60s for hard limit..."
sleep 60
dmesg | grep -i monitor | tail -10
echo ""
$ENGINE ps
screenshot 6 "hard limit kill"

# ── Screenshot 7 ─────────────────────────────────────
echo ""
echo "=== Scheduling test ==="
$ENGINE start sched-hi - /bin/nice -n 0  /cpu_hog 20
$ENGINE start sched-lo - /bin/nice -n 10 /cpu_hog 20

sleep 22

echo "--- sched-hi ---"
cat /tmp/container_logs/sched-hi.log
echo "--- sched-lo ---"
cat /tmp/container_logs/sched-lo.log
screenshot 7 "nice 0 vs nice 10"

# ── Screenshot 8 ─────────────────────────────────────
echo ""
echo "=== Shutdown ==="
$ENGINE shutdown || true
sleep 2

echo "=== Zombie check ==="
ps aux | grep defunct || echo "(no zombies)"

echo ""
echo "=== Unload module ==="
rmmod monitor 2>/dev/null && echo "unloaded" || echo "(already unloaded)"
dmesg | tail -5

screenshot 8 "cleanup complete"

echo ""
echo -e "${GREEN}=== Demo complete ===${NC}"
echo "Screenshots saved in: screenshots/"
