/*
 * libfprint driver for the FPC fingerprint sensor built into the
 * Microsoft Modern Keyboard with Fingerprint ID (045e:0815 / 045e:0816).
 *
 * The wire protocol was reverse engineered from the Windows UMDF driver
 * KeyboardFingerprintDriver.dll; see ../docs/PROTOCOL.md.
 *
 * Drop this file into libfprint/drivers/fpcusb.c, add 'fpcusb' to the
 * drivers list in meson_options.txt / libfprint/meson.build and rebuild.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fpcusb"

#include "drivers_api.h"
#include "fpcusb.h"

struct _FpiDeviceFpcUsb
{
  FpImageDevice parent;

  guint16       id;             /* transaction id                     */
  guint16       seq;            /* sequence counter                   */
  guint16       hwid;

  guint         width;
  guint         height;

  guint8       *frame;          /* assembly buffer, width*height + 12 */
  gsize         frame_len;
  gsize         frame_off;

  guint         hello_tries;
  GCancellable *cancellable;    /* cancels the pending image transfer  */
  gboolean      deactivating;
};

/* The sensor ignores the first commands after it has been powered on. */
#define FPCUSB_HELLO_TRIES 20

G_DEFINE_TYPE (FpiDeviceFpcUsb, fpi_device_fpcusb, FP_TYPE_IMAGE_DEVICE)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static guint16
fpcusb_crc16 (const guint8 *data, gsize len)
{
  guint16 crc = 0xffff;

  for (gsize i = 0; i < len; i++)
    {
      guint8 x = (guint8) ((crc >> 8) ^ data[i]);

      x ^= (guint8) (x >> 4);
      crc = (guint16) ((crc << 8) ^ ((guint16) x << 12) ^
                       ((guint16) x << 5) ^ x);
    }
  return crc;
}

static guint16
get16 (const guint8 *p)
{
  return (guint16) (p[0] | ((guint16) p[1] << 8));
}

static void
put16 (guint8 *p, guint16 v)
{
  p[0] = (guint8) (v & 0xff);
  p[1] = (guint8) (v >> 8);
}

static void
fpcusb_geometry (FpiDeviceFpcUsb *self)
{
  guint16 hw = self->hwid;

  if ((hw & 0xfff0) == 0x0210)          /* FPC1021 */
    self->width = 160, self->height = 160;
  else if ((hw & 0xfff0) == 0x1500)     /* FPC1150 */
    self->width = 80, self->height = 208;
  else if ((hw & 0xfff0) == 0x1400)     /* FPC1140 */
    self->width = 56, self->height = 192;
  else if ((hw & 0xff0f) == 0x0101)     /* FPC1022 */
    self->width = 112, self->height = 88;
  else                                  /* FPC1020 and unknown */
    self->width = 192, self->height = 192;

  fp_dbg ("sensor hwid 0x%04x -> %ux%u", hw, self->width, self->height);
}

/* Build an 8 byte command frame. */
static FpiUsbTransfer *
fpcusb_cmd_transfer (FpDevice *dev, FpiDeviceFpcUsb *self, guint16 op)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_OUT, 8);
  put16 (t->buffer + 0, op);
  put16 (t->buffer + 2, self->id);
  put16 (t->buffer + 4, self->seq);
  put16 (t->buffer + 6, fpcusb_crc16 (t->buffer, 6));
  t->short_is_error = TRUE;
  return t;
}

/* Transfers whose outcome does not matter still need a callback. */
static void
fpcusb_ignore_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  g_clear_error (&error);
}

/* Acknowledge an unsolicited error frame (0x000f) with 0x100f. */
static void
fpcusb_ack_error (FpDevice *dev, FpiDeviceFpcUsb *self)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  self->seq++;
  fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_OUT, 10);
  put16 (t->buffer + 0, FPCUSB_CMD_ERROR | FPCUSB_RESP);
  put16 (t->buffer + 2, 0);
  put16 (t->buffer + 4, self->id);
  put16 (t->buffer + 6, self->seq);
  put16 (t->buffer + 8, fpcusb_crc16 (t->buffer, 8));
  fpi_usb_transfer_submit (t, 1000, NULL, fpcusb_ignore_cb, NULL);
}

