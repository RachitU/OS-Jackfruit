#!/bin/bash
# setup_and_run.sh
# Run this on your Ubuntu VM after cloning your fork.
# Usage: sudo bash setup_and_run.sh
# It installs deps, builds everything, preps rootfs, loads the module.

set -e
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[*]${NC} $1"; }
warn()  { echo -e "${YELLOW}[!]${NC} $1"; }
die()   { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

[ "$EUID" -ne 0 ] && die "Run with sudo: sudo bash setup_and_run.sh"

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BOILER="$REPO_DIR/boilerplate"

info "=== Step 1: Install dependencies ==="
apt-get update -qq
apt-get install -y build-essential linux-headers-$(uname -r) wget tar 2>&1 | tail -5
info "Dependencies installed."

info "=== Step 2: Build user-space binaries ==="
cd "$BOILER"
make ci
info "User-space binaries built: engine, memory_hog, cpu_hog, io_pulse"

info "=== Step 3: Build kernel module ==="
make kmodule && info "Kernel module built: monitor.ko" || warn "Kernel module build failed (check headers). Memory limits will be disabled."

info "=== Step 4: Prepare Alpine rootfs ==="
cd "$REPO_DIR"
if [ ! -d rootfs-base ]; then
    mkdir rootfs-base
    ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz"
    TARBALL="alpine-minirootfs.tar.gz"
    info "Downloading Alpine mini rootfs..."
    wget -q --show-progress -O "$TARBALL" "$ALPINE_URL"
    tar -xzf "$TARBALL" -C rootfs-base
    rm "$TARBALL"
    info "rootfs-base ready."
else
    info "rootfs-base already exists, skipping download."
fi

for name in alpha beta; do
    if [ ! -d "rootfs-$name" ]; then
        cp -a rootfs-base "rootfs-$name"
        info "Created rootfs-$name"
    else
        info "rootfs-$name already exists."
    fi
done

info "Copying workload binaries into rootfs..."
cp "$BOILER/memory_hog" "$BOILER/cpu_hog" "$BOILER/io_pulse" rootfs-alpha/
cp "$BOILER/memory_hog" "$BOILER/cpu_hog" "$BOILER/io_pulse" rootfs-beta/

info "=== Step 5: Load kernel module ==="
if lsmod | grep -q monitor; then
    warn "Monitor module already loaded."
else
    if [ -f "$BOILER/monitor.ko" ]; then
        insmod "$BOILER/monitor.ko" && info "monitor.ko loaded." || warn "insmod failed."
    else
        warn "monitor.ko not found — skipping load."
    fi
fi

echo ""
echo -e "${GREEN}=====================================================${NC}"
echo -e "${GREEN}  Setup complete! Now run the demo:${NC}"
echo ""
echo "  TERMINAL 1 (supervisor):"
echo "    sudo $BOILER/engine supervisor $REPO_DIR/rootfs-alpha"
echo ""
echo "  TERMINAL 2 (commands):"
echo "    sudo $BOILER/engine start alpha - /bin/sh -c 'while true; do echo hello; sleep 1; done'"
echo "    sudo $BOILER/engine start beta  - /bin/sh -c 'while true; do echo beta; sleep 2; done'"
echo "    sudo $BOILER/engine ps"
echo "    sudo $BOILER/engine logs alpha"
echo "    sudo $BOILER/engine stop alpha"
echo "    sudo $BOILER/engine shutdown"
echo ""
echo "  When done:"
echo "    sudo rmmod monitor"
echo -e "${GREEN}=====================================================${NC}"
