/*
 * fpc_usb.h - userspace driver for the Fingerprint Cards (FPC) USB sensor
 *             used in the Microsoft Modern Keyboard with Fingerprint ID
 *             (USB 045e:0815 / 045e:0816, vendor interface MI_01).
 *
 * The protocol implemented here was reverse engineered from the Windows
 * UMDF driver KeyboardFingerprintDriver.dll shipped in
 * KeyboardwithFingerprintIDInstaller_*.exe.  See docs/PROTOCOL.md.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef FPC_USB_H
#define FPC_USB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* USB identification                                                  */
/* ------------------------------------------------------------------ */

#define FPC_USB_VID_MICROSOFT   0x045e
#define FPC_USB_PID_KEYBOARD_A  0x0815  /* Modern Keyboard with Fingerprint ID */
#define FPC_USB_PID_KEYBOARD_B  0x0816  /* same keyboard, second HW revision   */

/* The fingerprint function is the second USB interface (MI_01) of the
 * composite keyboard device.  It is a vendor specific interface with one
 * bulk IN and one bulk OUT endpoint (wMaxPacketSize 64). */
#define FPC_USB_INTERFACE       1
#define FPC_USB_PACKET          64

/* ------------------------------------------------------------------ */
/* Wire protocol                                                       */
/* ------------------------------------------------------------------ */

/* Request opcodes (host -> device).  A response carries op | FPC_RESP. */
#define FPC_RESP                0x1000

#define FPC_CMD_HELLO           0x0001  /* identify sensor, resync sequence   */
#define FPC_CMD_STATUS          0x0005  /* short 2-byte query, 4-byte answer  */
#define FPC_CMD_GET_IMAGE       0x0007  /* stream one fingerprint image       */
#define FPC_CMD_ABORT           0x0008  /* cancel a pending GET_IMAGE         */
#define FPC_CMD_ERROR           0x000f  /* device -> host error notification  */

/* Status word of a 0x1007 answer that carries no image.  Seen when a
 * second GET_IMAGE reaches the firmware while one is still pending. */
#define FPC_IMG_STATUS_NO_IMAGE 0x0003

/* Sensor hardware ids as reported by the HELLO response. */
#define FPC_HWID_1020           0x0200  /* mask 0xfff0 */
#define FPC_HWID_1021           0x0210  /* mask 0xfff0 */
#define FPC_HWID_1140           0x1400  /* mask 0xfff0 */
#define FPC_HWID_1150           0x1500  /* mask 0xfff0 */
#define FPC_HWID_1022           0x0101  /* mask 0xff0f */

/* Image resolution of every FPC sensor supported by the Windows driver. */
#define FPC_IMAGE_DPI           508

/* The firmware answers GET_IMAGE only when a finger is on the sensor at
 * the moment the command arrives, and needs about 300 ms to scan it.  A
 * capture is therefore a poll: one GET_IMAGE attempt of this length
 * after the other, with no ABORT in between. */
#define FPC_CAPTURE_ATTEMPT_MS  600

/* Pause between two attempts.  The Windows driver sleeps 15 ms after
 * cancelling its own pending read and before the next GET_IMAGE; a
 * longer gap loses quick touches. */
#define FPC_CAPTURE_GAP_MS      15

/* How many attempts in a row have to come back empty before the finger
 * counts as lifted. */
#define FPC_FINGER_OFF_ATTEMPTS 2

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

enum {
	FPC_OK            =  0,
	FPC_ERR_IO        = -1,   /* libusb transfer failed                  */
	FPC_ERR_TIMEOUT   = -2,   /* no answer within the requested time     */
	FPC_ERR_PROTO     = -3,   /* malformed frame / bad CRC / bad opcode  */
	FPC_ERR_ABORTED   = -4,   /* device sent an ABORT (0x1008) frame     */
	FPC_ERR_DEVICE    = -5,   /* device sent an ERROR (0x000f) frame     */
	FPC_ERR_NODEV     = -6,   /* no supported device found               */
	FPC_ERR_BUSY      = -7,   /* interface claimed by another driver     */
	FPC_ERR_NOMEM     = -8,
	FPC_ERR_PARAM     = -9,
};

const char *fpc_strerror(int err);

/* ------------------------------------------------------------------ */
/* Device handle                                                       */
/* ------------------------------------------------------------------ */

typedef struct fpc_dev fpc_dev;

/* Sensor description filled in by fpc_hello(). */
typedef struct {
	uint16_t hwid;          /* raw hardware id word from the HELLO reply */
	unsigned model;         /* 1020, 1021, 1022, 1140, 1150 or 0         */
	unsigned width;         /* image width  in pixels                    */
	unsigned height;        /* image height in pixels                    */
	char     fw[64];        /* firmware/sensor string, e.g. "1020.0..."  */
} fpc_sensor_info;

/* Open the first supported device.  `verbose` enables a protocol trace on
 * stderr (0 = quiet, 1 = frames, 2 = hex dumps). */
int  fpc_open(fpc_dev **out, int verbose);

/* Open a specific device by USB bus/address (0/0 = any). */
int  fpc_open_bus(fpc_dev **out, int verbose, uint8_t bus, uint8_t addr);

void fpc_close(fpc_dev *dev);

/* Identify the sensor and resynchronise the sequence counters.  Must be
 * called once after fpc_open() and after any fatal protocol error. */
int  fpc_hello(fpc_dev *dev, fpc_sensor_info *info);

/* Short status query (opcode 5).  Returns the raw 16-bit answer.
 *
 * The FPC1022 firmware of the Modern Keyboard stalls this command, so it
 * is kept for other sensors only - do not use it to detect a finger. */
int  fpc_status(fpc_dev *dev, uint16_t *value);

/* Cancel a pending capture (sends ABORT).  Safe to call from another
 * thread.  Do not use it to re-arm a poll loop: the Windows driver only
 * ever sends ABORT for a genuine cancellation. */
int  fpc_abort(fpc_dev *dev, unsigned timeout_ms);

/* Capture one image.  `buf` must hold width*height bytes (8 bit grey).
 *
 * The sensor is polled until it delivers a frame.  timeout_ms == 0 polls
 * until the user touches the sensor; otherwise the call returns
 * FPC_ERR_TIMEOUT if no finger is presented in time. */
int  fpc_capture(fpc_dev *dev, uint8_t *buf, size_t len, unsigned timeout_ms);

/* Wait until the sensor is free again.  A finger that keeps resting on it
 * answers every GET_IMAGE, so without this two captures in a row return
 * the same touch.  timeout_ms == 0 waits indefinitely. */
int  fpc_wait_finger_off(fpc_dev *dev, unsigned timeout_ms);

/* Accessors (valid after fpc_hello()). */
const fpc_sensor_info *fpc_info(const fpc_dev *dev);

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xffff) used by the protocol. */
uint16_t fpc_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FPC_USB_H */
