#!/bin/bash
#
# build_epics_rpi3.sh - EPICS base 7.0.10 for RTEMS 6 on the Raspberry Pi 3B+
#
# Clones EPICS base (branch 7.0 at the commit the patch was made for),
# fetches the PVAccess submodules, applies
# patches/epics-base-R7.0.10-raspberrypi.patch, sets the site configuration
# (RTEMS prefix, cross target RTEMS-raspberrypi2) and builds Base for the
# host and for RTEMS-raspberrypi2. Needs the RTEMS tools, kernel, libbsd,
# net-services and libtecla from scripts/build_rtems_rpi3.sh.
#
# Asks for:
#   EPICS directory  (default /usr/local/epics/R7.0.10/base)  where Base goes;
#                    a directory under /usr/local is created with sudo and
#                    handed to the current user, so the build itself and later
#                    rebuilds need no sudo
#   RTEMS install directory (default ~/RTEMS_PI)  as given to build_rtems_rpi3.sh;
#                    the prefix is <dir>/rtems-6/6
#
# Usage: scripts/build_epics_rpi3.sh [-y] [--prepare-only] [-h]
#   -y              take the defaults (or EPICS_DIR / RTEMS_INSTALL_DIR from the
#                   environment) without asking; also updates configure/RELEASE
#                   of this application if EPICS_BASE differs
#   --prepare-only  clone, submodules, patch and site settings, no make
#   -h              this text
# Environment: JOBS (default nproc), DRY_RUN=1 prints the build commands.
#
# Re-running is safe: clone, patch and settings are skipped when done; make
# only rebuilds what changed.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
PATCH_DIR=$PROJECT_DIR/patches

EPICS_REPO=https://github.com/epics-base/epics-base.git
EPICS_BRANCH=7.0
EPICS_COMMIT=5b23dcb46652e5f359af1f72477987e21868abd5   # R7.0.10 + 51 commits, what the patch is made for
EPICS_PATCH=$PATCH_DIR/epics-base-R7.0.10-raspberrypi.patch
PVA_MODULES="modules/pvData modules/pvAccess modules/normativeTypes modules/pvaClient modules/pvDatabase modules/pva2pva"
TARGET_ARCH=RTEMS-raspberrypi2
RTEMS_VERSION=6
HOST_PACKAGES="build-essential g++ perl libreadline-dev"

JOBS=${JOBS:-$(nproc)}
DRY_RUN=${DRY_RUN:-0}
YES=0
PREPARE_ONLY=0
for arg in "$@"; do
    case $arg in
        -y) YES=1 ;;
        --prepare-only) PREPARE_ONLY=1 ;;
        -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (try -h)"; exit 2 ;;
    esac
done

