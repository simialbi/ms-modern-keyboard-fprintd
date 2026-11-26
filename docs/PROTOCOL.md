# USB protocol of the Microsoft Modern Keyboard with Fingerprint ID

Everything below was recovered by reverse engineering the 32 bit Windows
driver shipped in `KeyboardwithFingerprintIDInstaller_32bit_DEU_1.0.162.exe`
and cross-checked against the descriptors of a real device.

## 1. What is inside the installer

The `.exe` is an InstallShield/MSI bundle; unpacking it
(`7z x`, then `msiexec`/`7z` on `driverinstaller.msi`) yields
`setup/files/driver/`, which contains several INF packages.  The one that
matters is `keyboardfingerprintsensor/`:

| file | role |
| --- | --- |
| `keyboardfingerprintsensor.inf` | binds `USB\VID_045E&PID_0815&MI_01` (and `&PID_0816&MI_01`) to WinUSB + UMDF2 |
| `x86/keyboardfingerprintdriver.dll` | the UMDF2 **WBDI** driver (Windows Biometric Driver Interface) — the file that was decompiled |
| `x86/keyboardfingerprintengine.dll` | the FPC matching/template engine |
| `keyboardfingerprintsensor.cat` | catalogue signature |

Sibling packages cover the Bluetooth variant of the same sensor
(`keyboardbluetoothfingerprintsensor/`), a filter driver
(`keyboardfingerprintintegration.sys`) and firmware update/pairing
helpers; none of them are needed for USB operation.

Verified excerpts from `keyboardfingerprintsensor.inf`:

```
Class              = Biometric
ClassGuid          = {53D29EF7-377C-4D14-864B-EB3A85769359}
DriverVer          = 12/21/2016,2.19.5.3
"Keyboard Fingerprint Sensor" = FpcUsb_Install, USB\VID_045E&PID_0815&MI_01
"Keyboard Fingerprint Sensor" = FpcUsb_Install, USB\VID_045E&PID_0816&MI_01
UmdfDispatcher     = WinUsb
UmdfService        = FpcMachUsb,UMDFFpcMachUsb_Install
DriverCLSID        = {7004B9B7-C377-4654-934C-5CB9F1E7C42B}
ServiceBinary      = %12%\UMDF\KeyboardFingerprintDriver.dll
HKR,WinBio\Configurations\0,SensorMode,0x10001,1        ; 1 = Basic
HKR,WinBio\Configurations\0,EngineAdapterBinary,,"KeyboardFingerprintEngine.dll"
```

`SensorMode = 1` ("Basic") is the important one: the sensor is *not* a
match-on-chip device, it only streams raw images and Windows does the
matching in software — exactly what libfprint does on Linux.

So the Windows side is **not** a kernel driver at all: it is a user mode
driver that talks to the device through WinUSB.  That is exactly what the
Linux port in this repository does with libusb — no kernel module needed.

## 2. USB topology

`lsusb -v -d 045e:0815` on the real keyboard:

```
bDeviceClass 0, bNumInterfaces 2, bcdDevice 1.13

Interface 0  class 3 (HID, boot keyboard)
    EP 0x81 Interrupt IN  64
    EP 0x02 Interrupt OUT 64

Interface 1  class 255/255/255  (vendor specific)  <-- fingerprint sensor
    EP 0x83 Bulk IN   64
    EP 0x04 Bulk OUT  64
```

The sensor is interface **1** (`MI_01`), the same interface the INF binds
to.  All traffic is plain bulk on `0x04` (host → device) and `0x83`
(device → host), 64 byte packets, little endian throughout.

## 3. Framing

### 3.1 Request (host → device), 8 bytes

| offset | size | field |
| --- | --- | --- |
| 0 | 2 | opcode |
| 2 | 2 | transaction id (random, constant for a session) |
| 4 | 2 | sequence number (incremented per command) |
| 6 | 2 | CRC-16 over bytes 0..5 |

### 3.2 Response (device → host), 10 + *payload* bytes

| offset | size | field |
| --- | --- | --- |
| 0 | 2 | `opcode \| 0x1000` |
| 2 | 2 | status (0 = ok) |
| 4 | N | payload |
| N+4 | 2 | transaction id (must equal the one sent) |
| N+6 | 2 | sequence number |
| N+8 | 2 | CRC-16 over all preceding bytes |

The driver rejects a frame when the CRC mismatches, when the transaction
id differs, or when the sequence number is not greater than the previous
one; it then re-synchronises with `HELLO`.

### 3.3 Checksum

CRC-16/CCITT-FALSE: polynomial `0x1021`, init `0xFFFF`, no reflection,
no final xor (check value for `"123456789"` is `0x29B1`).  The Windows
driver uses the table free Sarwate-style variant:

```c
x    = (crc >> 8) ^ byte;
x   ^= x >> 4;
crc  = (crc << 8) ^ (x << 12) ^ (x << 5) ^ x;
```

