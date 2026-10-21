#!/bin/bash
#
# build_rtems_rpi3.sh - RTEMS 6 for the Raspberry Pi 3B+ (BSP arm/raspberrypi2)
#
# Downloads, patches and builds everything the EPICS IOC in this repository
# needs below EPICS base, in this order:
#
#   1. RTEMS Source Builder  -> cross tools arm-rtems6-* (30..60 min)
#   2. RTEMS kernel          -> librtemsbsp.a etc.  + interrupt dispatcher fix
#   3. rtems-libbsd          -> libbsd.a (USB, LAN7800 Ethernet, TCP/IP) + Pi patch
#   4. rtems-net-services    -> libntp.a, libttcp.a, telnetd
#   5. libtecla              -> libtecla.a (iocsh line editing)
#
# Exact upstream commits are pinned to the ones verified on the Pi (see
# patches/RTEMS6_RASPBERRYPI_BUILD.md section 1). Patches come from the
# patches/ directory next to this scripts/ directory.
#
# Asks for two directories:
#   sources  (default ~/RTEMS_SRC)  cross tools build, kernel and modules
#   install  (default ~/RTEMS_PI)   tools, headers, libraries; the RTEMS prefix
#                                   becomes <install>/rtems-6/6
#
# Usage: scripts/build_rtems_rpi3.sh [-y] [--prepare-only] [-h]
#   -y              take the defaults (or RTEMS_SRC_DIR / RTEMS_INSTALL_DIR from
#                   the environment) without asking
#   --prepare-only  clone, check out and patch, but do not configure or build
#   -h              this text
# Environment: JOBS (default nproc), FORCE=1 rebuilds a step whose result is
# already installed, DRY_RUN=1 prints the build commands instead of running
# them.
#
# Re-running is safe: finished steps are skipped when their result exists.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
PATCH_DIR=$PROJECT_DIR/patches

# ---- versions (pinned, verified 2026-10) ----------------------------------
RTEMS_VERSION=6
RTEMS_CPU=arm
RTEMS_BSP=raspberrypi2

RSB_REPO=https://github.com/RTEMS/rtems-source-builder.git
RSB_COMMIT=3814cb0e7f86cca2be403eac831f9bf571984659      # branch 6, 2025-12-19
KERNEL_REPO=https://github.com/RTEMS/rtems.git
KERNEL_COMMIT=be446b02d091a2a745e03405eaab4de078571532   # branch 6, 2025-12-04
LIBBSD_REPO=https://github.com/RTEMS/rtems-libbsd.git
LIBBSD_COMMIT=c4fc203358208733463371409a73429d0b8f404e   # 6-freebsd-14, 2025-11-11
NETSVC_REPO=https://github.com/RTEMS/rtems-net-services.git
NETSVC_COMMIT=3381863ccf9f90f801fb16ad524a7fcff1301467   # branch 6, 2025-08-26
# Note: upstream re-pointed the "6.2" tags after these commits were taken;
# the commits above are what the IOC was verified with.

KERNEL_PATCH=$PATCH_DIR/rtems-raspberrypi-irq-dispatch-fix.patch
KERNEL_INI=$PATCH_DIR/rtems-kernel-raspberrypi2-config.ini
LIBBSD_PATCH=$PATCH_DIR/rtems-libbsd-raspberrypi.patch
# rtems-addon-packages (git.rtems.org, commit 4f5951a) is no longer online;
# the libtecla-1.6.3 tree and the RTEMS makefiles are kept in this repo.
TECLA_TAR=$PATCH_DIR/rtems-addon-packages-libtecla-1.6.3.tar.gz

HOST_PACKAGES="build-essential g++ gdb unzip pax bison flex texinfo python3-dev libpython3-dev libncurses-dev zlib1g-dev git u-boot-tools"

# ---- options ---------------------------------------------------------------
JOBS=${JOBS:-$(nproc)}
DRY_RUN=${DRY_RUN:-0}
FORCE=${FORCE:-0}
YES=0
PREPARE_ONLY=0
for arg in "$@"; do
    case $arg in
        -y) YES=1 ;;
        --prepare-only) PREPARE_ONLY=1 ;;
        -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (try -h)"; exit 2 ;;
    esac
done

