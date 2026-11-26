/*
 * fpc_capture - command line front end for the reverse engineered
 *               Microsoft Modern Keyboard with Fingerprint ID sensor.
 *
 *   fpc-capture info                 identify the sensor
 *   fpc-capture capture out.pgm      grab one image
 *   fpc-capture loop prefix          grab images until Ctrl-C
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpc_usb.h"

static volatile sig_atomic_t stop;

static void on_sigint(int sig)
{
	(void)sig;
	stop = 1;
}

static int write_pgm(const char *path, const uint8_t *img,
		     unsigned w, unsigned h)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		perror(path);
		return -1;
	}
	fprintf(f, "P5\n# FPC fingerprint, %u dpi\n%u %u\n255\n",
		FPC_IMAGE_DPI, w, h);
	fwrite(img, 1, (size_t)w * h, f);
	fclose(f);
	return 0;
}

static void usage(void)
{
	fputs("usage: fpc-capture [-v|-vv] <info|capture FILE|loop PREFIX>\n"
	      "\n"
	      "  info            identify the sensor and print its geometry\n"
	      "  capture FILE    wait for a finger and store one PGM image\n"
	      "  loop PREFIX     store PREFIX0001.pgm, PREFIX0002.pgm, ...\n"
	      "\n"
	      "  -v / -vv        protocol trace / hex dump on stderr\n",
	      stderr);
}

int main(int argc, char **argv)
{
	fpc_sensor_info info;
	fpc_dev *dev = NULL;
	uint8_t *img = NULL;
	size_t len;
	int verbose = 0, r, argi = 1;
	const char *cmd, *arg;

	while (argi < argc && argv[argi][0] == '-') {
		if (!strcmp(argv[argi], "-v"))
			verbose = 1;
		else if (!strcmp(argv[argi], "-vv"))
			verbose = 2;
		else {
			usage();
			return 2;
		}
		argi++;
	}
	if (argi >= argc) {
		usage();
		return 2;
	}
	cmd = argv[argi++];
	arg = argi < argc ? argv[argi] : NULL;

	signal(SIGINT, on_sigint);

	r = fpc_open(&dev, verbose);
	if (r != FPC_OK) {
		fprintf(stderr, "open: %s\n", fpc_strerror(r));
		if (r == FPC_ERR_NODEV)
			fputs("Is the keyboard plugged in via USB "
			      "(045e:0815 / 045e:0816)?\n", stderr);
		if (r == FPC_ERR_BUSY)
			fputs("Another process holds the interface.\n",
			      stderr);
		return 1;
	}

	r = fpc_hello(dev, &info);
	if (r != FPC_OK) {
		fprintf(stderr, "hello: %s\n", fpc_strerror(r));
		fpc_close(dev);
		return 1;
	}

	printf("sensor : FPC%u (hwid 0x%04x)\n", info.model, info.hwid);
	printf("image  : %u x %u, 8 bit grey, %u dpi\n",
	       info.width, info.height, FPC_IMAGE_DPI);
	printf("version: %s\n", info.fw);

	if (!strcmp(cmd, "info")) {
		uint16_t st = 0;

		if (fpc_status(dev, &st) == FPC_OK)
			printf("status : 0x%04x\n", st);
		fpc_close(dev);
		return 0;
	}

	len = (size_t)info.width * info.height;
	img = malloc(len);
	if (!img) {
		fpc_close(dev);
		return 1;
	}

	if (!strcmp(cmd, "capture")) {
		if (!arg) {
			usage();
			r = 2;
			goto done;
		}
		fputs("touch the fingerprint key...\n", stdout);
		fflush(stdout);
		r = fpc_capture(dev, img, len, 0);
		if (r != FPC_OK) {
			fprintf(stderr, "capture: %s\n", fpc_strerror(r));
			r = 1;
			goto done;
		}
		if (write_pgm(arg, img, info.width, info.height) == 0)
			printf("wrote %s\n", arg);
		r = 0;
	} else if (!strcmp(cmd, "loop")) {
		char path[512];
		int n = 0;

		if (!arg) {
			usage();
			r = 2;
			goto done;
		}
		while (!stop) {
			fputs("touch the fingerprint key (Ctrl-C to stop)...\n",
			      stdout);
			fflush(stdout);
			r = fpc_capture(dev, img, len, 0);
			if (r == FPC_ERR_ABORTED || r == FPC_ERR_TIMEOUT)
				continue;
			if (r != FPC_OK) {
				fprintf(stderr, "capture: %s\n",
					fpc_strerror(r));
				break;
			}
			snprintf(path, sizeof path, "%s%04d.pgm", arg, ++n);
			if (write_pgm(path, img, info.width, info.height) == 0)
				printf("wrote %s\n", path);
		}
		r = 0;
	} else {
		usage();
		r = 2;
	}

done:
	free(img);
	fpc_close(dev);
	return r;
}