`tests/test_frames.c` verifies the recovered code against a textbook
bit-by-bit implementation.

## 4. Opcodes

| op | name | request | response |
| --- | --- | --- | --- |
| `0x0001` | `HELLO` / identify | 8 bytes | 12 bytes, payload = 2 byte hardware id |
| `0x0005` | `STATUS` | 2 bytes (**no** id/seq/crc) | 4 bytes: `0x1005`, value |
| `0x0007` | `GET_IMAGE` | 8 bytes | `len + 12` bytes, see below |
| `0x0008` | `ABORT` | 8 bytes | 10 bytes, no payload |
| `0x000F` | `ERROR` | sent **by the device**, host answers with `0x100F` | — |

`0x0005` is the only command that is sent as a bare 2 byte opcode without
the id/sequence/CRC trailer.

### 4.1 HELLO and sensor identification

The 2 byte payload of the `0x1001` answer is the sensor hardware id.  The
Windows driver maps it to the sensor model, the image geometry and a
calibration/firmware string:

| hardware id | mask | sensor | image (W × H) | version string |
| --- | --- | --- | --- | --- |
| `0x0200` | `0xFFF0` | FPC1020 | 192 × 192 | `1020.0.0.0.12` |
| `0x0210` | `0xFFF0` | FPC1021 | 160 × 160 | `1021.11.5.12.0.0.1` |
| `0x0101` | `0xFF0F` | **FPC1022** (confirmed on hardware) | 112 × 88 | `1022.0.0.0.0.124.72.1` |
| `0x1400` | `0xFFF0` | FPC1140 | 56 × 192 | `FPC1140.1.0.22` |
| `0x1500` | `0xFFF0` | FPC1150 | 80 × 208 | `FPC1150.0.0.0.0.B1` |

Anything unknown falls back to the FPC1020 geometry.  Resolution is
**508 dpi** for all of them.  Pixels are 8 bit greyscale, no header,
row major.

The keyboard this was developed against answers `0x0121`, which matches
the FPC1022 rule (`hwid & 0xFF0F == 0x0101`) and delivers a 112 × 88
image — the captured ridge pattern is coherent at that stride, which
confirms both the width/height assignment and the reassembly.

### 4.2 Image transfer

```
host  -> 0x0007 id seq crc                     (8 bytes)
   ... ~300 ms later, *if* a finger was on the sensor already ...
dev   -> packet 1 (64 bytes): 0x1007 status len <58 bytes of payload>
dev   -> packet n (<=64 bytes): <2 byte tag> <up to 62 bytes of payload>
   ...
```

The logical frame is `len + 12` bytes long and is reassembled by
concatenating the first packet verbatim and every following packet
**with its first two bytes removed**.  `len` (offset 4) equals
`width * height`; the pixel data starts at offset 6 of the reassembled
frame and is followed by the usual id/seq/CRC trailer.

The last read is sized `min(remaining + 2, 64)`, which is why the final
packet is usually short.

**The firmware does not wait for a finger** (measured on the FPC1022 of
the keyboard, 2025-11):

* if a finger rests on the sensor when `GET_IMAGE` arrives, the first
  packet appears after roughly 300 ms,
* if the sensor is free, the command is simply *never* answered — not
  with an error, not later when the user touches.  A transfer that was
  submitted before the touch stays pending forever, and the next
  `GET_IMAGE` then returns the image of the *previous* touch.

Waiting for a finger is therefore a **poll**: `ABORT` + `GET_IMAGE` with
a deadline of about 600 ms, over and over (`FPC_CAPTURE_ATTEMPT_MS`).
Shorter attempts do not work - 350 ms missed every single touch in a
test of ten.  A touch has to overlap the moment a request is sent, so
brief taps are lost; about 80 % of touches held for ~2 s are caught.

While a finger keeps resting, *every* attempt returns an image, which is
how `fpc_wait_finger_off()` detects that the finger has been lifted: two
attempts in a row without an image.

**Known unsolved problem — the imaging engine wedges.**  After a few
hundred poll cycles the sensor stops delivering images altogether: it
still answers `HELLO` (`fpc-capture info` works), but every `GET_IMAGE`
runs into its deadline, for any finger.  `libusb_reset_device()` does
*not* recover it; only unplugging the keyboard does, and even then the
next capture may be the last one for a while.  Whatever the Windows
driver does to keep the engine alive (a calibration command among the
undocumented opcodes `0x0002`-`0x0006`?) is still unknown.

#### How the drivers poll

Both implementations use the same per-attempt deadline, but they differ
in *when* they poll:

* `fpc-capture` polls as long as the user asked it to (`loop` runs until
  Ctrl-C).
* The libfprint driver polls **only while fprintd is running an
  operation** — there is no permanent capture loop any more.  Each
  enrol stage and each verification arms its own deadline (30 s), and
  between two enrol stages the driver waits for the lift with a 5 s
  deadline.  Idle means no USB traffic at all, which keeps the sensor
  far away from the few hundred cycles that wedge it.