# ---- helpers ---------------------------------------------------------------
say()  { printf '\n==== %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY_RUN" = 1 ]; then printf '  (dry) %s\n' "$*"; else "$@"; fi; }
enter() {   # cd for a build step; in a dry run a not yet cloned directory is tolerated
    if [ -d "$1" ]; then cd "$1"; elif [ "$DRY_RUN" = 1 ]; then printf '  (dry) in %s:\n' "$1"; else die "missing directory $1"; fi
}

ask() {   # ask "prompt" "default" -> answer (with ~ expanded)
    local answer
    if [ "$YES" = 1 ]; then
        answer=$2
    else
        read -r -e -p "$1 [$2]: " answer
        answer=${answer:-$2}
    fi
    answer=${answer/#\~/$HOME}
    printf '%s' "$answer"
}

clone_at() {   # clone_at REPO DIR COMMIT
    local repo=$1 dir=$2 commit=$3
    if [ ! -d "$dir/.git" ]; then
        say "cloning $repo -> $dir"
        run git clone "$repo" "$dir"
    fi
    [ "$DRY_RUN" = 1 ] && [ ! -d "$dir/.git" ] && return 0
    local head
    head=$(git -C "$dir" rev-parse HEAD)
    if [ "$head" != "$commit" ]; then
        if ! git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
            run git -C "$dir" fetch origin
        fi
        if [ -n "$(git -C "$dir" status --short --untracked-files=no)" ]; then
            echo "  $dir is at $head with local changes; expected $commit."
            echo "  Leaving the tree as it is (commit or stash, then re-run to switch)."
        else
            say "checking out $commit in $dir"
            run git -C "$dir" checkout --detach "$commit"
        fi
    else
        echo "  $dir already at $commit"
    fi
}

apply_patch() {   # apply_patch DIR PATCH MARKER_FILE MARKER_REGEX
    local dir=$1 patch=$2 marker_file=$3 marker=$4
    [ -f "$patch" ] || die "patch not found: $patch"
    if [ ! -d "$dir" ]; then
        [ "$DRY_RUN" = 1 ] && { echo "  (dry) would apply $(basename "$patch") in $dir"; return 0; }
        die "$dir does not exist"
    fi
    if [ -f "$dir/$marker_file" ] && grep -q -- "$marker" "$dir/$marker_file"; then
        echo "  $(basename "$patch") already applied"
        return 0
    fi
    say "applying $(basename "$patch") in $dir"
    (cd "$dir" && patch -p1 --dry-run < "$patch" > /dev/null) \
        || die "$(basename "$patch") does not apply cleanly to $dir"
    run bash -c "cd '$dir' && patch -p1 < '$patch'"
}

# ---- directories -----------------------------------------------------------
echo "RTEMS 6 for the Raspberry Pi 3B+: tools, kernel, libbsd, net-services, libtecla"
echo "Patches from: $PATCH_DIR"
SRC_DIR=$(ask "Directory for the sources (tools build, kernel, modules)" "${RTEMS_SRC_DIR:-$HOME/RTEMS_SRC}")
INSTALL_DIR=$(ask "Directory for executables, includes and libraries" "${RTEMS_INSTALL_DIR:-$HOME/RTEMS_PI}")
RTEMS_ROOT=$INSTALL_DIR/rtems-$RTEMS_VERSION/$RTEMS_VERSION
BSP_LIB=$RTEMS_ROOT/$RTEMS_CPU-rtems$RTEMS_VERSION/$RTEMS_BSP/lib
echo
echo "  sources      : $SRC_DIR"
echo "  RTEMS prefix : $RTEMS_ROOT   (tools in $RTEMS_ROOT/bin)"
echo "  BSP libraries: $BSP_LIB"
echo "  jobs         : $JOBS"
[ "$PREPARE_ONLY" = 1 ] && echo "  prepare only : clone + patch, no build"
[ "$DRY_RUN" = 1 ] && echo "  DRY RUN      : build commands are printed, not run"
echo
for f in "$KERNEL_PATCH" "$KERNEL_INI" "$LIBBSD_PATCH" "$TECLA_TAR"; do
    [ -f "$f" ] || die "missing $f"
done
run mkdir -p "$SRC_DIR" "$INSTALL_DIR"
export PATH=$RTEMS_ROOT/bin:$PATH
export RTEMS_VERSION RTEMS_CPU RTEMS_BSP RTEMS_ROOT

# ---- 0. host packages ------------------------------------------------------
say "host packages"
missing=""
for p in $HOST_PACKAGES; do
    dpkg -s "$p" > /dev/null 2>&1 || missing="$missing $p"
done
if [ -n "$missing" ]; then
    echo "  not installed:$missing"
    if [ "$YES" = 1 ]; then
        echo "  continuing; install them with: sudo apt install$missing"
    else
        read -r -p "  install them now with sudo apt (y/N)? " a
        [ "${a,,}" = y ] && run sudo apt install -y $missing
    fi
else
    echo "  all present"
fi

# ---- 1. cross tools (RSB) --------------------------------------------------
say "1/5 cross tools (RTEMS Source Builder)"
clone_at "$RSB_REPO" "$SRC_DIR/rsb" "$RSB_COMMIT"
if [ "$PREPARE_ONLY" = 1 ]; then
    echo "  prepare-only: skipping the tools build"
elif [ -x "$RTEMS_ROOT/bin/$RTEMS_CPU-rtems$RTEMS_VERSION-gcc" ] && [ "$FORCE" != 1 ]; then
    echo "  $RTEMS_ROOT/bin/$RTEMS_CPU-rtems$RTEMS_VERSION-gcc exists, skipping (FORCE=1 rebuilds)"
else
    echo "  this takes 30 to 60 minutes"
    (enter "$SRC_DIR/rsb/rtems" && run ../source-builder/sb-check \
        && run ../source-builder/sb-set-builder --jobs="$JOBS" --prefix="$RTEMS_ROOT" \
               "$RTEMS_VERSION/rtems-$RTEMS_CPU")
fi

# ---- 2. kernel -------------------------------------------------------------
say "2/5 RTEMS kernel, BSP $RTEMS_CPU/$RTEMS_BSP"
clone_at "$KERNEL_REPO" "$SRC_DIR/kernel" "$KERNEL_COMMIT"
apply_patch "$SRC_DIR/kernel" "$KERNEL_PATCH" bsps/arm/raspberrypi/irq/irq.c \
    'ffs(pend & BCM2835_IRQ_BASIC_SPEEDUP_USED_BITS)'
if [ ! -f "$SRC_DIR/kernel/config.ini" ]; then
    run cp "$KERNEL_INI" "$SRC_DIR/kernel/config.ini"
    echo "  config.ini installed from patches/ (POSIX API on, tests on, 256 MB RAM, FDT copy)"
elif ! cmp -s "$KERNEL_INI" "$SRC_DIR/kernel/config.ini"; then
    # A kernel tree from earlier work may be configured for another BSP
    # (seen 2026-10-17: aarch64/raspberrypi4b, which needs tools this
    # script does not build). Keep a config.ini only if it is for our BSP.
    if grep -q "^\[$RTEMS_CPU/$RTEMS_BSP\]" "$SRC_DIR/kernel/config.ini" && [ "$FORCE" != 1 ]; then
        echo "  WARNING: $SRC_DIR/kernel/config.ini differs from $KERNEL_INI;"
        echo "           keeping yours (it configures $RTEMS_CPU/$RTEMS_BSP; FORCE=1 replaces it)"
    else
        bak=config.ini.$(date +%Y%m%d-%H%M%S).bak
        echo "  $SRC_DIR/kernel/config.ini does not configure $RTEMS_CPU/$RTEMS_BSP (or FORCE=1):"
        echo "  saved as $bak, replaced by $KERNEL_INI, old build/ removed"
        run mv "$SRC_DIR/kernel/config.ini" "$SRC_DIR/kernel/$bak"
        run cp "$KERNEL_INI" "$SRC_DIR/kernel/config.ini"
        run rm -rf "$SRC_DIR/kernel/build"
    fi
fi
if [ "$PREPARE_ONLY" = 1 ]; then
    echo "  prepare-only: skipping the kernel build"
elif [ -f "$BSP_LIB/librtemsbsp.a" ] && [ "$FORCE" != 1 ]; then
    echo "  $BSP_LIB/librtemsbsp.a exists, skipping (FORCE=1 rebuilds)"
else
    [ -x "$RTEMS_ROOT/bin/$RTEMS_CPU-rtems$RTEMS_VERSION-gcc" ] || [ "$DRY_RUN" = 1 ] \
        || die "cross tools not found in $RTEMS_ROOT/bin"
    run rm -f "$SRC_DIR/kernel"/.lock-waf_*     # stale lock from an interrupted or foreign build
    (enter "$SRC_DIR/kernel" && run ./waf configure --prefix="$RTEMS_ROOT" \
        && run ./waf -j"$JOBS" && run ./waf install)
fi

# ---- 3. rtems-libbsd -------------------------------------------------------
say "3/5 rtems-libbsd (network stack, USB, LAN7800 Ethernet)"
clone_at "$LIBBSD_REPO" "$SRC_DIR/rtems-libbsd" "$LIBBSD_COMMIT"
if [ -d "$SRC_DIR/rtems-libbsd/.git" ] && [ ! -f "$SRC_DIR/rtems-libbsd/freebsd-org/sys/dev/usb/net/if_muge.c" ]; then
    echo "  fetching the freebsd-org and rtems_waf submodules (large, several hundred MB)"
    run git -C "$SRC_DIR/rtems-libbsd" submodule update --init
fi
apply_patch "$SRC_DIR/rtems-libbsd" "$LIBBSD_PATCH" rtemsbsd/include/bsp/nexus-devices.h \
    'LIBBSP_ARM_RASPBERRYPI_BSP_H'
if [ "$PREPARE_ONLY" = 1 ]; then
    echo "  prepare-only: skipping the libbsd build"
elif [ -f "$BSP_LIB/libbsd.a" ] && [ "$FORCE" != 1 ]; then
    echo "  $BSP_LIB/libbsd.a exists, skipping (FORCE=1 rebuilds)"
else
    [ -f "$BSP_LIB/librtemsbsp.a" ] || [ "$DRY_RUN" = 1 ] || die "kernel not installed in $BSP_LIB"
    run rm -f "$SRC_DIR/rtems-libbsd"/.lock-waf_*     # stale lock from an interrupted or foreign build
    (enter "$SRC_DIR/rtems-libbsd" && run ./waf configure --prefix="$RTEMS_ROOT" \
            --rtems-bsps="$RTEMS_CPU/$RTEMS_BSP" \
        && run ./waf -j"$JOBS" && run ./waf install)
fi

# ---- 4. rtems-net-services -------------------------------------------------
say "4/5 rtems-net-services (ntp, telnetd, ttcp)"
clone_at "$NETSVC_REPO" "$SRC_DIR/rtems-net-services" "$NETSVC_COMMIT"
if [ -d "$SRC_DIR/rtems-net-services/.git" ] && [ ! -f "$SRC_DIR/rtems-net-services/rtems_waf/rtems.py" ]; then
    run git -C "$SRC_DIR/rtems-net-services" submodule update --init
fi
if [ "$PREPARE_ONLY" = 1 ]; then
    echo "  prepare-only: skipping the net-services build"
elif [ -f "$BSP_LIB/libntp.a" ] && [ "$FORCE" != 1 ]; then
    echo "  $BSP_LIB/libntp.a exists, skipping (FORCE=1 rebuilds)"
else
    [ -f "$BSP_LIB/libbsd.a" ] || [ "$DRY_RUN" = 1 ] || die "libbsd not installed in $BSP_LIB"
    run rm -f "$SRC_DIR/rtems-net-services"/.lock-waf_*     # stale lock from an interrupted or foreign build
    (enter "$SRC_DIR/rtems-net-services" && run ./waf configure --prefix="$RTEMS_ROOT" \
            --rtems-bsps="$RTEMS_CPU/$RTEMS_BSP" \
        && run ./waf -j"$JOBS" && run ./waf install)
fi

# ---- 5. libtecla -----------------------------------------------------------
say "5/5 libtecla 1.6.3 (rtems-addon-packages)"
ADDON_DIR=$SRC_DIR/rtems-addon-packages
if [ ! -d "$ADDON_DIR/libtecla-1.6.3" ]; then
    run mkdir -p "$ADDON_DIR"
    run tar -xzf "$TECLA_TAR" -C "$ADDON_DIR"
    echo "  unpacked $(basename "$TECLA_TAR") into $ADDON_DIR"
fi
if [ "$PREPARE_ONLY" = 1 ]; then
    echo "  prepare-only: skipping the libtecla build"
elif [ -f "$BSP_LIB/libtecla.a" ] && [ "$FORCE" != 1 ]; then
    echo "  $BSP_LIB/libtecla.a exists, skipping (FORCE=1 rebuilds)"
else
    export RTEMS_MAKEFILE_PATH=$RTEMS_ROOT/$RTEMS_CPU-rtems$RTEMS_VERSION/$RTEMS_BSP
    [ -f "$RTEMS_MAKEFILE_PATH/Makefile.inc" ] || [ "$DRY_RUN" = 1 ] \
        || die "$RTEMS_MAKEFILE_PATH/Makefile.inc missing (kernel not installed?)"
    (enter "$ADDON_DIR/libtecla-1.6.3" && run make -f ../RTEMS_Makefiles/Makefile.libtecla)
fi

# ---- summary ---------------------------------------------------------------
say "result"
if [ "$PREPARE_ONLY" = 1 ] || [ "$DRY_RUN" = 1 ]; then
    echo "  nothing built in this mode"
else
    for lib in librtemsbsp.a librtemscpu.a libbsd.a libntp.a libttcp.a libtecla.a libtecla_r.a; do
        if [ -f "$BSP_LIB/$lib" ]; then printf '  ok       %s\n' "$lib"; else printf '  MISSING  %s\n' "$lib"; fi
    done
fi
cat <<EOF

Next: EPICS base with scripts/build_epics_rpi3.sh (answer "$INSTALL_DIR" to
its RTEMS question). Put the tools on your PATH for later work:
  export PATH=$RTEMS_ROOT/bin:\$PATH
EOF