say()  { printf '\n==== %s\n' "$*"; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY_RUN" = 1 ]; then printf '  (dry) %s\n' "$*"; else "$@"; fi; }
enter() {   # cd for a build step; in a dry run a not yet cloned directory is tolerated
    if [ -d "$1" ]; then cd "$1"; elif [ "$DRY_RUN" = 1 ]; then printf '  (dry) in %s:\n' "$1"; else die "missing directory $1"; fi
}
ask() {
    local answer
    if [ "$YES" = 1 ]; then answer=$2; else read -r -e -p "$1 [$2]: " answer; answer=${answer:-$2}; fi
    printf '%s' "${answer/#\~/$HOME}"
}

# ---- directories -----------------------------------------------------------
echo "EPICS base 7.0.10 for RTEMS 6 / Raspberry Pi 3B+"
EPICS_DIR=$(ask "Directory for EPICS base" "${EPICS_DIR:-/usr/local/epics/R7.0.10/base}")
RTEMS_INSTALL_DIR=$(ask "RTEMS install directory (as given to build_rtems_rpi3.sh)" "${RTEMS_INSTALL_DIR:-$HOME/RTEMS_PI}")
RTEMS_ROOT=$RTEMS_INSTALL_DIR/rtems-$RTEMS_VERSION/$RTEMS_VERSION
BSP_LIB=$RTEMS_ROOT/arm-rtems$RTEMS_VERSION/raspberrypi2/lib
echo
echo "  EPICS base   : $EPICS_DIR"
echo "  RTEMS prefix : $RTEMS_ROOT"
echo "  target       : $TARGET_ARCH, jobs $JOBS"
[ "$PREPARE_ONLY" = 1 ] && echo "  prepare only : no make"
[ "$DRY_RUN" = 1 ] && echo "  DRY RUN      : build commands are printed, not run"
echo
[ -f "$EPICS_PATCH" ] || die "missing $EPICS_PATCH"

say "RTEMS installation"
if [ "$DRY_RUN" != 1 ]; then
    [ -x "$RTEMS_ROOT/bin/arm-rtems$RTEMS_VERSION-gcc" ] || die "no arm-rtems$RTEMS_VERSION-gcc in $RTEMS_ROOT/bin; run scripts/build_rtems_rpi3.sh first"
    for lib in librtemsbsp.a libbsd.a libntp.a libtecla.a; do
        [ -f "$BSP_LIB/$lib" ] || die "$BSP_LIB/$lib missing; run scripts/build_rtems_rpi3.sh first"
    done
fi
echo "  ok: tools and BSP libraries found"
export PATH=$RTEMS_ROOT/bin:$PATH

say "host packages"
missing=""
for p in $HOST_PACKAGES; do dpkg -s "$p" > /dev/null 2>&1 || missing="$missing $p"; done
if [ -n "$missing" ]; then
    echo "  not installed:$missing"
    if [ "$YES" = 1 ]; then echo "  continuing; install with: sudo apt install$missing"
    else read -r -p "  install them now with sudo apt (y/N)? " a; [ "${a,,}" = y ] && run sudo apt install -y $missing; fi
else
    echo "  all present"
fi

# ---- clone -----------------------------------------------------------------
say "EPICS base sources"
if [ ! -d "$EPICS_DIR/.git" ]; then
    parent=$(dirname "$EPICS_DIR")
    if [ ! -d "$parent" ] || [ ! -w "$parent" ]; then
        echo "  $parent is not writable: creating $EPICS_DIR with sudo and handing it to $USER"
        run sudo mkdir -p "$EPICS_DIR"
        run sudo chown "$USER" "$EPICS_DIR"
    fi
    if [ -d "$EPICS_DIR" ] && [ -n "$(ls -A "$EPICS_DIR" 2>/dev/null)" ]; then
        die "$EPICS_DIR exists, is not empty and is not a git checkout"
    fi
    run git clone -b "$EPICS_BRANCH" "$EPICS_REPO" "$EPICS_DIR"
fi
if [ -d "$EPICS_DIR/.git" ]; then
    head=$(git -C "$EPICS_DIR" rev-parse HEAD)
    if [ "$head" != "$EPICS_COMMIT" ]; then
        if [ -n "$(git -C "$EPICS_DIR" status --short --untracked-files=no)" ]; then
            echo "  $EPICS_DIR is at $head with local changes; expected $EPICS_COMMIT, leaving it"
        else
            git -C "$EPICS_DIR" cat-file -e "$EPICS_COMMIT^{commit}" 2>/dev/null || run git -C "$EPICS_DIR" fetch origin
            run git -C "$EPICS_DIR" checkout --detach "$EPICS_COMMIT"
        fi
    else
        echo "  already at $EPICS_COMMIT"
    fi
    say "PVAccess submodules (empty after a plain clone)"
    run git -C "$EPICS_DIR" submodule update --init $PVA_MODULES
fi

# ---- patch -----------------------------------------------------------------
say "Raspberry Pi patch"
if [ -f "$EPICS_DIR/configure/os/RULES.RTEMS-Image" ]; then
    echo "  already applied (configure/os/RULES.RTEMS-Image present)"
elif [ -d "$EPICS_DIR/configure" ]; then
    (cd "$EPICS_DIR" && patch -p1 --dry-run < "$EPICS_PATCH" > /dev/null) || die "patch does not apply cleanly"
    run bash -c "cd '$EPICS_DIR' && patch -p1 < '$EPICS_PATCH'"
fi

# ---- site settings ---------------------------------------------------------
say "site settings"
SITE_RTEMS=$EPICS_DIR/configure/os/CONFIG_SITE.Common.RTEMS
SITE=$EPICS_DIR/configure/CONFIG_SITE
if [ -f "$SITE_RTEMS" ]; then
    if grep -q "^RTEMS_BASE *= *$RTEMS_ROOT\$" "$SITE_RTEMS"; then
        echo "  RTEMS_BASE already $RTEMS_ROOT"
    else
        run sed -i -E "s|^RTEMS_BASE *=.*|RTEMS_BASE = $RTEMS_ROOT|" "$SITE_RTEMS"
        echo "  RTEMS_BASE = $RTEMS_ROOT  ($SITE_RTEMS)"
    fi
    grep -q "^RTEMS_VERSION *= *$RTEMS_VERSION" "$SITE_RTEMS" || run sed -i -E "s|^RTEMS_VERSION *=.*|RTEMS_VERSION = $RTEMS_VERSION|" "$SITE_RTEMS"
    grep -q "^COMMANDLINE_LIBRARY *= *LIBTECLA" "$SITE_RTEMS" || echo "  WARNING: COMMANDLINE_LIBRARY is not LIBTECLA in $SITE_RTEMS"
fi
if [ -f "$SITE" ]; then
    if grep -q "^CROSS_COMPILER_TARGET_ARCHS *=.*$TARGET_ARCH" "$SITE"; then
        echo "  CROSS_COMPILER_TARGET_ARCHS already contains $TARGET_ARCH"
    else
        run sed -i -E "s|^CROSS_COMPILER_TARGET_ARCHS *=.*|CROSS_COMPILER_TARGET_ARCHS = $TARGET_ARCH|" "$SITE"
        echo "  CROSS_COMPILER_TARGET_ARCHS = $TARGET_ARCH  ($SITE)"
    fi
fi

# ---- build -----------------------------------------------------------------
say "build (host + $TARGET_ARCH)"
if [ "$PREPARE_ONLY" = 1 ]; then
    echo "  prepare-only: run later:  cd $EPICS_DIR && PATH=$RTEMS_ROOT/bin:\$PATH make -j$JOBS"
else
    echo "  about 15 to 30 minutes; the cross tools must stay on PATH for every later make"
    (enter "$EPICS_DIR" && run make -j"$JOBS")
fi

# ---- verify ----------------------------------------------------------------
say "result"
if [ "$PREPARE_ONLY" != 1 ] && [ "$DRY_RUN" != 1 ]; then
    for f in lib/$TARGET_ARCH/libCom.a lib/$TARGET_ARCH/libdbCore.a lib/$TARGET_ARCH/libpvAccess.a \
             lib/$TARGET_ARCH/libqsrv.a dbd/PVAServerRegister.dbd dbd/qsrv.dbd cfg/CONFIG_QSRV_VERSION; do
        if [ -f "$EPICS_DIR/$f" ]; then printf '  ok       %s\n' "$f"; else printf '  MISSING  %s\n' "$f"; fi
    done
fi

# ---- this application's configure/RELEASE ---------------------------------
RELEASE=$PROJECT_DIR/configure/RELEASE
if [ -f "$RELEASE" ]; then
    current=$(sed -n -E 's|^EPICS_BASE *= *||p' "$RELEASE" | tail -1)
    if [ "$current" = "$EPICS_DIR" ]; then
        echo "  configure/RELEASE of the IOC already points at $EPICS_DIR"
    else
        echo "  configure/RELEASE of the IOC has EPICS_BASE = $current"
        if [ "$YES" = 1 ]; then a=y; else read -r -p "  set it to $EPICS_DIR (y/N)? " a; fi
        if [ "${a,,}" = y ]; then
            run sed -i -E "s|^EPICS_BASE *=.*|EPICS_BASE = $EPICS_DIR|" "$RELEASE"
            echo "  updated $RELEASE"
        fi
    fi
fi
cat <<EOF

Next: build the IOC
  cd $PROJECT_DIR
  export PATH=$RTEMS_ROOT/bin:\$PATH
  make            # kernel7.img (rpi3.img), rpi3_plain.img and epics.tar.gz in bin/$TARGET_ARCH
EOF