#### Recovery sequence of the libfprint driver

A command transfer (`ABORT`/`GET_IMAGE`) that fails is the symptom of the
wedge; a *timeout while reading the image* is not, it only means "no
finger yet".  After eight consecutive command failures in one operation
the driver, in this order:

1. re-runs the `HELLO` handshake (up to 20 attempts, 200 ms apart) and
   resumes the capture if the sensor answers again,
2. otherwise issues a USB port reset (`g_usb_device_reset()`),
3. and finally fails the operation with `FP_DEVICE_ERROR_PROTO` and the
   message that the keyboard has to be unplugged and plugged back in.

Step 2 is known not to help on this firmware; it is kept because it is
the last thing the host can try before the user has to act, and the
explicit error is what turns a hang into something actionable.

### 4.3 Cancelling

A pending `GET_IMAGE` is cancelled by sending `0x0008`.  The device then
answers the *outstanding* read with a `0x1008` frame instead of image
data, which is how both the Windows driver and this port break out of the
blocking wait.  The Windows driver always sends an `ABORT` before a new
`GET_IMAGE` to make sure the firmware is idle.

Two different aborts are therefore needed, and mixing them up breaks the
poll loop:

* the **internal** abort that every poll attempt sends before its
  `GET_IMAGE` (`abort_cmd()` in `src/fpc_usb.c`, state
  `CAPTURE_SEND_ABORT` in the driver).  Its answer is read and discarded;
  the input pipe is only drained when that handshake itself fails.
* the **caller facing** abort that interrupts a running operation
  (`fpc_abort()`, the `cancel` callback of the driver).  fprintd uses it
  for its own timeout and for Ctrl-C; a `0x1008` arriving in place of
  image data is treated as "no image this round", so a cancelled verify
  leaves the device immediately usable.

### 4.4 Error notifications

The device may push an unsolicited `0x000F` frame (for example when the
finger was removed too early).  The host acknowledges it with `0x100F`
carrying the current id and the next sequence number; the transfer is
then restarted.

## 5. What is *not* implemented here

The Windows stack also contains:

* `KeyboardFingerprintEngine.dll` — the FPC matching algorithm, template
  creation, and enrolment quality feedback.  On Linux this job used to be
  left to libfprint's own NBIS/bozorth3 pipeline.

  NBIS is **not** a free replacement though, and measurement says it
  cannot be made one: it was designed for full sized 500 dpi prints, and
  bozorth3 scores a comparison with **0** as soon as one of the two
  prints carries fewer than ten minutiae.  Ten real presses of one finger
  on this sensor (`tools/fpc_bench.c`) yield **0 to 2 minutiae each** and
  **every** pairwise score is 0, with or without contrast normalisation
  and at 1×, 2× and 3× enlargement.  A 5.6 × 4.4 mm window simply does
  not contain ten ridge endings.

  Matching such a sensor needs the approach the vendor algorithm uses:
  comparing the *images* (normalised cross correlation over a search of
  shifts and small rotations) against several stored frames per finger,
  not minutiae.  Measured on this hardware, two presses hitting the same
  spot correlate at 0.97 while a different finger stays at or below 0.33
  — but two presses of the same finger that land on different spots can
  drop to 0.17, so one template per finger is not enough.

  That is what `src/fpc_match.c` now does, and the driver is a plain
  `FpDevice` because of it: the template is a set of up to 16
  preprocessed frames in a versioned `"FPCT"` blob, and the comparison
  happens in the driver, not in libfprint.  The algorithm is far simpler
  than the vendor's (no mosaicking, no template update after a successful
  verification), so the Windows engine is still the better matcher.
* A "secure"/match-on-chip path and firmware update commands.  Neither is
  used by the normal enrol/verify flow of this sensor: the firmware only
  streams raw images, the matching happens on the host in both worlds.

## 6. Notes for anyone extending this

* The sensor does **not** provide a separate "finger present" interrupt,
  and `STATUS` (`0x0005`) is stalled by this firmware, so it cannot be
  used for finger detection either.  Waiting for a finger means polling
  `GET_IMAGE`, see 4.2 — a transfer submitted *before* the touch is never
  completed, so the libfprint driver must not wait on a single one.
* The HID keyboard interface (interface 0) is untouched — the keyboard
  keeps working with the normal `usbhid` driver while the sensor is in use.
* Image polarity: ridges come out dark on this sensor.  That mattered for
  the old image-device driver (`FPI_IMAGE_COLORS_INVERTED`); correlation
  does not care, because both sides of the comparison come from the same
  sensor and go through the same preprocessing.  `fpc-capture` writes the
  raw bytes, so its PGMs and the frames inside a template have identical
  polarity — which is what makes the offline evaluation comparable to the
  driver.
