/*
 * libfprint driver for the FPC sensor in the Microsoft Modern Keyboard
 * with Fingerprint ID (045e:0815 / 045e:0816).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "fpi-device.h"

G_DECLARE_FINAL_TYPE (FpiDeviceFpcUsb, fpi_device_fpcusb, FPI, DEVICE_FPCUSB,
                      FpDevice)

/* USB */
#define FPCUSB_INTERFACE   1
#define FPCUSB_EP_IN       0x83
#define FPCUSB_EP_OUT      0x04
#define FPCUSB_PACKET      64

/* protocol */
#define FPCUSB_RESP        0x1000
#define FPCUSB_CMD_HELLO   0x0001
#define FPCUSB_CMD_STATUS  0x0005
#define FPCUSB_CMD_IMAGE   0x0007
#define FPCUSB_CMD_ABORT   0x0008
#define FPCUSB_CMD_ERROR   0x000f

/* Status word of a 0x1007 answer that carries no image: the firmware
 * answers every GET_IMAGE, and this is its "no finger right now". */
#define FPCUSB_IMG_STATUS_NO_IMAGE 0x0003

#define FPCUSB_DPI         508