/*
 * Validate a complete response frame:
 *   u16 op|0x1000 | u16 status | payload | u16 id | u16 seq | u16 crc
 */
static gboolean
fpcusb_check (FpiDeviceFpcUsb *self, const guint8 *f, gsize len, guint16 op,
              GError **error)
{
  guint16 crc;

  if (len < 10)
    {
      g_propagate_error (error, fpi_device_error_new_msg
                           (FP_DEVICE_ERROR_PROTO, "short frame"));
      return FALSE;
    }
  if (get16 (f) != (op | FPCUSB_RESP))
    {
      g_propagate_error (error, fpi_device_error_new_msg
                           (FP_DEVICE_ERROR_PROTO,
                           "unexpected opcode 0x%04x", get16 (f)));
      return FALSE;
    }
  crc = fpcusb_crc16 (f, len - 2);
  if (crc != get16 (f + len - 2))
    {
      g_propagate_error (error, fpi_device_error_new_msg
                           (FP_DEVICE_ERROR_PROTO, "bad crc"));
      return FALSE;
    }
  if (get16 (f + len - 6) != self->id)
    {
      g_propagate_error (error, fpi_device_error_new_msg
                           (FP_DEVICE_ERROR_PROTO, "transaction id mismatch"));
      return FALSE;
    }
  self->seq = get16 (f + len - 4);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* open: say hello and learn the sensor geometry                       */
/* ------------------------------------------------------------------ */

enum open_states {
  OPEN_SEND_HELLO,
  OPEN_RECV_HELLO,
  OPEN_NUM_STATES,
};

static void
open_recv_cb (FpiUsbTransfer *transfer, FpDevice *dev,
              gpointer user_data, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  g_autoptr(GError) local_error = error;

  if (!local_error)
    {
      GError *proto_error = NULL;

      if (fpcusb_check (self, transfer->buffer, transfer->actual_length,
                        FPCUSB_CMD_HELLO, &proto_error))
        {
          self->hwid = get16 (transfer->buffer + 4);
          fpcusb_geometry (self);
          fpi_ssm_next_state (transfer->ssm);
          return;
        }
      local_error = proto_error;
    }

  if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&local_error));
      return;
    }

  /* The firmware drops the first commands after power on, exactly like
   * the Windows driver we retry a couple of times. */
  if (++self->hello_tries < FPCUSB_HELLO_TRIES)
    {
      fp_dbg ("hello attempt %u failed: %s", self->hello_tries,
              local_error->message);
      fpi_ssm_jump_to_state_delayed (transfer->ssm, OPEN_SEND_HELLO, 200);
      return;
    }

  fpi_ssm_mark_failed (transfer->ssm, g_steal_pointer (&local_error));
}

static void
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  FpiUsbTransfer *t;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_SEND_HELLO:
      if (self->hello_tries == 0)
        {
          self->id = (guint16) g_random_int_range (1, G_MAXUINT16);
          self->seq = (guint16) g_random_int_range (0, 0x7ff);
        }
      t = fpcusb_cmd_transfer (dev, self, FPCUSB_CMD_HELLO);
      t->ssm = ssm;
      fpi_usb_transfer_submit (t, 1000, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case OPEN_RECV_HELLO:
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, 12);
      fpi_usb_transfer_submit (t, 2000, NULL, open_recv_cb, NULL);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
open_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_image_device_open_complete (FP_IMAGE_DEVICE (dev), error);
}

static void
fpcusb_open (FpImageDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                     FPCUSB_INTERFACE, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self->hello_tries = 0;

  fpi_ssm_start (fpi_ssm_new (FP_DEVICE (dev), open_run_state,
                              OPEN_NUM_STATES), open_done);
}

