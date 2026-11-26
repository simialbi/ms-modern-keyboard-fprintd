/*
 * Self test for the frame helpers that were recovered from the Windows
 * driver.  Runs without hardware.
 *
 *   make test
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fpc_usb.h"

static int failures;

static void check(const char *what, unsigned long got, unsigned long want)
{
	if (got == want) {
		printf("ok   %-42s 0x%04lx\n", what, got);
	} else {
		printf("FAIL %-42s got 0x%04lx want 0x%04lx\n",
		       what, got, want);
		failures++;
	}
}

/* Reference implementation of CRC-16/CCITT-FALSE, bit by bit. */
static uint16_t crc_reference(const uint8_t *d, size_t n)
{
	uint16_t crc = 0xffff;

	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)d[i] << 8;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
					     : (uint16_t)(crc << 1);
	}
	return crc;
}

int main(void)
{
	const uint8_t check_str[] = "123456789";
	uint8_t frame[8];
	uint8_t rsp[12];
	size_t i;

	/* 1. the table free implementation matches the textbook algorithm */
	check("CRC-16/CCITT-FALSE(\"123456789\")",
	      fpc_crc16(check_str, 9), 0x29b1);
	check("reference matches recovered implementation",
	      fpc_crc16(check_str, 9), crc_reference(check_str, 9));

	for (i = 0; i < 256; i++) {
		uint8_t buf[17];
		size_t n = i % 17;

		for (size_t j = 0; j < n; j++)
			buf[j] = (uint8_t)(i * 7 + j * 31);
		if (fpc_crc16(buf, n) != crc_reference(buf, n)) {
			printf("FAIL crc mismatch for length %zu\n", n);
			failures++;
			break;
		}
	}
	if (!failures)
		printf("ok   crc matches reference for 256 random buffers\n");

	/* 2. a HELLO command frame is built exactly like the Windows driver */
	memset(frame, 0, sizeof frame);
	frame[0] = FPC_CMD_HELLO;              /* opcode 0x0001 */
	frame[2] = 0x34; frame[3] = 0x12;      /* id  0x1234     */
	frame[4] = 0x05; frame[5] = 0x00;      /* seq 0x0005     */
	check("crc of a HELLO request", fpc_crc16(frame, 6),
	      crc_reference(frame, 6));

	/* 3. a HELLO response is accepted and yields the sensor id */
	memset(rsp, 0, sizeof rsp);
	rsp[0] = 0x01; rsp[1] = 0x10;          /* 0x1001          */
	rsp[2] = 0x00; rsp[3] = 0x00;          /* status ok       */
	rsp[4] = 0x10; rsp[5] = 0x02;          /* hwid  0x0210    */
	rsp[6] = 0x34; rsp[7] = 0x12;          /* id              */
	rsp[8] = 0x06; rsp[9] = 0x00;          /* seq             */
	{
		uint16_t crc = fpc_crc16(rsp, 10);

		rsp[10] = (uint8_t)(crc & 0xff);
		rsp[11] = (uint8_t)(crc >> 8);
		check("response crc covers everything but the crc",
		      fpc_crc16(rsp, 10),
		      (unsigned)(rsp[10] | (rsp[11] << 8)));
	}

	/* 4. frame size arithmetic of an image transfer */
	check("FPC1021 frame length (160x160 + 12)",
	      160u * 160u + 12u, 25612u);
	check("FPC1020 frame length (192x192 + 12)",
	      192u * 192u + 12u, 36876u);

	printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n",
	       failures);
	return failures ? 1 : 0;
}
