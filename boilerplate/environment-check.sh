#!/bin/bash
# environment-check.sh - VM environment preflight check for OS-Jackfruit
# Must be run as root (sudo ./environment-check.sh)

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAIL=1; }

FAIL=0

echo "=== OS-Jackfruit Environment Check ==="
echo

# Root check
if [ "$EUID" -ne 0 ]; then
    fail "Must run as root (sudo)"
    exit 1
else
    pass "Running as root"
fi

# OS check
if grep -q "Ubuntu" /etc/os-release 2>/dev/null; then
    . /etc/os-release
    pass "OS: $NAME $VERSION_ID"
    if [[ "$VERSION_ID" != "22.04" && "$VERSION_ID" != "24.04" ]]; then
        warn "Recommended Ubuntu 22.04 or 24.04 (got $VERSION_ID)"
    fi
else
    fail "Not running Ubuntu – kernel module loading may not work"
fi

# Secure Boot check
if command -v mokutil &>/dev/null; then
    SB=$(mokutil --sb-state 2>&1)
    if echo "$SB" | grep -q "SecureBoot disabled"; then
        pass "Secure Boot: disabled"
    else
        fail "Secure Boot appears enabled – kernel module loading will fail. Disable in BIOS."
    fi
else
    warn "mokutil not found – cannot check Secure Boot status"
fi

# WSL check
if grep -qi "microsoft" /proc/version 2>/dev/null; then
    fail "Running inside WSL – kernel modules and namespaces will NOT work"
else
    pass "Not running in WSL"
fi

# Kernel headers
KVER=$(uname -r)
KDIR="/lib/modules/$KVER/build"
if [ -d "$KDIR" ]; then
    pass "Kernel headers found: $KDIR"
else
    fail "Kernel headers missing: $KDIR\n  Run: sudo apt install linux-headers-$(uname -r)"
fi

# Build tools
for tool in gcc make modprobe; do
    if command -v $tool &>/dev/null; then
        pass "$tool found: $(which $tool)"
    else
        fail "$tool not found – run: sudo apt install build-essential"
    fi
done

# Namespace support
for ns in pid uts mnt; do
    if [ -f "/proc/self/ns/$ns" ]; then
        pass "Namespace supported: $ns"
    else
        fail "Namespace NOT supported: $ns (need real Linux kernel)"
    fi
done

# User namespaces (optional but useful)
if [ -f "/proc/sys/kernel/unprivileged_userns_clone" ]; then
    VAL=$(cat /proc/sys/kernel/unprivileged_userns_clone)
    if [ "$VAL" = "1" ]; then
        pass "Unprivileged user namespaces: enabled"
    else
        warn "Unprivileged user namespaces: disabled (ok for root use)"
    fi
fi

# chroot capability
if unshare --pid --fork --mount-proc true 2>/dev/null; then
    pass "unshare(2) works (namespace creation available)"
else
    fail "unshare(2) failed – namespace isolation will not work"
fi

# /tmp writable
if touch /tmp/.engine_check_$$ 2>/dev/null && rm /tmp/.engine_check_$$; then
    pass "/tmp is writable"
else
    fail "/tmp is not writable"
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}All checks passed. Environment is ready.${NC}"
else
    echo -e "${RED}Some checks FAILED. Fix the issues above before proceeding.${NC}"
    exit 1
fi
