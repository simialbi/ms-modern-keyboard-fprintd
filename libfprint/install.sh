#!/bin/sh
#
# Build libfprint with the fpcusb driver and plug it into the running
# system, so that fprintd (and with it GNOME, KDE and PAM) can use the
# fingerprint sensor of the Microsoft Modern Keyboard with Fingerprint ID.
#
# The distribution package is never touched: the patched library is
# installed into its own prefix (/usr/local by default) and fprintd is
# pointed at it with a systemd drop-in.  --uninstall undoes everything.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

PREFIX=/usr/local
SRCDIR=
REF=
BUILD_ONLY=0
UNINSTALL=0

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
DROPIN=/etc/systemd/system/fprintd.service.d/50-local-libfprint.conf
UPSTREAM=https://gitlab.freedesktop.org/libfprint/libfprint.git

usage()
{
	cat <<EOF
usage: $0 [options]

  --prefix DIR    where the patched libfprint is installed (default $PREFIX)
  --src DIR       libfprint checkout to use (default \$ROOT/build/libfprint)
  --ref REF       git ref to build (default: the tag matching the installed
                  libfprint, else the default branch)
  --build-only    build, do not install anything
  --uninstall     remove the patched library and the fprintd drop-in
  -h, --help      this text

Everything up to the installation runs as your normal user; the install
step uses sudo.
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
	--prefix) PREFIX=$2; shift 2 ;;
	--prefix=*) PREFIX=${1#*=}; shift ;;
	--src) SRCDIR=$2; shift 2 ;;
	--src=*) SRCDIR=${1#*=}; shift ;;
	--ref) REF=$2; shift 2 ;;
	--ref=*) REF=${1#*=}; shift ;;
	--build-only) BUILD_ONLY=1; shift ;;
	--uninstall) UNINSTALL=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

: "${SRCDIR:=$ROOT/build/libfprint}"
BUILDDIR=$SRCDIR/_build

if [ "$(id -u)" = 0 ]; then
	SUDO=
else
	SUDO=sudo
fi

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ #
# uninstall                                                           #
# ------------------------------------------------------------------ #

if [ "$UNINSTALL" = 1 ]; then
	say "removing the fprintd drop-in"
	$SUDO rm -f "$DROPIN"
	$SUDO rmdir --ignore-fail-on-non-empty /etc/systemd/system/fprintd.service.d 2>/dev/null || true

	if [ -r "$BUILDDIR/meson-logs/install-log.txt" ]; then
		say "removing files installed from $BUILDDIR"
		sed -n '/^\//p' "$BUILDDIR/meson-logs/install-log.txt" |
			while read -r f; do $SUDO rm -f "$f"; done
	else
		say "removing $PREFIX/lib/libfprint-2.so*"
		$SUDO rm -f "$PREFIX"/lib/libfprint-2.so*
	fi

	$SUDO rm -f /etc/udev/rules.d/60-fpc-keyboard.rules
	$SUDO ldconfig || true
	$SUDO udevadm control --reload 2>/dev/null || true
	$SUDO systemctl daemon-reload 2>/dev/null || true
	$SUDO systemctl try-restart fprintd.service 2>/dev/null || true
	say "done - fprintd uses the distribution libfprint again"
	exit 0
fi

# ------------------------------------------------------------------ #
# prerequisites                                                       #
# ------------------------------------------------------------------ #

for tool in git meson ninja pkg-config cc; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done

