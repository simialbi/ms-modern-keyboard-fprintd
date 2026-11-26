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
| `0x0005` | `STATUS` | 8 bytes | 10 bytes, no payload — never sent by the Windows driver, see §6 |
| `0x0007` | `GET_IMAGE` | 8 bytes | `len + 12` bytes, see below |
| `0x0008` | `ABORT` | 8 bytes | 10 bytes, no payload |
| `0x000F` | `ERROR` | sent **by the device**, host answers with `0x100F` | — |

Every request uses the same 8 byte framing; `0x0005` was initially
believed to be a bare 2 byte opcode, but the firmware answers it only in
the regular form.

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
* if the sensor is free, the firmware answers the request with a
  `0x1007` frame carrying **status `0x0003`** and no payload — "no image
  for you".  The answer arrives within a few tens of milliseconds.
* a request is *not* remembered: a transfer left waiting for 35 s was
  not completed by a later touch (verified 2025-11-26 on a healthy
  engine).

Waiting for a finger is therefore a **poll**: `GET_IMAGE`, read the
answer, repeat after 15 ms (`FPC_CAPTURE_GAP_MS`); the read deadline of
600 ms (`FPC_CAPTURE_ATTEMPT_MS`) only guards against a firmware that
stopped answering.  In practice a poll cycle costs about 43 ms, and 8 of
13 one-second presses were caught in a 1841-poll run.

**No `ABORT` is sent between two attempts.**  Re-reading the Windows
driver (2025-11) showed that it cancels only its *own* pending read
(`WinUsb`-level cancel, invisible on the wire), sleeps 15 ms and sends
the next `GET_IMAGE`; opcode `0x0008` goes out only for a genuine
cancellation.  The earlier poll loop here sent one `ABORT` per attempt —
thousands per session — and that had two effects: the `0x1008` answers
hid the `0x0003` frames (which is why the firmware looked silent and why
a 600 ms deadline per attempt seemed necessary), and the imaging engine
stopped answering for good after a few hundred cycles.  Without the
abort, 1841 consecutive polls ran without a single error.

While a finger keeps resting, *every* attempt returns an image, which is
how a lift is detected: the answer switches back to status `0x0003`
(`fpc_wait_finger_off()` requires two such attempts in a row, the driver
stops at the first).

**The imaging engine can wedge.**  It then stops delivering images
altogether: it still answers `HELLO` (`fpc-capture info` works, status
0, correct hwid), but every `GET_IMAGE` runs into its deadline without
any answer at all, for any finger and also for a request that is left
waiting without any polling.
`libusb_reset_device()`, `libusb_clear_halt()` on both endpoints and
re-selecting the alternate setting all succeed and change nothing, and
on 2025-11-26 even a physical replug did not bring the engine back.
What *did* revive it is a **full logical re-enumeration** of the device
from the host:

```sh
echo -n 1-2.3.3 | sudo tee /sys/bus/usb/drivers/usb/unbind
sleep 5
echo -n 1-2.3.3 | sudo tee /sys/bus/usb/drivers/usb/bind
```

(the port id comes from `lsusb -t`; give it at least five seconds to
come back).  Since removing the per-attempt `ABORT` the wedge has not
been reproduced.

There is **no hidden vendor command** that could be missing: the Windows
driver only ever sends `HELLO`, `GET_IMAGE`, `ABORT` and the `0x100F`
acknowledgement (six CRC call sites in
`KeyboardFingerprintDriver.dll`), has no WinUSB control transfers, no
pipe reset and no recovery path of its own.  The opcodes `0x0002`-`0x0006`
are never used by it, and `0x0005` is the only one this firmware answers
at all (with a bare 10-byte acknowledgement, see §6).  The one
difference that remained was the per-attempt `ABORT` described above,
which is why it was removed.

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

A single fruitless poll means nothing — the user simply has not touched
the sensor yet.  The driver therefore treats two situations as a wedge:

* eight consecutive failures of a `GET_IMAGE` command transfer, and
* thirty consecutive polls without an image **after the engine has
  already delivered at least one image during this operation** — that is
  exactly the "it worked and then stopped" signature, and it is what
  breaks an enrolment in the middle.

In both cases the driver, in this order:

1. re-runs the `HELLO` handshake (up to 20 attempts, 200 ms apart) and
   resumes the capture if the sensor answers again,
2. and after three such attempts without a single image in between fails
   the operation with `FP_DEVICE_ERROR_PROTO` and the message that the
   device has to be re-bound or the keyboard replugged.

A successful capture resets the attempt counter, so a user who takes a
long break between two presses never runs into step 2.

The driver deliberately does **not** call `g_usb_device_reset()`: it
never revived the engine in any measurement, and it makes the kernel
re-enumerate the keyboard, which fprintd reports as
`enroll-disconnected` in the middle of a run.

#### Keeping the pipe in sync

A read that ran into its deadline while the firmware answered late
leaves that answer in the input pipe, and every following frame is then
read one answer too early - observed as `bad crc` and, in one run, as a
stale `0x1001` HELLO reply in place of an image header.  Whenever a
frame cannot be used (deadline, unexpected opcode, short or truncated
packet, failing CRC) the driver therefore reads the pipe empty
(`CAPTURE_DRAIN`, 60 ms per read) before sending the next `GET_IMAGE`,
and counts the round as an empty poll instead of failing the operation.
`src/fpc_usb.c` does the same with `drain()`.

Everything else that merely produced no usable image — a poll deadline, a
`0x1008` abort frame in place of the image, an unsolicited `0x000F` — is
a *retry*, not a failure: during an enrolment it is reported with
`fpi_device_enroll_progress()` and the same stage is attempted again, so
the run continues.  fprintd shows this as a non-final
`enroll-retry-scan`.  Completing the enrolment with such an error would
abort the whole run, which is what fprintd prints as a final
`enroll-retry-scan` after the stages passed so far.

### 4.3 Cancelling

A pending `GET_IMAGE` is cancelled by sending `0x0008`.  The device then
answers the *outstanding* read with a `0x1008` frame instead of image
data, which is how both the Windows driver and this port break out of the
blocking wait.

`0x0008` is reserved for exactly that: interrupting a running operation
(`fpc_abort()`, the `cancel` callback of the driver).  fprintd uses it
for its own timeout and for Ctrl-C.  A `0x1008` arriving in place of
image data is treated as "no image this round", so a cancelled verify
leaves the device immediately usable.

The poll loop itself must **not** use it (see 4.2); the input pipe is
only drained when a frame could not be parsed at all.

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

* The sensor does **not** provide a separate "finger present" interrupt.
  `STATUS` (`0x0005`) does answer when it is sent as a normal 8-byte
  frame and nothing else is in flight, but the answer is a bare
  acknowledgement that does not change while a finger is on the sensor —
  and the Windows driver never sends it.  Waiting for a finger means polling
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
