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
   ... device blocks until a finger touches the sensor ...
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

### 4.3 Cancelling

A pending `GET_IMAGE` is cancelled by sending `0x0008`.  The device then
answers the *outstanding* read with a `0x1008` frame instead of image
data, which is how both the Windows driver and this port break out of the
blocking wait.  The Windows driver always sends an `ABORT` before a new
`GET_IMAGE` to make sure the firmware is idle.

### 4.4 Error notifications

The device may push an unsolicited `0x000F` frame (for example when the
finger was removed too early).  The host acknowledges it with `0x100F`
carrying the current id and the next sequence number; the transfer is
then restarted.

## 5. What is *not* implemented here

The Windows stack also contains:

* `KeyboardFingerprintEngine.dll` — the FPC matching algorithm, template
  creation, and enrolment quality feedback.  On Linux this job is done by
  libfprint's own NBIS/bozorth3 pipeline, so it is not needed.
* A "secure"/match-on-chip path and firmware update commands.  Neither is
  used by the normal enrol/verify flow of this sensor: the firmware only
  streams raw images, the matching happens on the host in both worlds.

## 6. Notes for anyone extending this

* The sensor does **not** provide a separate "finger present" interrupt.
  Waiting for a finger is simply a `GET_IMAGE` whose answer is delayed,
  which is why the libfprint driver submits that transfer without a
  timeout and relies on `ABORT` for cancellation.
* The HID keyboard interface (interface 0) is untouched — the keyboard
  keeps working with the normal `usbhid` driver while the sensor is in use.
* Image polarity: ridges come out dark on this sensor, so the libfprint
  driver sets `FPI_IMAGE_COLORS_INVERTED`.  If your images look like a
  photographic negative, flip that flag.
