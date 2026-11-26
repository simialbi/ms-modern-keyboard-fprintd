/*
 * fpc_usb.c - userspace driver for the FPC fingerprint sensor of the
 *             Microsoft Modern Keyboard with Fingerprint ID.
 *
 * Reverse engineered from KeyboardFingerprintDriver.dll (FPC WBDI 2.19.5.3).
 * See docs/PROTOCOL.md for the frame format and the flow of each command.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libusb-1.0/libusb.h>

#include "fpc_usb.h"

/* ------------------------------------------------------------------ */

struct fpc_dev {
	libusb_context       *ctx;
	libusb_device_handle *h;
	int                   iface;
	uint8_t               ep_in;
	uint8_t               ep_out;
	int                   detached;	/* kernel driver was detached */

	uint16_t              id;	/* random per-session transaction id */
	uint16_t              seq;	/* 11 bit sequence counter           */

	int                   verbose;
	volatile int          cancel;	/* set by fpc_abort()               */

	fpc_sensor_info       info;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

const char *fpc_strerror(int err)
{
	switch (err) {
	case FPC_OK:          return "success";
	case FPC_ERR_IO:      return "USB I/O error";
	case FPC_ERR_TIMEOUT: return "timeout";
	case FPC_ERR_PROTO:   return "protocol error";
	case FPC_ERR_ABORTED: return "transfer aborted by device";
	case FPC_ERR_DEVICE:  return "device reported an error";
	case FPC_ERR_NODEV:   return "no supported device found";
	case FPC_ERR_BUSY:    return "interface busy";
	case FPC_ERR_NOMEM:   return "out of memory";
	case FPC_ERR_PARAM:   return "invalid parameter";
	default:              return "unknown error";
	}
}

static void trace(const fpc_dev *d, int level, const char *fmt, ...)
{
	va_list ap;

	if (!d || d->verbose < level)
		return;
	fputs("fpc: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void trace_hex(const fpc_dev *d, const char *tag,
		      const uint8_t *buf, size_t len)
{
	size_t i;

	if (!d || d->verbose < 2)
		return;
	fprintf(stderr, "fpc: %s [%zu]", tag, len);
	for (i = 0; i < len && i < 64; i++)
		fprintf(stderr, " %02X", buf[i]);
	if (len > 64)
		fputs(" ...", stderr);
	fputc('\n', stderr);
}

static void msleep(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/*
 * CRC-16/CCITT-FALSE, poly 0x1021, init 0xffff, no reflection, no xor-out.
 *
 * Decompiled from sub_1000874e:
 *     x   = (crc >> 8) ^ byte;
 *     x  ^= x >> 4;
 *     crc = (crc << 8) ^ (x << 12) ^ (x << 5) ^ x;
 */
uint16_t fpc_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xffff;
	size_t i;

	for (i = 0; i < len; i++) {
		uint8_t x = (uint8_t)((crc >> 8) ^ data[i]);

		x ^= (uint8_t)(x >> 4);
		crc = (uint16_t)((crc << 8) ^ ((uint16_t)x << 12) ^
				 ((uint16_t)x << 5) ^ x);
	}
	return crc;
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ------------------------------------------------------------------ */
/* raw transport                                                       */
/* ------------------------------------------------------------------ */

static int bulk_out(fpc_dev *d, const void *buf, int len)
{
	int done = 0, r;

	trace_hex(d, "OUT", buf, (size_t)len);
	r = libusb_bulk_transfer(d->h, d->ep_out, (unsigned char *)buf, len,
				 &done, 2000);
	if (r == LIBUSB_ERROR_TIMEOUT)
		return FPC_ERR_TIMEOUT;
	if (r < 0) {
		trace(d, 1, "bulk out failed: %s", libusb_error_name(r));
		return FPC_ERR_IO;
	}
	return done == len ? FPC_OK : FPC_ERR_IO;
}

/*
 * Read exactly `len` bytes.  timeout_ms == 0 waits forever; the wait is
 * chopped into one second slices so that fpc_abort() can break out of a
 * pending capture.
 */
static int bulk_in(fpc_dev *d, void *buf, int len, unsigned timeout_ms)
{
	uint8_t *p = buf;
	unsigned waited = 0;
	int got = 0;

	while (got < len) {
		int done = 0;
		int slice = (timeout_ms == 0 || timeout_ms - waited > 1000)
				? 1000 : (int)(timeout_ms - waited);
		int r = libusb_bulk_transfer(d->h, d->ep_in, p + got,
					     len - got, &done, (unsigned)slice);

		got += done;
		if (r == LIBUSB_ERROR_TIMEOUT) {
			if (d->cancel)
				return FPC_ERR_ABORTED;
			if (got > 0)
				break;		/* short frame, done */
			waited += (unsigned)slice;
			if (timeout_ms && waited >= timeout_ms)
				return FPC_ERR_TIMEOUT;
			continue;
		}
		if (r < 0) {
			trace(d, 1, "bulk in failed: %s", libusb_error_name(r));
			return FPC_ERR_IO;
		}
		if (done == 0)
			break;
		/* a short packet terminates the transfer */
		if (done % FPC_USB_PACKET)
			break;
	}
	trace_hex(d, "IN ", buf, (size_t)got);
	return got;
}

/* Drop whatever the device may still have queued (used after open). */
static void drain(fpc_dev *d)
{
	uint8_t junk[FPC_USB_PACKET];
	int done;

	do {
		int r;

		done = 0;
		r = libusb_bulk_transfer(d->h, d->ep_in, junk, sizeof junk,
					 &done, 200);
		if (done)
			trace_hex(d, "drain", junk, (size_t)done);
		if (r < 0 && r != LIBUSB_ERROR_TIMEOUT)
			break;
	} while (done == FPC_USB_PACKET);
}

/* ------------------------------------------------------------------ */
/* framing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Request frame (8 bytes, little endian):
 *     u16 opcode
 *     u16 transaction id
 *     u16 sequence
 *     u16 crc16 over the preceding 6 bytes
 */
static int send_cmd(fpc_dev *d, uint16_t op)
{
	uint8_t f[8];

	put16(f + 0, op);
	put16(f + 2, d->id);
	put16(f + 4, d->seq);
	put16(f + 6, fpc_crc16(f, 6));
	trace(d, 1, "-> cmd 0x%04x id 0x%04x seq 0x%04x", op, d->id, d->seq);
	return bulk_out(d, f, sizeof f);
}

/*
 * Response frame:
 *     u16 opcode | 0x1000
 *     u16 status
 *     <payload>
 *     u16 transaction id
 *     u16 sequence
 *     u16 crc16 over everything before the crc
 */
static int check_resp(fpc_dev *d, const uint8_t *f, size_t len, uint16_t op)
{
	uint16_t crc;

	if (len < 10)
		return FPC_ERR_PROTO;

	if (get16(f) == (FPC_CMD_ERROR | FPC_RESP) ||
	    get16(f) == FPC_CMD_ERROR)
		return FPC_ERR_DEVICE;
	if (get16(f) == (FPC_CMD_ABORT | FPC_RESP) && op != FPC_CMD_ABORT)
		return FPC_ERR_ABORTED;
	if (get16(f) != (op | FPC_RESP)) {
		trace(d, 1, "unexpected opcode 0x%04x (want 0x%04x)",
		      get16(f), op | FPC_RESP);
		return FPC_ERR_PROTO;
	}

	crc = fpc_crc16(f, len - 2);
	if (crc != get16(f + len - 2)) {
		trace(d, 1, "bad crc: got 0x%04x want 0x%04x",
		      get16(f + len - 2), crc);
		return FPC_ERR_PROTO;
	}
	if (get16(f + len - 6) != d->id) {
		trace(d, 1, "id mismatch 0x%04x != 0x%04x",
		      get16(f + len - 6), d->id);
		return FPC_ERR_PROTO;
	}
	/* The device owns the sequence counter; adopt whatever it echoes. */
	d->seq = get16(f + len - 4);
	trace(d, 1, "<- rsp 0x%04x status 0x%04x seq 0x%04x",
	      get16(f), get16(f + 2), d->seq);
	return (int)len;
}

/* Acknowledge an unsolicited error frame (0x000f) with 0x100f. */
static void ack_error(fpc_dev *d)
{
	uint8_t f[10];

	d->seq++;
	put16(f + 0, FPC_CMD_ERROR | FPC_RESP);
	put16(f + 2, 0);
	put16(f + 4, d->id);
	put16(f + 6, d->seq);
	put16(f + 8, fpc_crc16(f, 8));
	trace(d, 1, "-> ack error frame");
	bulk_out(d, f, sizeof f);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void set_geometry(fpc_sensor_info *s)
{
	uint16_t hw = s->hwid;

	if ((hw & 0xfff0) == FPC_HWID_1021) {
		s->model = 1021; s->width = 160; s->height = 160;
		snprintf(s->fw, sizeof s->fw, "1021.11.5.12.0.0.1");
	} else if ((hw & 0xfff0) == FPC_HWID_1150) {
		s->model = 1150; s->width = 80;  s->height = 208;
		snprintf(s->fw, sizeof s->fw, "FPC1150.0.0.0.0.B1");
	} else if ((hw & 0xfff0) == FPC_HWID_1140) {
		s->model = 1140; s->width = 56;  s->height = 192;
		snprintf(s->fw, sizeof s->fw, "FPC1140.1.0.22");
	} else if ((hw & 0xff0f) == FPC_HWID_1022) {
		s->model = 1022; s->width = 112; s->height = 88;
		snprintf(s->fw, sizeof s->fw, "1022.0.0.0.0.124.72.1");
	} else {
		/* 0x02x0 and everything else: FPC1020 defaults */
		s->model = 1020; s->width = 192; s->height = 192;
		snprintf(s->fw, sizeof s->fw, "1020.0.0.0.12");
	}
}

int fpc_hello(fpc_dev *d, fpc_sensor_info *out)
{
	uint8_t f[12];
	int try, r = FPC_ERR_IO;

	if (!d)
		return FPC_ERR_PARAM;

	/* The Windows driver seeds both counters from the tick count. */
	d->id  = (uint16_t)(rand() & 0xffff);
	d->seq = (uint16_t)(rand() & 0x07ff);

	for (try = 0; try < 20; try++) {
		r = send_cmd(d, FPC_CMD_HELLO);
		if (r < 0)
			goto retry;

		msleep(10);
		r = bulk_in(d, f, (int)sizeof f, 200);
		if (r == (int)sizeof f) {
			r = check_resp(d, f, sizeof f, FPC_CMD_HELLO);
			if (r > 0)
				break;
			if (r == FPC_ERR_DEVICE)
				ack_error(d);
		} else if (r >= 0) {
			r = FPC_ERR_PROTO;
		}
retry:
		msleep(200);
	}
	if (r < 0)
		return r;

	d->info.hwid = get16(f + 4);
	set_geometry(&d->info);
	trace(d, 1, "sensor FPC%u hwid 0x%04x %ux%u",
	      d->info.model, d->info.hwid, d->info.width, d->info.height);
	if (out)
		*out = d->info;
	return FPC_OK;
}

int fpc_status(fpc_dev *d, uint16_t *value)
{
	uint8_t req[2], rsp[4];
	int r;

	if (!d)
		return FPC_ERR_PARAM;

	/* Opcode 5 is the one command that is sent without id/seq/crc. */
	put16(req, FPC_CMD_STATUS);
	r = bulk_out(d, req, sizeof req);
	if (r < 0)
		return r;

	r = bulk_in(d, rsp, (int)sizeof rsp, 1000);
	if (r < 0)
		return r;
	if (r != (int)sizeof rsp)
		return FPC_ERR_PROTO;
	if (value)
		*value = get16(rsp + 2);
	trace(d, 1, "status 0x%04x -> 0x%04x", get16(rsp), get16(rsp + 2));
	return FPC_OK;
}

int fpc_abort(fpc_dev *d, unsigned timeout_ms)
{
	uint8_t f[10];
	int r;

	if (!d)
		return FPC_ERR_PARAM;

	d->cancel = 1;
	d->seq++;
	r = send_cmd(d, FPC_CMD_ABORT);
	if (r < 0)
		return r;

	r = bulk_in(d, f, (int)sizeof f, timeout_ms ? timeout_ms : 5000);
	if (r < 0)
		return r;
	if (r != (int)sizeof f)
		return FPC_ERR_PROTO;
	return check_resp(d, f, sizeof f, FPC_CMD_ABORT) > 0
			? FPC_OK : FPC_ERR_PROTO;
}

/*
 * One image transfer.
 *
 * The reply to GET_IMAGE is a single logical frame of len+12 bytes that is
 * chopped into 64 byte USB packets.  The first packet carries the frame
 * header (opcode, status, length) directly; every following packet repeats
 * a two byte tag that has to be stripped before the payload is appended.
 */
int fpc_capture(fpc_dev *d, uint8_t *img, size_t len, unsigned timeout_ms)
{
	size_t total, off, remaining;
	uint8_t pkt[FPC_USB_PACKET];
	uint8_t *frame;
	int r;

	if (!d || !img || len < FPC_USB_PACKET)
		return FPC_ERR_PARAM;

	d->cancel = 0;

	/* Cancel a transfer that may still be pending in the firmware. */
	fpc_abort(d, 500);
	d->cancel = 0;
	msleep(15);

	total = len + 12;
	frame = malloc(total);
	if (!frame)
		return FPC_ERR_NOMEM;

	d->seq++;
	r = send_cmd(d, FPC_CMD_GET_IMAGE);
	if (r < 0)
		goto out;
	msleep(10);

	/* This read blocks until the user actually touches the sensor. */
	r = bulk_in(d, pkt, FPC_USB_PACKET, timeout_ms);
	if (r < 0)
		goto out;
	if (r < 6) {
		r = FPC_ERR_PROTO;
		goto out;
	}
	if (get16(pkt) == FPC_CMD_ERROR) {
		ack_error(d);
		r = FPC_ERR_DEVICE;
		goto out;
	}
	if (get16(pkt) == (FPC_CMD_ABORT | FPC_RESP)) {
		r = FPC_ERR_ABORTED;
		goto out;
	}
	if (get16(pkt) != (FPC_CMD_GET_IMAGE | FPC_RESP)) {
		trace(d, 1, "capture: unexpected opcode 0x%04x", get16(pkt));
		r = FPC_ERR_PROTO;
		goto out;
	}
	if (get16(pkt + 2) != 0) {
		trace(d, 1, "capture: status 0x%04x", get16(pkt + 2));
		r = FPC_ERR_DEVICE;
		goto out;
	}
	if (get16(pkt + 4) != (uint16_t)len)
		trace(d, 1, "capture: device sent %u bytes, expected %zu",
		      get16(pkt + 4), len);

	memcpy(frame, pkt, (size_t)r);
	off = (size_t)r;
	remaining = total - off;

	while (remaining) {
		size_t want = remaining + 2;

		if (want > FPC_USB_PACKET)
			want = FPC_USB_PACKET;

		r = bulk_in(d, pkt, (int)want, timeout_ms ? timeout_ms : 5000);
		if (r < 0)
			goto out;
		if (r < 3)
			break;
		if (get16(pkt) == (FPC_CMD_ABORT | FPC_RESP)) {
			r = FPC_ERR_ABORTED;
			goto out;
		}
		memcpy(frame + off, pkt + 2, (size_t)r - 2);
		off       += (size_t)r - 2;
		remaining -= (size_t)r - 2;
	}

	if (off != total) {
		trace(d, 1, "capture: short frame %zu/%zu", off, total);
		r = FPC_ERR_PROTO;
		goto out;
	}

	r = check_resp(d, frame, total, FPC_CMD_GET_IMAGE);
	if (r < 0)
		goto out;

	memcpy(img, frame + 6, len);
	r = FPC_OK;
out:
	free(frame);
	return r;
}

const fpc_sensor_info *fpc_info(const fpc_dev *d)
{
	return d ? &d->info : NULL;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

static int is_supported(const struct libusb_device_descriptor *dd)
{
	return dd->idVendor == FPC_USB_VID_MICROSOFT &&
	       (dd->idProduct == FPC_USB_PID_KEYBOARD_A ||
		dd->idProduct == FPC_USB_PID_KEYBOARD_B);
}

/* Locate the vendor interface and its bulk endpoint pair. */
static int find_endpoints(fpc_dev *d, libusb_device *dev)
{
	struct libusb_config_descriptor *cfg;
	int i, j, k, found = 0;

	if (libusb_get_active_config_descriptor(dev, &cfg) < 0)
		return FPC_ERR_IO;

	for (i = 0; i < cfg->bNumInterfaces && !found; i++) {
		for (j = 0; j < cfg->interface[i].num_altsetting; j++) {
			const struct libusb_interface_descriptor *id =
				&cfg->interface[i].altsetting[j];
			uint8_t in = 0, out = 0;

			for (k = 0; k < id->bNumEndpoints; k++) {
				const struct libusb_endpoint_descriptor *ep =
					&id->endpoint[k];

				if ((ep->bmAttributes & 0x03) !=
				    LIBUSB_TRANSFER_TYPE_BULK)
					continue;
				if (ep->bEndpointAddress & 0x80)
					in = ep->bEndpointAddress;
				else
					out = ep->bEndpointAddress;
			}
			if (in && out) {
				d->iface  = id->bInterfaceNumber;
				d->ep_in  = in;
				d->ep_out = out;
				found = 1;
				break;
			}
		}
	}
	libusb_free_config_descriptor(cfg);
	return found ? FPC_OK : FPC_ERR_NODEV;
}

int fpc_open_bus(fpc_dev **out, int verbose, uint8_t bus, uint8_t addr)
{
	libusb_device **list = NULL;
	fpc_dev *d;
	ssize_t n, i;
	int r = FPC_ERR_NODEV;

	if (!out)
		return FPC_ERR_PARAM;
	*out = NULL;

	d = calloc(1, sizeof *d);
	if (!d)
		return FPC_ERR_NOMEM;
	d->verbose = verbose;

	if (libusb_init(&d->ctx) < 0) {
		free(d);
		return FPC_ERR_IO;
	}

	n = libusb_get_device_list(d->ctx, &list);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor dd;

		if (libusb_get_device_descriptor(list[i], &dd) < 0)
			continue;
		if (!is_supported(&dd))
			continue;
		if (bus && libusb_get_bus_number(list[i]) != bus)
			continue;
		if (addr && libusb_get_device_address(list[i]) != addr)
			continue;

		trace(d, 1, "found %04x:%04x at %u:%u", dd.idVendor,
		      dd.idProduct, libusb_get_bus_number(list[i]),
		      libusb_get_device_address(list[i]));

		r = find_endpoints(d, list[i]);
		if (r < 0)
			continue;

		if (libusb_open(list[i], &d->h) < 0) {
			r = FPC_ERR_IO;
			continue;
		}
		libusb_set_auto_detach_kernel_driver(d->h, 1);
		if (libusb_claim_interface(d->h, d->iface) < 0) {
			libusb_close(d->h);
			d->h = NULL;
			r = FPC_ERR_BUSY;
			continue;
		}
		r = FPC_OK;
		break;
	}
	if (list)
		libusb_free_device_list(list, 1);

	if (r < 0) {
		libusb_exit(d->ctx);
		free(d);
		return r;
	}

	trace(d, 1, "claimed interface %d, ep in 0x%02x out 0x%02x",
	      d->iface, d->ep_in, d->ep_out);
	drain(d);
	srand((unsigned)time(NULL));
	*out = d;
	return FPC_OK;
}

int fpc_open(fpc_dev **out, int verbose)
{
	return fpc_open_bus(out, verbose, 0, 0);
}

void fpc_close(fpc_dev *d)
{
	if (!d)
		return;
	if (d->h) {
		libusb_release_interface(d->h, d->iface);
		libusb_close(d->h);
	}
	if (d->ctx)
		libusb_exit(d->ctx);
	free(d);
}
