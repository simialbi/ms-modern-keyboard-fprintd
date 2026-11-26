# Linux driver for the Microsoft Modern Keyboard with Fingerprint ID

[![CI](https://github.com/simialbi/ms-modern-keyboard-fprintd/actions/workflows/ci.yml/badge.svg)](https://github.com/simialbi/ms-modern-keyboard-fprintd/actions/workflows/ci.yml)

A Unix/Linux driver for the fingerprint sensor built into the **Microsoft
Modern Keyboard with Fingerprint ID** (USB `045e:0815`, also `045e:0816`),
reverse engineered from the Windows driver in
`KeyboardwithFingerprintIDInstaller_32bit_DEU_1.0.162.exe`.

The sensor is a **Fingerprint Cards (FPC)** chip sitting behind a
vendor-specific USB interface.  On Windows it is driven by a *user mode*
driver on top of WinUSB — there is no kernel driver to port.  The Linux
equivalent is therefore a userspace driver on top of libusb, plus a
driver for libfprint so that `fprintd`, GNOME/KDE settings, PAM login and
`sudo` can use it.

```
.
├── src/                 standalone userspace driver + CLI (libusb)
│   ├── fpc_usb.h/.c     the driver itself (reusable library)
│   └── fpc_capture.c    "fpc-capture" command line tool
├── libfprint/           driver for the libfprint stack (fprintd, PAM, GNOME)
│   ├── fpcusb.c/.h
│   └── install.sh       rebuilds libfprint with the driver and wires up fprintd
├── udev/                access rule for the device node
├── tests/               offline self tests of the recovered framing
├── .github/workflows/   CI: build, tests, sanitizers, static analysis
└── docs/PROTOCOL.md     full protocol documentation
```

## Status

| part | state |
| --- | --- |
| Protocol recovered from the Windows binary | done, documented in `docs/PROTOCOL.md` |
| USB topology confirmed on real hardware | done (interface 1, bulk `0x83` IN / `0x04` OUT) |
| Userspace driver + CLI | builds clean, offline self tests pass |
| CRC implementation | verified against the CRC-16/CCITT-FALSE reference vector |
| Sensor identification on hardware | **working** — FPC1022, hwid `0x0121`, 112 × 88 |
| Live image capture on hardware | **working** — complete frame, CRC/id/sequence all valid, recognisable ridges |
| libfprint driver | **working** — built into libfprint 1.94.100, `fprintd-enroll` completes on real hardware |

Verified against a real keyboard (`045e:0815`, bcdDevice 1.13):

```
fpc: -> cmd 0x0001 id 0x2f9f seq 0x0616
fpc: <- rsp 0x1001 status 0x0000 seq 0x0617
fpc: sensor FPC1022 hwid 0x0121 112x88
fpc: -> cmd 0x0008 id 0x2f9f seq 0x0618      (abort a stale transfer)
fpc: <- rsp 0x1008 status 0x0000 seq 0x0619
fpc: -> cmd 0x0007 id 0x2f9f seq 0x061a      (get image)
fpc: <- rsp 0x1007 status 0x0000 seq 0x061b
wrote /tmp/finger.pgm
```

The resulting 112 × 88 image contains continuous, coherent ridge lines, so
the frame reassembly, the row stride and the image geometry are all
correct.

The libfprint driver was verified the same way, on CachyOS with
libfprint 1.94.100 and fprintd 1.94.5:

```
$ fprintd-list $USER
User simialbi has no fingers enrolled for Microsoft Modern Keyboard with Fingerprint ID (FPC).
$ fprintd-enroll
Enrolling right-index-finger finger.
Enroll result: enroll-stage-passed
...
Enroll result: enroll-completed
```

If a second reader is present (a laptop sensor, for example), `fprintd`
lists both; pick this one with `fprintd-enroll -d /net/reactivated/Fprint/Device/1`
or whichever device id it reports for the keyboard.

## Build

Requires `libusb-1.0` development headers and a C compiler.

```sh
sudo apt install libusb-1.0-0-dev build-essential   # Debian/Ubuntu
sudo pacman -S libusb base-devel                    # Arch

make            # -> build/fpc-capture
make test       # offline checks, no hardware needed
```

Extra compiler/linker flags can be injected without losing the mandatory
ones, e.g. for a sanitizer build:

```sh
make test EXTRA_CFLAGS="-fsanitize=address,undefined" \
          EXTRA_LDLIBS="-fsanitize=address,undefined"
```

## Continuous integration

`.github/workflows/ci.yml` runs on every push, on every pull request and
weekly.  No fingerprint hardware is available on GitHub runners, so CI
covers everything that works offline:

| job | what it does |
| --- | --- |
| `build` | builds with **gcc and clang** on Ubuntu 24.04 and 22.04, runs `make test`, checks that the CLI exits cleanly without hardware, and verifies `make install` / `make uninstall` into a staging dir |
| `sanitizers` | rebuilds the tests with AddressSanitizer + UndefinedBehaviorSanitizer |
| `static-analysis` | `cppcheck` (informational) and the clang static analyzer via `scan-build --status-bugs` (blocking) |
| `udev-rule` | `udevadm verify` on `udev/60-fpc-keyboard.rules` |
| `libfprint-driver` | clones upstream libfprint, drops `libfprint/fpcusb.c` in and builds it — non-blocking, because upstream changes frequently |

The binary built on Ubuntu 24.04 is uploaded as a workflow artifact.

To make CI required before merging, enable branch protection on `main`
and mark the `build (...)` checks as required.  Anything that actually
touches the sensor still has to be tested locally — the pull request
template asks for the `fpc-capture -v info` output.

## Device access

The vendor interface is root-only by default.  Install the udev rule once:

```sh
sudo cp udev/60-fpc-keyboard.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
```

Then unplug/replug the keyboard.  (`make install` does the same and also
installs the binary into `/usr/local/bin`.)

Without the rule you can still test with `sudo ./build/fpc-capture ...`.

## Usage

```sh
# which sensor is in this keyboard?
fpc-capture info

# wait for a finger and save the image
fpc-capture capture finger.pgm

# keep capturing until Ctrl-C
fpc-capture loop scan-

# with a full protocol trace / hex dump
fpc-capture -v info
fpc-capture -vv capture finger.pgm
```

Example output (this keyboard):

```
sensor : FPC1022 (hwid 0x0121)
image  : 112 x 88, 8 bit grey, 508 dpi
version: 1022.0.0.0.0.124.72.1
```

Images are written as binary PGM (`P5`), viewable with any image viewer,
GIMP, `feh`, `display`, …

The keyboard itself keeps working normally the whole time: only the
vendor interface is claimed, the HID keyboard interface stays with the
kernel.

## Using it with fprintd / GNOME / PAM

`libfprint/fpcusb.c` is a driver for libfprint ≥ 1.94 (the stack behind
`fprintd`, GNOME Settings and `pam_fprintd`).  Since libfprint links its
drivers statically, the library has to be rebuilt with the driver in it.
`libfprint/install.sh` does all of that:

```sh
./libfprint/install.sh          # build and install, asks for sudo
./libfprint/install.sh --help   # options
```

It

1. clones libfprint and checks out the version that is installed on the
   system — or the newest release if libfprint is not installed, as on a
   CI runner — falling back to the default branch when that ref cannot
   be configured or built with the local meson,
2. copies `fpcusb.c`/`fpcusb.h` into `libfprint/drivers/` and registers
   the driver in the `drivers_info` dictionary of `meson.build` and in
   `driver_sources` of `libfprint/meson.build`, aborting if upstream
   renamed them instead of producing a library without the driver,
3. builds and installs into `/usr/local` — the distribution package is
   left untouched,
4. installs `udev/60-fpc-keyboard.rules`,
5. adds `/etc/systemd/system/fprintd.service.d/50-local-libfprint.conf`
   so that **only** `fprintd` loads the patched library
   (`LD_LIBRARY_PATH=/usr/local/lib`).

Build requirements beyond the ones above: `git`, `meson`, `ninja`, the
glib development tools and the libfprint dependencies:

```sh
sudo pacman -S git meson glib2-devel libgusb pixman nss openssl libgudev   # Arch
sudo apt install git meson ninja-build libglib2.0-dev libgusb-dev \
     libpixman-1-dev libnss3-dev libgudev-1.0-dev                          # Debian
```

Afterwards:

```sh
fprintd-list "$USER"   # should report the device ("no prints" is fine)
fprintd-enroll         # touch the fingerprint key when asked
fprintd-verify
```

Enable it for login with `pam-auth-update` (Debian/Ubuntu) or by adding
`auth sufficient pam_fprintd.so` to the relevant PAM stack (Arch:
`/etc/pam.d/system-local-login`, `sudo`, …).

Everything can be undone with:

```sh
./libfprint/install.sh --uninstall
```

For a quick check without installing, build only
(`./libfprint/install.sh --build-only`) and run libfprint's own
`examples/img-capture` from `build/libfprint/_build`.

The driver reports:

* scan type **press** (touch sensor, not a swipe sensor),
* geometry taken from the sensor at open time,
* `FPI_IMAGE_COLORS_INVERTED`, because the raw image has dark ridges.

## How it works (short version)

```
host -> 0x0001 ...            "hello"
dev  -> 0x1001 ok <hwid>      identifies the FPC chip -> image geometry
host -> 0x0007 ...            "give me an image"
          (device stays silent until a finger touches the sensor)
dev  -> 0x1007 ok <len> <192*192 bytes of 8 bit grey> <id> <seq> <crc>
host -> 0x0008 ...            cancels a pending capture
```

Every frame is protected by a CRC-16/CCITT-FALSE and carries a
transaction id and a sequence number.  The image is split over 64 byte
bulk packets, where every packet after the first starts with a two byte
tag that has to be stripped.  The full details — all opcodes, the sensor
id table, the error handling — are in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Troubleshooting

| symptom | cause |
| --- | --- |
| `open: USB I/O error` | no permission for `/dev/bus/usb/...`; install the udev rule or use `sudo` |
| `open: interface busy` | another process (e.g. `fprintd`) holds the interface — stop it first |
| `hello: protocol error` | the firmware is out of sync; unplug/replug the keyboard |
| capture never returns | that is normal — it waits for a finger; press the fingerprint key |
| image looks like a negative | flip `FPI_IMAGE_COLORS_INVERTED` in the libfprint driver |

Useful debugging: `fpc-capture -vv`, and `sudo modprobe usbmon` +
`wireshark` on `usbmon1` to see the raw frames.

## Legal

This is clean-room-style interoperability work: the protocol was analysed
to make the hardware usable on Linux.  No Microsoft or FPC code is
included or redistributed here; the installer itself is not part of the
produced driver.  Interoperability reverse engineering of this kind is
explicitly permitted in the EU (Directive 2009/24/EC Art. 6) and in many
other jurisdictions, but you are responsible for your own local law.

The code in `src/`, `libfprint/`, `tests/` and `udev/` is licensed under
the LGPL-2.1-or-later, matching libfprint so the driver can be upstreamed.
