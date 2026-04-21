#!/bin/bash
# demo.sh - Full demo sequence with screenshot prompts
# Run AFTER setup_and_run.sh has completed.
# Usage: sudo bash demo.sh
# Keep a second terminal open with the supervisor running first!

set -e
YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE="$REPO_DIR/boilerplate/engine"
ROOTFS="$REPO_DIR/rootfs-alpha"

screenshot() {
    echo ""
    echo -e "${YELLOW}>>> SCREENSHOT $1: $2${NC}"
    echo "    Press ENTER when you have taken the screenshot..."
    read -r
}

[ "$EUID" -ne 0 ] && { echo "Run with sudo"; exit 1; }

echo -e "${GREEN}=== OS-Jackfruit Demo ===${NC}"
echo ""
echo "BEFORE YOU CONTINUE:"
echo "  Open a SECOND terminal and run:"
echo "    sudo $ENGINE supervisor $ROOTFS"
echo "  Then come back here and press ENTER."
read -r

# ── Screenshot 1: Module loaded ──────────────────────────────────────
echo ""
echo "=== Kernel module status ==="
dmesg | grep -i monitor | tail -5
ls -l /dev/container_monitor 2>/dev/null || echo "(device not found – module not loaded)"
screenshot 1 "dmesg showing monitor module loaded + /dev/container_monitor exists"

# ── Start containers ─────────────────────────────────────────────────
echo ""
echo "=== Starting containers alpha and beta ==="
$ENGINE start alpha - /bin/sh -c 'while true; do echo hello-from-alpha; sleep 1; done'
sleep 1
$ENGINE start beta  - /bin/sh -c 'while true; do echo hello-from-beta; sleep 2; done'
sleep 3

# ── Screenshot 2: ps output ───────────────────────────────────────────
echo ""
echo "=== Container list (ps) ==="
$ENGINE ps
screenshot 2 "engine ps showing alpha and beta in running state"

# ── Screenshot 3: Log file ────────────────────────────────────────────
echo ""
echo "=== Log contents for alpha ==="
sleep 3
cat /tmp/container_logs/alpha.log
screenshot 3 "Log file showing output captured via bounded-buffer pipeline"

# ── Screenshot 4: CLI stop command ───────────────────────────────────
echo ""
echo "=== Stopping alpha via CLI ==="
$ENGINE stop alpha
sleep 1
$ENGINE ps
screenshot 4 "CLI stop command sent over UNIX socket + ps showing alpha stopped"

# ── Screenshot 5 & 6: Memory limits ──────────────────────────────────
echo ""
echo "=== Starting memory_hog with soft=50MB hard=100MB ==="
$ENGINE start hog - soft=50 hard=100 /memory_hog 200 10 200
echo "Waiting 30s for soft limit to trigger..."
sleep 30
echo "=== dmesg - soft limit ==="
dmesg | grep -i monitor | tail -10
screenshot 5 "dmesg showing soft-limit WARNING for hog container"

echo "Waiting 60s for hard limit to trigger..."
sleep 60
echo "=== dmesg - hard limit ==="
dmesg | grep -i monitor | tail -10
echo ""
echo "=== ps after kill ==="
$ENGINE ps
screenshot 6 "dmesg showing hard-limit KILL + ps showing hog in killed state"

# ── Screenshot 7: Scheduling experiment ──────────────────────────────
echo ""
echo "=== Scheduling experiment: nice 0 vs nice 10 ==="
$ENGINE start sched-hi - /bin/nice -n 0  /cpu_hog 20
$ENGINE start sched-lo - /bin/nice -n 10 /cpu_hog 20
echo "Running 20s CPU experiment..."
sleep 22
echo "--- sched-hi (nice 0) ---"
cat /tmp/container_logs/sched-hi.log
echo "--- sched-lo (nice 10) ---"
cat /tmp/container_logs/sched-lo.log
screenshot 7 "Both logs side by side showing different Miter/s (nice 0 vs nice 10)"

# ── Screenshot 8: Clean teardown ──────────────────────────────────────
echo ""
echo "=== Shutting down supervisor ==="
$ENGINE shutdown || true
sleep 2
echo "=== Zombie check (should be empty) ==="
ps aux | grep defunct || echo "(no zombies found)"
echo ""
echo "=== Unloading kernel module ==="
rmmod monitor 2>/dev/null && echo "monitor.ko unloaded" || echo "(already unloaded)"
dmesg | tail -5
screenshot 8 "No zombies in ps aux + dmesg showing module unloaded cleanly"

echo ""
echo -e "${GREEN}=== Demo complete! Now push to GitHub: ===${NC}"
echo "  cd $REPO_DIR"
echo "  git add -A"
echo "  git commit -m 'Complete implementation with demo'"
echo "  git push origin main"