static void
fpcusb_close (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  FPCUSB_INTERFACE, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

/* ------------------------------------------------------------------ */
/* capture loop                                                        */
/* ------------------------------------------------------------------ */

enum capture_states {
  CAPTURE_SEND_ABORT,
  CAPTURE_RECV_ABORT,
  CAPTURE_SEND_CMD,
  CAPTURE_FIRST_PACKET,
  CAPTURE_NEXT_PACKET,
  CAPTURE_FINISH,
  CAPTURE_NUM_STATES,
};

/*
 * Every capture starts by cancelling whatever the firmware may still be
 * doing; the reply to that abort is swallowed here so that the image
 * transfer starts on a clean pipe (this mirrors fpc-capture).
 */
static void
capture_abort_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  g_autoptr(GError) local_error = error;

  if (!local_error && transfer->actual_length >= 10)
    self->seq = get16 (transfer->buffer + transfer->actual_length - 4);

  fpi_ssm_next_state_delayed (transfer->ssm, 15);
}

static void
capture_first_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  guint16 op;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (transfer->actual_length < 6)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short first packet"));
      return;
    }

  op = get16 (transfer->buffer);
  if (op == FPCUSB_CMD_ERROR || op == (FPCUSB_CMD_ERROR | FPCUSB_RESP))
    {
      /* unsolicited error frame: acknowledge it and start over */
      fpcusb_ack_error (dev, self);
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }
  if (op == (FPCUSB_CMD_ABORT | FPCUSB_RESP))
    {
      /* aborted: silently restart the capture loop */
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }
  if (op != (FPCUSB_CMD_IMAGE | FPCUSB_RESP) || get16 (transfer->buffer + 2))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "image error 0x%04x/0x%04x",
                                                     op,
                                                     get16 (transfer->buffer + 2)));
      return;
    }

  /* A finger is on the sensor - tell the state machine of libfprint. */
  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);

  memcpy (self->frame, transfer->buffer, transfer->actual_length);
  self->frame_off = transfer->actual_length;
  fpi_ssm_next_state (transfer->ssm);
}

static void
capture_next_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                 gpointer user_data, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (transfer->actual_length < 3)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short continuation"));
      return;
    }
  if (get16 (transfer->buffer) == (FPCUSB_CMD_ABORT | FPCUSB_RESP))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      return;
    }

  /* strip the two byte continuation tag */
  memcpy (self->frame + self->frame_off, transfer->buffer + 2,
          transfer->actual_length - 2);
  self->frame_off += transfer->actual_length - 2;

  if (self->frame_off < self->frame_len)
    fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_NEXT_PACKET);
  else
    fpi_ssm_next_state (transfer->ssm);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  FpiUsbTransfer *t;
  GError *error = NULL;
  FpImage *img;
  gsize want;

  if (self->deactivating)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "deactivating"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_SEND_ABORT:
      self->seq++;
      t = fpcusb_cmd_transfer (dev, self, FPCUSB_CMD_ABORT);
      t->ssm = ssm;
      fpi_usb_transfer_submit (t, 1000, NULL, fpi_ssm_usb_transfer_cb, NULL);
      break;

    case CAPTURE_RECV_ABORT:
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, 10);
      fpi_usb_transfer_submit (t, 500, NULL, capture_abort_cb, NULL);
      break;

    case CAPTURE_SEND_CMD:
      self->frame_len = (gsize) self->width * self->height + 12;
      g_free (self->frame);
      self->frame = g_malloc0 (self->frame_len);
      self->frame_off = 0;
      self->seq++;
      t = fpcusb_cmd_transfer (dev, self, FPCUSB_CMD_IMAGE);
      t->ssm = ssm;
      fpi_usb_transfer_submit (t, 1000, NULL, fpi_ssm_usb_transfer_cb, NULL);
      break;

    case CAPTURE_FIRST_PACKET:
      /* No timeout: the device answers when a finger touches the sensor. */
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, FPCUSB_PACKET);
      fpi_usb_transfer_submit (t, 0, self->cancellable,
                               capture_first_cb, NULL);
      break;

    case CAPTURE_NEXT_PACKET:
      want = self->frame_len - self->frame_off + 2;
      if (want > FPCUSB_PACKET)
        want = FPCUSB_PACKET;
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, want);
      fpi_usb_transfer_submit (t, 5000, NULL, capture_next_cb, NULL);
      break;

    case CAPTURE_FINISH:
      if (!fpcusb_check (self, self->frame, self->frame_len,
                         FPCUSB_CMD_IMAGE, &error))
        {
          fpi_ssm_mark_failed (ssm, error);
          return;
        }

      img = fp_image_new (self->width, self->height);
      img->flags |= FPI_IMAGE_COLORS_INVERTED;
      memcpy (img->data, self->frame + 6,
              (gsize) self->width * self->height);
      fpi_image_device_image_captured (FP_IMAGE_DEVICE (dev), img);
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), FALSE);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
capture_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (self->deactivating)
    {
      g_clear_error (&error);
      self->deactivating = FALSE;
      fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (dev), NULL);
      return;
    }

  /* A retry error means "no usable image this time" - just listen again. */
  if (error && error->domain == FP_DEVICE_RETRY)
    {
      fp_dbg ("capture retry: %s", error->message);
      g_clear_error (&error);
    }

  if (error)
    {
      fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
      return;
    }

  /* keep listening for the next finger */
  fpi_ssm_start (fpi_ssm_new (dev, capture_run_state, CAPTURE_NUM_STATES),
                 capture_done);
}

