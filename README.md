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
├── tools/               fpc_bench.c: scores captured images like libfprint does
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
| libfprint driver | **working** — a plain `FpDevice` with its own enrol/verify/identify; `fprintd-enroll` completes all stages and `fprintd-verify` reports `verify-match` on hardware |
| Matching quality | **works, thinly calibrated** — multi-frame correlation instead of NBIS; the genuine finger verifies on hardware, but two fingers of one hand can reach 0.744, so the threshold is 0.78 and genuine presses may need a repeat, see [Matching quality](#matching-quality) |
| Capture reliability | **much improved** — the per-attempt `ABORT` was the problem; without it the firmware answers every request (status `0x0003` = no finger) and 1841 consecutive polls ran without an error, see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) §4.2 |

Verified against a real keyboard (`045e:0815`, bcdDevice 1.13):

```
fpc: -> cmd 0x0001 id 0x2f9f seq 0x0616
fpc: <- rsp 0x1001 status 0x0000 seq 0x0617
fpc: sensor FPC1022 hwid 0x0121 112x88
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
make bench      # -> build/fpc-bench, needs a built libfprint checkout
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
| `build` | builds with **gcc and clang** on Ubuntu 24.04 and 22.04, runs `make test` (protocol + matcher) and the `fpc-match-eval` smoke test, checks that the CLI exits cleanly without hardware, and verifies `make install` / `make uninstall` into a staging dir |
| `sanitizers` | rebuilds the tests with AddressSanitizer + UndefinedBehaviorSanitizer |
| `static-analysis` | `cppcheck` (informational) and the clang static analyzer via `scan-build --status-bugs` (blocking) |
| `udev-rule` | `udevadm verify` on `udev/60-fpc-keyboard.rules` |
| `libfprint-driver` | clones upstream libfprint, drops `libfprint/fpcusb.c` plus the shared matcher in, builds it and re-checks that both sources compile warning-free — non-blocking, because upstream changes frequently |

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
2. copies `fpcusb.c`/`fpcusb.h` **and** the shared matcher
   `src/fpc_match.c|h` (as `fpcusb_match.c|h`) into `libfprint/drivers/`
   and registers both sources in the `drivers_info` dictionary of
   `meson.build` and in `driver_sources` of `libfprint/meson.build`,
   aborting if upstream renamed them instead of producing a library
   without the driver,
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
fprintd-delete "$USER"   # mandatory after a driver update, see below
fprintd-list "$USER"     # should report the device ("no prints" is fine)
fprintd-enroll           # sixteen presses, see the enrolment advice below
fprintd-verify
```

**Always delete the old prints after installing or updating this
driver.**  The driver stores its own template format; prints enrolled
with an earlier version (or with the built-in sensor) are rejected with
"please delete it and enrol again".

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
* sixteen enrolment stages, see below,
* its **own** matching: the driver is a plain `FpDevice`, not an
  `FpImageDevice`, because libfprint's minutiae matcher cannot work on a
  sensor this small (see below).

### Matching quality

The sensor only images **5.6 × 4.4 mm** (112 × 88 pixels at 508 dpi),
which is tiny for libfprint's NBIS pipeline, and that had a consequence
that is easy to misread:

* enrolment never compares anything — every scan is turned into a set of
  minutiae and appended to the template, so it always succeeds,
* verification runs bozorth3, and bozorth3 returns the score **0**
  without even looking at the data when one of the two prints has fewer
  than ten minutiae (`MIN_COMPUTABLE_BOZORTH_MINUTIAE`).

So a sensor that produces too few minutiae enrols perfectly and then
reports `verify-no-match` forever, no matter how low the threshold is
set.  **That is exactly what this sensor does**, and the measurement
below says it cannot be fixed by preprocessing: ten real presses of one
finger yield 0 to 2 minutiae each and every bozorth3 score is 0, at 1×,
2× and 3× enlargement, with and without normalisation.  That is why the
driver no longer uses that pipeline at all.

#### Why NBIS cannot be tuned into working here (measured on real presses)

The numbers further down were obtained from crops of libfprint's own
reference print, which is a *full sized* scan reduced to this geometry —
it flatters the sensor.  Ten presses captured from the real keyboard
(one finger, `fpc-capture loop`) look like this:

```
variant                             min.med >=10 min. score.med     >=thr
scale 1x raw        all minutiae          2      0/10         0    0/45
scale 2x normalized all minutiae          2      0/10         0    0/45
scale 3x normalized all minutiae          0      0/10         0    0/45
```

Not a single frame reaches the ten minutiae bozorth3 needs, so **every**
score is 0 — including the comparison of a frame with itself.  No
threshold, no enlargement and no contrast trick changes that: a
5.6 × 4.4 mm window does not contain ten ridge endings.

#### What the driver does instead

The driver matches this sensor the way the vendor algorithm does:
it compares the **images**.

* Every frame is preprocessed by `src/fpc_match.c`: the histogram is
  stretched over the full grey range and a local mean is subtracted, so
  that pressure and dry skin do not dominate the comparison.