MKENUMS=$(pkg-config --variable=glib_mkenums glib-2.0 2>/dev/null || true)
if [ -z "$MKENUMS" ] || [ ! -x "$MKENUMS" ]; then
	REAL_MKENUMS=$(command -v glib-mkenums 2>/dev/null || true)
	[ -n "$REAL_MKENUMS" ] ||
		die "glib-mkenums is missing - install the glib development tools
       (Arch: glib2-devel, Debian/Ubuntu: libglib2.0-dev, Fedora: glib2-devel)"

	# glib-2.0.pc points at a path that does not exist (some distributions
	# split the tools off); hand meson a .pc file with the real paths.
	say "fixing up the glib-2.0 tool paths for meson"
	PCDIR=$ROOT/build/pkgconfig
	mkdir -p "$PCDIR"
	sed -e "s|^glib_mkenums=.*|glib_mkenums=$REAL_MKENUMS|" \
	    -e "s|^glib_genmarshal=.*|glib_genmarshal=$(command -v glib-genmarshal || echo "$REAL_MKENUMS")|" \
	    "$(pkg-config --variable=pcfiledir glib-2.0)/glib-2.0.pc" >"$PCDIR/glib-2.0.pc"
	PKG_CONFIG_PATH=$PCDIR${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
	export PKG_CONFIG_PATH
fi

for mod in glib-2.0 gio-2.0 gusb; do
	pkg-config --exists "$mod" ||
		die "the $mod development files are missing"
done

# ------------------------------------------------------------------ #
# fetch the matching libfprint sources                                #
# ------------------------------------------------------------------ #

if [ -z "$REF" ]; then
	installed=$(pkg-config --modversion libfprint-2 2>/dev/null || true)
	[ -n "$installed" ] && REF=v$installed
fi

if [ -d "$SRCDIR/.git" ]; then
	say "updating $SRCDIR"
	git -C "$SRCDIR" fetch --quiet --tags origin
else
	say "cloning libfprint into $SRCDIR"
	mkdir -p "$(dirname "$SRCDIR")"
	git clone --quiet "$UPSTREAM" "$SRCDIR"
fi

DEFAULT_BRANCH=$(git -C "$SRCDIR" symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null | cut -d/ -f2-)
: "${DEFAULT_BRANCH:=master}"

# The release tag is preferred because it matches the installed library
# exactly; if it cannot be configured (old tags break with new meson
# releases) the same version from the default branch is used instead.
if [ -n "$REF" ] && git -C "$SRCDIR" rev-parse --verify --quiet "$REF" >/dev/null; then
	CANDIDATES="$REF $DEFAULT_BRANCH"
else
	[ -n "$REF" ] && say "no ref $REF upstream, using $DEFAULT_BRANCH"
	CANDIDATES=$DEFAULT_BRANCH
fi

# ------------------------------------------------------------------ #
# add the driver, register it in the build system and configure       #
# ------------------------------------------------------------------ #

add_driver()
{
	cp "$HERE/fpcusb.c" "$HERE/fpcusb.h" "$SRCDIR/libfprint/drivers/"

	python3 - "$SRCDIR" <<'PY'
import pathlib, sys

src = pathlib.Path(sys.argv[1])

# 1. top level meson.build: driver metadata (no 'optional' -> default set)
p = src / 'meson.build'
s = p.read_text()
if "'fpcusb'" not in s:
    anchor = "    # SPI driver"
    if anchor not in s:
        sys.exit("meson.build: cannot find the drivers_info anchor")
    s = s.replace(anchor, "    'fpcusb': {},\n\n" + anchor, 1)
    p.write_text(s)
    print("  registered in meson.build")

# 2. libfprint/meson.build: source file of the driver
q = src / 'libfprint' / 'meson.build'
t = q.read_text()
if "'fpcusb'" not in t:
    anchor = "helper_sources = {"
    if anchor not in t:
        sys.exit("libfprint/meson.build: cannot find the driver_sources anchor")
    t = t.replace("}\n\n" + anchor,
                  "    'fpcusb' : files('drivers/fpcusb.c'),\n}\n\n" + anchor, 1)
    q.write_text(t)
    print("  registered in libfprint/meson.build")
PY
}

configured=
for ref in $CANDIDATES; do
	say "preparing libfprint $ref"
	git -C "$SRCDIR" -c advice.detachedHead=false \
		checkout --quiet --force "$ref"
	if git -C "$SRCDIR" symbolic-ref --quiet HEAD >/dev/null 2>&1; then
		git -C "$SRCDIR" pull --quiet --ff-only || true
	fi

	add_driver

	rm -rf "$BUILDDIR"
	if meson setup "$BUILDDIR" "$SRCDIR" --prefix="$PREFIX" \
		-Dintrospection=false -Ddoc=false -Dgtk-examples=false \
		-Dinstalled-tests=false -Dudev_hwdb=disabled \
		>"$SRCDIR/meson-setup.log" 2>&1; then
		configured=$ref
		break
	fi
	say "$ref cannot be configured with this meson, trying the next ref"
	tail -5 "$SRCDIR/meson-setup.log"
done

[ -n "$configured" ] ||
	die "meson setup failed, see $SRCDIR/meson-setup.log"
say "configured libfprint $configured in $BUILDDIR"

say "building"
ninja -C "$BUILDDIR"

if [ "$BUILD_ONLY" = 1 ]; then
	say "built $BUILDDIR/libfprint/libfprint-2.so.2 (not installed)"
	exit 0
fi

# ------------------------------------------------------------------ #
# install                                                             #
# ------------------------------------------------------------------ #

say "installing into $PREFIX (needs root)"
$SUDO meson install -C "$BUILDDIR" >/dev/null
$SUDO ldconfig || true

say "installing the udev rule"
$SUDO install -m 0644 "$ROOT/udev/60-fpc-keyboard.rules" /etc/udev/rules.d/
$SUDO udevadm control --reload
$SUDO udevadm trigger --subsystem-match=usb --attr-match=idVendor=045e || true

say "pointing fprintd at $PREFIX/lib"
$SUDO install -d /etc/systemd/system/fprintd.service.d
printf '# Added by %s - use the libfprint built with the fpcusb driver.\n[Service]\nEnvironment=LD_LIBRARY_PATH=%s/lib\n' \
	"$0" "$PREFIX" | $SUDO tee "$DROPIN" >/dev/null
$SUDO systemctl daemon-reload
$SUDO systemctl stop fprintd.service 2>/dev/null || true

cat <<EOF

$(say "done")
Check that fprintd now sees the keyboard:

    fprintd-list "\$USER"        # "no prints" is fine, an error is not
    fprintd-enroll              # touch the fingerprint key when asked

To undo everything:

    $0 --uninstall
EOF