/* ------------------------------------------------------------------ */
/* activate / deactivate                                               */
/* ------------------------------------------------------------------ */

static void
fpcusb_activate (FpImageDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  self->deactivating = FALSE;
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();
  fpi_image_device_activate_complete (dev, NULL);
  fpi_ssm_start (fpi_ssm_new (FP_DEVICE (dev), capture_run_state,
                              CAPTURE_NUM_STATES), capture_done);
}

static void
fpcusb_deactivate (FpImageDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  FpiUsbTransfer *t;

  self->deactivating = TRUE;

  /* Unblock the pending image transfer: the firmware answers a pending
   * GET_IMAGE with an ABORT frame as soon as opcode 8 arrives. */
  self->seq++;
  t = fpcusb_cmd_transfer (FP_DEVICE (dev), self, FPCUSB_CMD_ABORT);
  fpi_usb_transfer_submit (t, 1000, NULL, fpcusb_ignore_cb, NULL);

  g_cancellable_cancel (self->cancellable);
}

static void
fpcusb_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  /* The firmware itself waits for the finger, nothing to do here. */
}

/* ------------------------------------------------------------------ */
/* boilerplate                                                         */
/* ------------------------------------------------------------------ */

static const FpIdEntry id_table[] = {
  { .vid = 0x045e, .pid = 0x0815, },
  { .vid = 0x045e, .pid = 0x0816, },
  { .vid = 0, .pid = 0, },
};

static void
fpi_device_fpcusb_init (FpiDeviceFpcUsb *self)
{
  self->width = 192;
  self->height = 192;
}

static void
fpi_device_fpcusb_finalize (GObject *object)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (object);

  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->frame, g_free);
  G_OBJECT_CLASS (fpi_device_fpcusb_parent_class)->finalize (object);
}

static void
fpi_device_fpcusb_class_init (FpiDeviceFpcUsbClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_fpcusb_finalize;

  dev_class->id = "fpcusb";
  dev_class->full_name = "Microsoft Modern Keyboard with Fingerprint ID (FPC)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_open = fpcusb_open;
  img_class->img_close = fpcusb_close;
  img_class->activate = fpcusb_activate;
  img_class->deactivate = fpcusb_deactivate;
  img_class->change_state = fpcusb_change_state;

  /* Geometry is learned at open time; these are the FPC1020 defaults. */
  img_class->img_width = 192;
  img_class->img_height = 192;
  img_class->bz3_threshold = 25;
}