* A finger's template is a **set of up to 16 frames**, stored in a
  versioned blob (`"FPCT"`) inside the print; enrolment collects sixteen
  presses and rejects a press that covers too little of the sensor or
  that is a near duplicate (score ≥ 0.95) of an already stored one,
  which is why it asks you to move the finger around.
* A verification captures one frame and scores it against **every** frame
  of the template by zero mean normalised cross correlation over a
  translation and a small rotation search; the best score decides.  A
  frame with too little contact is reported as `verify-retry-scan`, not
  as a no-match.
* A press that stays below the threshold does **not** end the
  verification: libfprint allows a driver exactly one result per verify,
  so the driver itself waits for the next press and uses the best of up
  to **three** presses (`FPCUSB_MATCH_ATTEMPTS`).  Just press again when
  nothing seems to happen.
* The very same file is compiled into the offline tools and the unit
  tests (`make test`, `make eval`), so what is measured is what ships.

How to enrol so that verification actually works:

* give each press a clear lift and hold it for about a second — brief
  taps are not answered by the firmware at all,
* **move the finger between presses**: roll it slightly left/right, up
  and down, and vary the angle.  Sixteen presses of exactly the same spot
  produce a template that only recognises that spot, and the driver will
  ask you to lift and retry when it sees the same image twice,
* use the part of the fingertip you actually put on the key when logging
  in.

A press the firmware does not answer **never ends the enrolment**: it is
reported as `enroll-retry-scan` and the driver keeps waiting for the next
one, so you can take as long as you like.  Only if the imaging engine
stops answering *although it answered before* does the driver intervene —
it re-runs the `HELLO` handshake and, after three futile attempts, asks
you to re-bind or replug the keyboard (see
[`docs/PROTOCOL.md`](docs/PROTOCOL.md) §4.2).  It deliberately does *not*
reset the USB port: that made the kernel re-enumerate the keyboard and
fprintd reported `enroll-disconnected` in the middle of a run.

This is necessary because the window is so small: same finger/same spot
scores ≈ 0.96, but the same finger on a *different* spot only ≈ 0.17 —
and, measured later on two index fingers of the same hand, a **different
finger still reaches 0.744**.  A single stored frame can therefore never
be enough, and the threshold has to sit above what two different fingers
of the same hand can reach.

**The template is a set of fingerprint images.**  `fprintd` stores it
under `/var/lib/fprint/<user>/fpcusb/` with mode 0600, owned by root —
but anyone who can read those files has pictures of your fingertip, not
just a feature vector.

#### Measured calibration

The calibration data are 26 real captures of **two index fingers of the
same hand**: 15 left and 11 right presses, giving 160 same-finger and
165 cross-finger comparisons.  Scored pairwise with the shipped matcher:

| pair | median | maximum |
| --- | ---: | ---: |
| same finger | 0.618 | 0.960 |
| different finger | 0.173 | **0.744** |

That maximum is the whole problem.  Against a 16-frame template the best
score is 0.59 … 0.96 for the enrolled finger and up to 0.744 for the
other one, so the FAR/FRR curve of the evaluator looks like this:

| threshold | FAR | FRR |
| ---: | ---: | ---: |
| 0.45 | 18/26 | 1/26 |
| 0.60 | 7/26 | 2/26 |
| 0.65 | 3/26 | 3/26 (EER 0.115) |
| **0.78** | **0/26** | **4/26** |
| 0.85 | 0/26 | 6/26 |

The shipped defaults are therefore a **16-frame template** (one per
enrolment stage), a 37 × 29 pixel translation window, a ±4° rotation
search and a threshold of **0.78**
(`FPC_MATCH_THRESHOLD_DEFAULT` in `src/fpc_match.h`).

The earlier default of 0.45 came from an end-to-end round against a
single, far less similar finger (impostor presses 0.117 … 0.389, genuine
0.412 … 0.854) and was **wrong**: with it, the other index finger
verified successfully on the hardware, and `fprintd` refused to enrol it
at all with `enroll-duplicate`, because before every enrolment fprintd
identifies the new press against all stored prints.  Both symptoms are
the same false accept.

A rejected press is normal on a sensor this small — the driver waits for
up to three presses per verification before it answers.  The score of
every press is in the journal, which makes it easy to see whether it was
close or nowhere near.

Narrowing the search window, changing the rotation range or the number
of frames does not improve the separation (EER 0.115 … 0.27 across the
whole sweep), and neither does judging the *sharpness* of the
correlation peak instead of its height
(`fpc-match-eval --score sharp|mix`, same EER).  What would help is more
coverage of the fingertip — the genuine score only drops below 0.78 when
the press lands on skin the template has never seen.

If you want it stricter or more forgiving, pass the override to the
installer - it ends up in fprintd's systemd drop-in:

```sh
sudo FPCUSB_THRESHOLD=0.60 ./libfprint/install.sh
sudo systemctl restart fprintd
```

These numbers come from **one person and two fingers**.  Zero observed
false accepts at 0.78 does *not* establish a production FAR — the
measured 0.744 between two fingers of one hand shows how little room
there is.  Treat the sensor as a convenience factor, not as a security
grade one, until someone runs a broader study.

You can repeat the measurement yourself, without root and without
fprintd — the evaluator uses the very same matcher as the driver:

```sh
make eval

./build/fpc-capture loop /tmp/left-    # ~15 presses of ONE finger
./build/fpc-capture loop /tmp/right-   # ~12 presses of ANOTHER finger

./build/fpc-match-eval \
    --finger 'left:/tmp/left-*.pgm' --finger 'right:/tmp/right-*.pgm'
./build/fpc-match-eval --sweep \
    --finger 'left:/tmp/left-*.pgm' --finger 'right:/tmp/right-*.pgm'
```

The first invocation prints the FAR/FRR curve, the EER and both score
histograms for the shipped configuration; `--sweep` compares search
windows, rotation ranges and template sizes.  Every print is scored
against a template built from the *other* presses of the same finger, so
no frame ever matches itself.

#### Debugging the matcher at runtime

These environment variables are read once when the device is opened, so
set them for the whole `fprintd` service (`systemctl edit fprintd`), not
for `fprintd-verify`:

| variable | effect |
| --- | --- |
| `FPCUSB_THRESHOLD` | decision threshold, `(0, 1]`, default 0.78 |
| `FPCUSB_MAX_SHIFT` | translation search radius in pixels, both axes |
| `FPCUSB_ROT_STEPS` | rotation steps per side (0 disables the rotation search) |
| `FPCUSB_DUMP_DIR` | write every captured frame there as PGM — **these are fingerprint images** |

The score of every verification is logged unconditionally, as a warning
so that it survives the default log level:

```sh
journalctl -u fprintd -b --no-pager | grep "verify score"
verify score 0.823 (threshold 0.780, best of 16 frames: #0, coverage 0.97)
```

`fpc-bench` is kept as the documented counter example: it runs the NBIS
extraction and bozorth3 matching that *cannot* work here
(`make bench`, needs `./libfprint/install.sh --build-only` first).  Its
numbers below were taken on *synthetic* input, which is why they looked
far better than the hardware turned out to be.  Four overlapping
112 × 88 crops — the exact geometry of this sensor — of libfprint's own
reference print (`examples/prints/loop-right.png`), plus one crop of a
different area as an impostor:

```
variant                             min.med >=10 min. score.med     >=thr(20)
scale 1x raw        all minutiae          5      0/4          0    0/6
scale 1x normalized all minutiae         10      2/4          0    0/6
scale 2x raw        all minutiae         15      4/4         29    6/6
scale 2x normalized all minutiae         14      4/4         32    6/6
scale 2x normalized partial              13      4/4         29    4/6
scale 3x normalized all minutiae         14      4/4         17    1/6

genuine pairs 23 … 38      impostor 3
```

At 1× not a single image reaches ten minutiae, so every comparison
scores 0 — that is the `verify-no-match` that cannot be fixed with a
threshold.  2× clears the hurdle here only because this is a crop of a
full sized scan; on real presses from the keyboard even 2× and 3× stay
at 0 to 2 minutiae (table above), which is what made the correlation
matcher necessary.

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
| capture never returns | it polls for a finger; press the fingerprint key and **hold it for ~1 s**, very brief taps can still fall between two requests |
| no image at all any more, `info` still works | the imaging engine is wedged — un- and rebind the device (`/sys/bus/usb/drivers/usb/{unbind,bind}`, wait 5 s); a USB reset, a pipe reset, re-selecting the interface and even a replug have all been seen to fail (see [`docs/PROTOCOL.md`](docs/PROTOCOL.md) §4.2) |
| `verify-no-match` for your own finger | all three presses of this verification hit an area the enrolment never covered — the scores are in the journal (`journalctl -u fprintd \| grep "verify score"`); re-enrol covering more of the fingertip. Lowering `FPCUSB_THRESHOLD` below 0.75 lets *other* fingers in, see [Matching quality](#matching-quality) |
| `verify-match` for a finger you did not enrol | the threshold is too low for this sensor; 0.78 is the measured minimum. Check `FPCUSB_THRESHOLD` in fprintd's drop-in and re-install |
| `enroll-duplicate` for a second finger | fprintd compares every new finger against the already enrolled ones before enrolling, and a too low threshold makes a different finger look like a duplicate — same cause as a false `verify-match` |
| `verify-retry-scan` | too little skin on the sensor, press flatter and hold ~2 s |
| `enroll-retry-scan` | nothing arrived within 30 s — press the key and hold it for ~2 s; the stage is simply repeated |
| `enroll-finger-not-centered` / `enroll-remove-and-retry` | the press was empty or identical to a previous one; lift the finger and place it slightly differently |
| enrolment ends with "the sensor stopped answering" | the imaging engine wedged mid-enrolment and three handshake attempts failed; un- and rebind the device or replug the keyboard, then enrol again |
| `enroll-disconnected` although the keyboard is still there | fprintd reports every device error that way; look at `journalctl -u fprintd`. "Device disabled to prevent overheating" was libfprint's temperature model and is fixed (`temp_hot_seconds = -1`) |
| `verify`/`enroll` fails with "please delete it and enrol again" | the stored template predates this driver or came from another sensor — `fprintd-delete "$USER"` and enrol again |

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
