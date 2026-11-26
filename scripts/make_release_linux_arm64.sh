#!/usr/bin/env bash
# The 64-bit ARM Linux release, made on an x86 PC. It runs make_release_linux.sh
# unchanged inside an ARM Ubuntu 24.04 that QEMU emulates, so everything it
# builds and bundles (launcher, engines, builders, GCC 15) is ARM code.
# Arguments go to make_release_linux.sh; the result lands in
# Releases/linux-arm64. Emulation makes this much slower than the x86 build.
#
# Run as root in WSL Ubuntu 24.04 (make_release.ps1 -LinuxArm64 does).
# First use installs qemu-user-static and mmdebstrap on the host, and creates
# the ARM system in $GSR_ARM64_ROOT (default /root/gsr-arm64-root). Its mounts
# live in a private mount namespace, so they vanish with this script.
# --prepare only sets up the ARM system and stops (no build).
set -euo pipefail
prepare_only=0
if [ "${1:-}" = --prepare ]; then prepare_only=1; shift; fi

[ "$(id -u)" = 0 ] || { echo "Run as root (wsl -u root)." >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || { echo "On an ARM machine run make_release_linux.sh itself." >&2; exit 1; }
collection_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
arm_root=$(realpath -m "${GSR_ARM64_ROOT:-/root/gsr-arm64-root}")
case "$arm_root" in /|/mnt|/mnt/*) echo "Unsafe ARM system path: $arm_root" >&2; exit 1 ;; esac

# ---- 1. QEMU on the host ------------------------------------------------------
missing=()
command -v qemu-aarch64-static >/dev/null || missing+=(qemu-user-static)
command -v mmdebstrap >/dev/null || missing+=(mmdebstrap)
if [ ${#missing[@]} -gt 0 ]; then
    echo "Installing ${missing[*]}"
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${missing[@]}"
fi
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    systemctl restart systemd-binfmt 2>/dev/null || true
fi
[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || {
    echo "ARM programs cannot run here: QEMU's binfmt entry qemu-aarch64 is not registered." >&2; exit 1; }
# F (fix binary): the chroot needs no copy of QEMU inside it.
grep -q '^flags:.*F' /proc/sys/fs/binfmt_misc/qemu-aarch64 || {
    echo "QEMU's qemu-aarch64 binfmt entry lacks the F flag; chroot would fail." >&2; exit 1; }

# ---- 2. The ARM Ubuntu ----------------------------------------------------------
# The same release (24.04) and GCC source (the ubuntu-toolchain-r/test PPA) as
# the x86 build, so both packages ask the same glibc of players.
packages=build-essential,cmake,ninja-build,make,binutils,zip,python3,git,curl,ca-certificates,file,mount,libsdl2-dev,zlib1g-dev,libgl-dev
if [ ! -f "$arm_root/.gsr-ready" ]; then
    if [ ! -f "$arm_root/.gsr-base" ]; then
        echo "Creating the ARM Ubuntu in $arm_root (first use only)"
        rm -rf -- "$arm_root"
        mmdebstrap --architectures=arm64 --variant=apt --include="$packages" noble "$arm_root" \
            "deb http://ports.ubuntu.com/ubuntu-ports noble main universe" \
            "deb http://ports.ubuntu.com/ubuntu-ports noble-updates main universe" \
            "deb http://ports.ubuntu.com/ubuntu-ports noble-security main universe"
        touch "$arm_root/.gsr-base"
    fi
    ppa=$(ls /etc/apt/sources.list.d/ubuntu-toolchain-r-ubuntu-test-noble.sources 2>/dev/null || true)
    [ -n "$ppa" ] || { echo "The host lacks the ubuntu-toolchain-r/test PPA that gives GCC 15." >&2; exit 1; }
    cp "$ppa" "$arm_root/etc/apt/sources.list.d/"
    cp /etc/resolv.conf "$arm_root/etc/resolv.conf"
    # Launchpad's PPA server answers busy (503) to bursts: retry, one at a time.
    echo "Installing GCC 15 in the ARM Ubuntu"
    chroot "$arm_root" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive \
        bash -c 'apt-get update -qq && apt-get install -y -qq -o Acquire::Retries=8 \
                 -o Acquire::Queue-Mode=access gcc-15 g++-15'
    touch "$arm_root/.gsr-ready"
fi
[ "$(chroot "$arm_root" uname -m)" = aarch64 ] || { echo "The ARM system does not run as ARM." >&2; exit 1; }
if [ "$prepare_only" = 1 ]; then
    echo "ARM system ready: $arm_root ($(chroot "$arm_root" g++-15 -dumpmachine), GCC $(chroot "$arm_root" g++-15 -dumpversion))"
    exit 0
fi

# ---- 3. The release, inside the ARM system ---------------------------------------
# /mnt (the Windows drives: this checkout and the test ROMs) appears at the
# same paths inside, so the script's paths and GSR_ROM/TLA_ROM work as given.
case "$collection_root" in /mnt/*) ;; *) echo "Expected the checkout under /mnt: $collection_root" >&2; exit 1 ;; esac
cp /etc/resolv.conf "$arm_root/etc/resolv.conf"
mkdir -p "$arm_root/mnt"
export ARM_ROOT=$arm_root COLLECTION=$collection_root
exec unshare --mount --propagation private bash -c '
    set -e
    # Its own mount, so the release test'"'"'s unshare -m works inside the chroot.
    mount --bind "$ARM_ROOT" "$ARM_ROOT"
    mount -t proc proc "$ARM_ROOT/proc"
    mount --rbind /sys "$ARM_ROOT/sys"
    mount --rbind /dev "$ARM_ROOT/dev"
    mount --rbind /mnt "$ARM_ROOT/mnt"
    mount -t tmpfs tmpfs "$ARM_ROOT/tmp"
    exec chroot "$ARM_ROOT" /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 \
        GSR_ROM="${GSR_ROM:-}" TLA_ROM="${TLA_ROM:-}" GSR_REPORT_HOST="${GSR_REPORT_HOST:-}" \
        GSR_LINUX_WORK=/root/gsr-linux-release \
        bash -c "cd \"\$0\" && exec bash scripts/make_release_linux.sh \"\$@\"" "$COLLECTION" "$@"
' _ "$@"
