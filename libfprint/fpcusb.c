/*
 * libfprint driver for the FPC fingerprint sensor built into the
 * Microsoft Modern Keyboard with Fingerprint ID (045e:0815 / 045e:0816).
 *
 * The wire protocol was reverse engineered from the Windows UMDF driver
 * KeyboardFingerprintDriver.dll; see ../docs/PROTOCOL.md.
 *
 * The sensor images 5.6 x 4.4 mm only, which is far too little for the
 * NBIS minutiae pipeline of libfprint: measured on this hardware a press
 * yields 0 to 2 minutiae and bozorth3 returns 0 below ten of them, so
 * every verification failed.  The driver is therefore not an
 * FpImageDevice: it keeps a set of preprocessed frames per finger and
 * compares a probe against all of them by normalised cross correlation
 * (../src/fpc_match.c, installed next to this file as fpcusb_match.c).
 *
 * Drop this file into libfprint/drivers/fpcusb.c, add 'fpcusb' to the
 * drivers list in meson_options.txt / libfprint/meson.build and rebuild;
 * libfprint/install.sh does all of that.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define FP_COMPONENT "fpcusb"

#include <stdlib.h>

#include "drivers_api.h"
#include "fpcusb.h"
#include "fpcusb_match.h"

typedef enum {
  FPCUSB_ACTION_NONE,
  FPCUSB_ACTION_ENROLL,
  FPCUSB_ACTION_VERIFY,
  FPCUSB_ACTION_IDENTIFY,
} FpcUsbAction;

struct _FpiDeviceFpcUsb
{
  FpDevice      parent;

  guint16       id;             /* transaction id                     */
  guint16       seq;            /* sequence counter                   */
  guint16       hwid;

  guint         width;
  guint         height;

  guint8       *frame;          /* assembly buffer, width*height + 12 */
  gsize         frame_len;
  gsize         frame_off;

  /* the operation currently driven by fprintd */
  FpcUsbAction  action;
  gint64        deadline;       /* monotonic microseconds             */
  gboolean      finger_present;
  gboolean      wait_for_lift;
  guint         command_failures;
  guint         empty_polls;    /* polls without an image, this press  */
  guint         images_seen;    /* images delivered in this operation  */
  guint         recover_count;

  /* enrolment state */
  guint8       *enroll_frames;  /* FPC_TPL_MAX_FRAMES frames          */
  guint         enroll_count;
  guint         enroll_stage;

  guint            hello_tries;
  guint            dump_count;
  gchar           *dump_dir;
  gdouble          threshold;
  fpc_match_params match_params;
};

/* The sensor ignores the first commands after it has been powered on. */
#define FPCUSB_HELLO_TRIES 20

/*
 * The firmware answers GET_IMAGE only while a finger is already on the
 * sensor and never completes a request that was submitted beforehand, so
 * capturing means polling GET_IMAGE.  600 ms per attempt is the value
 * that works on this hardware; 350 ms caught nothing at all.
 *
 * No ABORT is sent between two attempts: the Windows driver cancels only
 * its own pending read and sends the next GET_IMAGE 15 ms later, and it
 * reserves opcode 0x0008 for a real cancellation.  An abort per attempt
 * means thousands of them per session, which is the most plausible cause
 * of the imaging engine going silent for good.
 */
#define FPCUSB_ATTEMPT_MS   600
#define FPCUSB_GAP_MS       15
#define FPCUSB_DRAIN_MS     60
#define FPCUSB_OPERATION_MS 30000
#define FPCUSB_LIFT_MS      5000

/*
 * Sixteen presses fill the template completely (FPC_TPL_MAX_FRAMES).  The
 * window is only 5.6 x 4.4 mm, so the number of stored frames is what
 * decides whether a later press lands on skin the template has seen.
 */
#define FPCUSB_ENROLL_STAGES   16
#define FPCUSB_MIN_COVERAGE    0.10
#define FPCUSB_DUPLICATE_SCORE 0.95

/* Consecutive command failures that are treated as a wedged engine. */
#define FPCUSB_WEDGE_FAILURES 8

/*
 * The imaging engine of this firmware can stop answering GET_IMAGE at
 * all.  A healthy firmware answers every request within milliseconds,
 * either with an image or with FPCUSB_IMG_STATUS_NO_IMAGE, so only polls
 * that are not answered at all count here - and only once the engine has
 * answered at least one image during this operation.
 */
#define FPCUSB_WEDGE_POLLS      30
#define FPCUSB_WEDGE_RECOVERIES 3

G_DEFINE_TYPE (FpiDeviceFpcUsb, fpi_device_fpcusb, FP_TYPE_DEVICE)

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

static gsize
fpcusb_pixels (FpiDeviceFpcUsb *self)
{
  return (gsize) self->width * self->height;
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

/*
 * The search window, the rotation range and the decision threshold come
 * from the FAR/FRR measurement in tools/fpc_match_eval.c; they stay
 * overridable so that a field problem can be investigated without a
 * rebuild.
 */
static void
fpcusb_configure_matcher (FpiDeviceFpcUsb *self)
{
  const gchar *env;

  fpc_match_default_params (&self->match_params, self->width, self->height);
  self->threshold = FPC_MATCH_THRESHOLD_DEFAULT;

  env = g_getenv ("FPCUSB_THRESHOLD");
  if (env)
    {
      gdouble value = g_ascii_strtod (env, NULL);

      if (value > 0.0 && value <= 1.0)
        self->threshold = value;
      else
        fp_warn ("ignoring FPCUSB_THRESHOLD=%s, expected (0, 1]", env);
    }

  env = g_getenv ("FPCUSB_MAX_SHIFT");
  if (env)
    {
      gint64 value = g_ascii_strtoll (env, NULL, 10);

      if (value > 0 && value < self->width && value < self->height)
        {
          self->match_params.max_shift_x = (gint) value;
          self->match_params.max_shift_y = (gint) value;
        }
      else
        {
          fp_warn ("ignoring FPCUSB_MAX_SHIFT=%s, expected 1..%u", env,
                   MIN (self->width, self->height) - 1);
        }
    }

  env = g_getenv ("FPCUSB_ROT_STEPS");
  if (env)
    {
      gint64 value = g_ascii_strtoll (env, NULL, 10);

      if (value >= 0 && value <= 16)
        self->match_params.rot_steps = (gint) value;
      else
        fp_warn ("ignoring FPCUSB_ROT_STEPS=%s, expected 0..16", env);
    }

  g_clear_pointer (&self->dump_dir, g_free);
  env = g_getenv ("FPCUSB_DUMP_DIR");
  if (env && *env)
    self->dump_dir = g_strdup (env);

  fp_dbg ("matcher: threshold %.3f, shift %d/%d, rotation %d x %.1f deg",
          self->threshold, self->match_params.max_shift_x,
          self->match_params.max_shift_y, self->match_params.rot_steps,
          self->match_params.rot_step_deg);
}

/* Diagnostic dump of a captured frame; only active with FPCUSB_DUMP_DIR. */
static void
fpcusb_dump_frame (FpiDeviceFpcUsb *self, const guint8 *pixels)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *header = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GByteArray) pgm = NULL;

  if (!self->dump_dir)
    return;

  if (g_mkdir_with_parents (self->dump_dir, 0700) != 0)
    {
      fp_warn ("cannot create %s", self->dump_dir);
      g_clear_pointer (&self->dump_dir, g_free);
      return;
    }

  path = g_strdup_printf ("%s/fpcusb-%04u.pgm", self->dump_dir,
                          ++self->dump_count);
  header = g_strdup_printf ("P5\n%u %u\n255\n", self->width, self->height);
  pgm = g_byte_array_new ();
  g_byte_array_append (pgm, (const guint8 *) header, strlen (header));
  g_byte_array_append (pgm, pixels, fpcusb_pixels (self));

  if (!g_file_set_contents (path, (const gchar *) pgm->data, pgm->len, &error))
    fp_warn ("cannot write %s: %s", path, error->message);
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
/* template codec                                                      */
/* ------------------------------------------------------------------ */

static GVariant *
fpcusb_template_to_variant (FpiDeviceFpcUsb *self)
{
  g_autofree guint8 *blob = NULL;
  gsize len = fpc_match_template_size (self->width, self->height,
                                       self->enroll_count);

  if (len == 0)
    return NULL;

  blob = g_malloc (len);
  if (fpc_match_template_serialize (blob, len, self->hwid, self->width,
                                    self->height, self->enroll_count,
                                    self->enroll_frames) != 0)
    return NULL;

  return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, blob, len, 1);
}

/*
 * Return the frames of a stored print, or NULL with an error explaining
 * why they cannot be used (wrong sensor, wrong geometry, old format).
 */
static const guint8 *
fpcusb_template_frames (FpiDeviceFpcUsb *self, FpPrint *print,
                        GVariant **holder, guint *count, GError **error)
{
  g_autoptr(GVariant) data = NULL;
  const guint8 *blob;
  const guint8 *frames = NULL;
  fpc_tpl_header header;
  gsize len = 0;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("ay")))
    {
      g_propagate_error (error, fpi_device_error_new_msg
                           (FP_DEVICE_ERROR_DATA_INVALID,
                           "no fpcusb template in this print, please enrol again"));
      return NULL;
    }

  blob = g_variant_get_fixed_array (data, &len, 1);
  if (fpc_match_template_parse (blob, len, self->hwid, self->width,
                               self->height, &header, &frames) != 0)
    {
      g_propagate_error (error, fpi_device_error_new_msg
                           (FP_DEVICE_ERROR_DATA_INVALID,
                           "template does not match this sensor or format, "
                           "please delete it and enrol again"));
      return NULL;
    }

  *holder = g_steal_pointer (&data);
  *count = header.count;
  return frames;
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
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (!error)
    fpcusb_configure_matcher (self);

  fpi_device_open_complete (dev, error);
}

static void
fpcusb_open (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     FPCUSB_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->hello_tries = 0;

  fpi_ssm_start (fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES),
                 open_done);
}

static void
fpcusb_close (FpDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  FPCUSB_INTERFACE, 0, &error);
  fpi_device_close_complete (dev, error);
}

/* ------------------------------------------------------------------ */
/* capture: poll until a finger is on the sensor                       */
/* ------------------------------------------------------------------ */

/*
 * CAPTURE_DRAIN comes first on purpose: every capture starts by reading
 * the input pipe empty.  An answer left over from an earlier operation
 * would otherwise be used as *this* press - which once let a finger that
 * was never enrolled verify successfully, because the frame compared was
 * the last one of the enrolment.
 */
enum capture_states {
  CAPTURE_DRAIN,
  CAPTURE_SEND_CMD,
  CAPTURE_FIRST_PACKET,
  CAPTURE_NEXT_PACKET,
  CAPTURE_FINISH,
  CAPTURE_NUM_STATES,
};

static void fpcusb_operation_failed (FpDevice *dev, GError *error);
static void fpcusb_frame_captured (FpDevice *dev);
static void fpcusb_lift_detected (FpDevice *dev);

static gboolean
fpcusb_deadline_passed (FpiDeviceFpcUsb *self)
{
  return g_get_monotonic_time () >= self->deadline;
}

static gboolean
fpcusb_engine_wedged (FpiDeviceFpcUsb *self)
{
  return self->images_seen > 0 && self->empty_polls >= FPCUSB_WEDGE_POLLS;
}

/*
 * A read that ran into its deadline while the firmware answered late
 * leaves that answer in the pipe, and every following frame is then read
 * one answer too early - which shows up as a bad crc or as a stale HELLO
 * reply in place of an image.  Whenever a frame could not be used, the
 * pipe is therefore emptied before the next request.
 */
static void
fpcusb_capture_retry (FpiDeviceFpcUsb *self, FpiSsm *ssm)
{
  self->empty_polls++;

  if (fpcusb_deadline_passed (self))
    {
      fpi_ssm_mark_failed (ssm, fpi_device_retry_new_msg
                             (FP_DEVICE_RETRY_GENERAL,
                             "no usable image from the sensor"));
      return;
    }

  fpi_ssm_jump_to_state (ssm, CAPTURE_DRAIN);
}

/* A failed command is the symptom of the wedged imaging engine. */
static void
capture_command_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                    gpointer user_data, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        self->command_failures++;
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  self->command_failures = 0;
  fpi_ssm_next_state (transfer->ssm);
}

static void
capture_first_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  guint16 op;

  if (error)
    {
      /* A timeout simply means that no finger is on the sensor yet. */
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_clear_error (&error);

          if (self->wait_for_lift)
            {
              fpi_ssm_mark_completed (transfer->ssm);
              return;
            }

          fpcusb_capture_retry (self, transfer->ssm);
          return;
        }

      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length < 6)
    {
      fp_dbg ("short first packet (%zi bytes), resynchronising",
              transfer->actual_length);
      fpcusb_capture_retry (self, transfer->ssm);
      return;
    }

  op = get16 (transfer->buffer);
  if (op == FPCUSB_CMD_ERROR || op == (FPCUSB_CMD_ERROR | FPCUSB_RESP))
    {
      /* unsolicited error frame: acknowledge it and poll again */
      fpcusb_ack_error (dev, self);
      self->empty_polls++;
      if (fpcusb_deadline_passed (self))
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
          return;
        }
      fpi_ssm_jump_to_state_delayed (transfer->ssm, CAPTURE_SEND_CMD,
                                     FPCUSB_GAP_MS);
      return;
    }
  if (op == (FPCUSB_CMD_ABORT | FPCUSB_RESP))
    {
      self->empty_polls++;
      if (fpcusb_deadline_passed (self))
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
          return;
        }
      fpi_ssm_jump_to_state_delayed (transfer->ssm, CAPTURE_SEND_CMD,
                                     FPCUSB_GAP_MS);
      return;
    }
  if (op == (FPCUSB_CMD_IMAGE | FPCUSB_RESP) &&
      get16 (transfer->buffer + 2) == FPCUSB_IMG_STATUS_NO_IMAGE)
    {
      /* "no finger right now" - the engine is alive and answering, so
       * this is the normal empty round and also how a lift is seen. */
      self->empty_polls = 0;

      if (self->wait_for_lift)
        {
          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }

      if (fpcusb_deadline_passed (self))
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_retry_new_msg
                                 (FP_DEVICE_RETRY_GENERAL,
                                 "no finger on the sensor"));
          return;
        }

      fpi_ssm_jump_to_state_delayed (transfer->ssm, CAPTURE_SEND_CMD,
                                     FPCUSB_GAP_MS);
      return;
    }
  if (op != (FPCUSB_CMD_IMAGE | FPCUSB_RESP) || get16 (transfer->buffer + 2))
    {
      /* A frame of an earlier request, or a status we cannot use: empty
       * the pipe and ask again instead of failing the operation. */
      fp_dbg ("unusable answer 0x%04x/0x%04x, resynchronising", op,
              get16 (transfer->buffer + 2));
      fpcusb_capture_retry (self, transfer->ssm);
      return;
    }

  self->empty_polls = 0;
  self->images_seen++;
  self->recover_count = 0;

  if (!self->finger_present)
    {
      self->finger_present = TRUE;
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NEEDED);
    }

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
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fp_dbg ("continuation packet failed: %s", error->message);
          g_clear_error (&error);
          fpcusb_capture_retry (self, transfer->ssm);
          return;
        }

      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (transfer->actual_length < 3 ||
      get16 (transfer->buffer) == (FPCUSB_CMD_ABORT | FPCUSB_RESP))
    {
      /* the image was cut short - throw the half frame away */
      fpcusb_capture_retry (self, transfer->ssm);
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

/*
 * Keep reading until the pipe runs dry (a timeout), then send the next
 * request.  Anything read here belonged to a request whose answer we
 * already gave up on.
 */
static void
capture_drain_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  if (error)
    {
      if (g_error_matches (error, G_USB_DEVICE_ERROR,
                           G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_clear_error (&error);
          fpi_ssm_jump_to_state_delayed (transfer->ssm, CAPTURE_SEND_CMD,
                                         FPCUSB_GAP_MS);
          return;
        }

      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fp_dbg ("drained %zi stale bytes", transfer->actual_length);
  fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_DRAIN);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  GCancellable *cancellable = fpi_device_get_cancellable (dev);
  FpiUsbTransfer *t;
  GError *error = NULL;
  gsize want;

  if (g_cancellable_is_cancelled (cancellable))
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "cancelled"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_SEND_CMD:
      self->frame_len = fpcusb_pixels (self) + 12;
      g_free (self->frame);
      self->frame = g_malloc0 (self->frame_len);
      self->frame_off = 0;
      self->seq++;
      t = fpcusb_cmd_transfer (dev, self, FPCUSB_CMD_IMAGE);
      t->ssm = ssm;
      fpi_usb_transfer_submit (t, 1000, cancellable, capture_command_cb, NULL);
      break;

    case CAPTURE_FIRST_PACKET:
      /* The device answers only while a finger is on the sensor, so a
       * timeout here is the normal "not yet" and not an error. */
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, FPCUSB_PACKET);
      fpi_usb_transfer_submit (t, FPCUSB_ATTEMPT_MS, cancellable,
                               capture_first_cb, NULL);
      break;

    case CAPTURE_NEXT_PACKET:
      want = self->frame_len - self->frame_off + 2;
      if (want > FPCUSB_PACKET)
        want = FPCUSB_PACKET;
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, want);
      fpi_usb_transfer_submit (t, 5000, cancellable, capture_next_cb, NULL);
      break;

    case CAPTURE_DRAIN:
      /* Read away whatever is left in the pipe, then ask again. */
      t = fpi_usb_transfer_new (dev);
      t->ssm = ssm;
      fpi_usb_transfer_fill_bulk (t, FPCUSB_EP_IN, FPCUSB_PACKET);
      fpi_usb_transfer_submit (t, FPCUSB_DRAIN_MS, cancellable,
                               capture_drain_cb, NULL);
      break;

    case CAPTURE_FINISH:
      if (!fpcusb_check (self, self->frame, self->frame_len,
                         FPCUSB_CMD_IMAGE, &error))
        {
          fp_dbg ("discarding frame: %s", error->message);
          g_clear_error (&error);
          fpcusb_capture_retry (self, ssm);
          return;
        }

      /* While waiting for the lift an image means the finger is still
       * there, so keep polling until the sensor stops answering. */
      if (self->wait_for_lift)
        {
          if (fpcusb_deadline_passed (self))
            {
              fpi_ssm_mark_completed (ssm);
              return;
            }
          fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_SEND_CMD, FPCUSB_GAP_MS);
          return;
        }

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

  if (self->wait_for_lift)
    {
      self->wait_for_lift = FALSE;

      if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpcusb_operation_failed (dev, error);
          return;
        }

      /* A lift that cannot be observed must not abort the enrolment. */
      g_clear_error (&error);

      if (self->finger_present)
        {
          self->finger_present = FALSE;
          fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NEEDED,
                                                   FP_FINGER_STATUS_PRESENT);
        }

      fpcusb_lift_detected (dev);
      return;
    }

  if (error)
    {
      fpcusb_operation_failed (dev, error);
      return;
    }

  fpcusb_frame_captured (dev);
}

static void
fpcusb_capture_start (FpDevice *dev, gboolean wait_for_lift)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  gint timeout_ms = wait_for_lift ? FPCUSB_LIFT_MS : FPCUSB_OPERATION_MS;

  self->wait_for_lift = wait_for_lift;
  self->deadline = g_get_monotonic_time () + timeout_ms * G_TIME_SPAN_MILLISECOND;

  if (!wait_for_lift)
    self->empty_polls = 0;

  if (!wait_for_lift && !self->finger_present)
    fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NEEDED,
                                             FP_FINGER_STATUS_NONE);

  fpi_ssm_start (fpi_ssm_new (dev, capture_run_state, CAPTURE_NUM_STATES),
                 capture_done);
}

/* ------------------------------------------------------------------ */
/* wedge recovery                                                      */
/* ------------------------------------------------------------------ */

static void
recover_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (error)
    {
      g_clear_error (&error);

      /* HELLO did not help either: a port reset is the last thing the
       * host can do before the user has to replug the keyboard. */
      if (!g_usb_device_reset (fpi_device_get_usb_device (dev), &error))
        g_clear_error (&error);

      fpcusb_operation_failed (dev, fpi_device_error_new_msg
                                 (FP_DEVICE_ERROR_PROTO,
                                 "the fingerprint sensor stopped answering; "
                                 "unplug the keyboard and plug it back in"));
      return;
    }

  self->command_failures = 0;
  fpcusb_capture_start (dev, FALSE);
}

/*
 * The imaging engine of this firmware can stop answering GET_IMAGE while
 * HELLO still works.  Re-running the handshake is cheap and sometimes
 * enough; if it is not, the operation fails with an actionable message
 * instead of polling forever.
 */
static gboolean
fpcusb_try_recover (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (self->action == FPCUSB_ACTION_NONE ||
      self->recover_count >= FPCUSB_WEDGE_RECOVERIES)
    return FALSE;

  if (self->command_failures < FPCUSB_WEDGE_FAILURES &&
      !fpcusb_engine_wedged (self))
    return FALSE;

  fp_warn ("the sensor stopped answering, re-initialising it (attempt %u)",
           self->recover_count + 1);

  /*
   * No USB reset here.  It never revived the engine in any measurement,
   * and it makes the kernel re-enumerate the keyboard, which fprintd
   * reports as a disconnected device in the middle of an enrolment.
   * Only a full unbind/rebind helps, and that is the user's call.
   */
  self->recover_count++;
  self->hello_tries = 0;
  fpi_ssm_start (fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES),
                 recover_done);
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static void
fpcusb_operation_reset (FpiDeviceFpcUsb *self)
{
  self->action = FPCUSB_ACTION_NONE;
  self->wait_for_lift = FALSE;
  self->finger_present = FALSE;
  self->empty_polls = 0;
  self->images_seen = 0;
  self->recover_count = 0;
  self->enroll_count = 0;
  self->enroll_stage = 0;
  g_clear_pointer (&self->enroll_frames, g_free);
}

static void
fpcusb_operation_failed (FpDevice *dev, GError *error)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  FpcUsbAction action = self->action;
  gboolean was_present = self->finger_present;

  /* The sensor stopped answering altogether: try to revive it instead of
   * handing the user a bare USB error. */
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
      fpcusb_try_recover (dev))
    {
      g_clear_error (&error);
      return;
    }

  /*
   * The engine answered before and has now gone silent for a whole press
   * window, and re-initialising it did not help either: asking the user
   * for yet another press would never succeed.
   */
  if (action != FPCUSB_ACTION_NONE && error &&
      error->domain == FP_DEVICE_RETRY && fpcusb_engine_wedged (self))
    {
      g_clear_error (&error);
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                       "the fingerprint sensor stopped "
                                       "answering; unbind and rebind it "
                                       "(/sys/bus/usb/drivers/usb) or "
                                       "replug the keyboard");
    }

  /*
   * A press that could not be used is only a hint during enrolment: tell
   * the user about it and keep polling instead of ending the whole run,
   * which fprintd would report as a final enroll-retry-scan.
   */
  if (action == FPCUSB_ACTION_ENROLL && error &&
      error->domain == FP_DEVICE_RETRY)
    {
      self->wait_for_lift = FALSE;

      if (was_present)
        {
          self->finger_present = FALSE;
          fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE,
                                                   FP_FINGER_STATUS_PRESENT);
        }

      fp_dbg ("enrol stage %u: %s, waiting for the next press",
              self->enroll_stage, error->message);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL, error);
      fpcusb_capture_start (dev, FALSE);
      return;
    }

  fpcusb_operation_reset (self);

  if (was_present)
    fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE,
                                             FP_FINGER_STATUS_PRESENT);

  switch (action)
    {
    case FPCUSB_ACTION_ENROLL:
      fpi_device_enroll_complete (dev, NULL, error);
      break;

    case FPCUSB_ACTION_VERIFY:
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
          fpi_device_verify_complete (dev, NULL);
        }
      else
        {
          fpi_device_verify_complete (dev, error);
        }
      break;

    case FPCUSB_ACTION_IDENTIFY:
      if (error && error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_identify_report (dev, NULL, NULL, error);
          fpi_device_identify_complete (dev, NULL);
        }
      else
        {
          fpi_device_identify_complete (dev, error);
        }
      break;

    case FPCUSB_ACTION_NONE:
    default:
      fpi_device_action_error (dev, error);
      break;
    }
}

/* Hand the freshly captured frame over to the matcher. */
static guint8 *
fpcusb_take_frame (FpiDeviceFpcUsb *self)
{
  gsize pixels = fpcusb_pixels (self);
  guint8 *frame = g_malloc (pixels);

  memcpy (frame, self->frame + 6, pixels);
  fpc_match_preprocess (frame, self->width, self->height);
  return frame;
}

static void
fpcusb_enroll_frame (FpDevice *dev, guint8 *frame)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  gsize pixels = fpcusb_pixels (self);
  gdouble coverage = fpc_match_coverage (frame, self->width, self->height);

  if (coverage < FPCUSB_MIN_COVERAGE)
    {
      fp_dbg ("enrol frame rejected: coverage %.3f", coverage);
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      fpcusb_capture_start (dev, TRUE);
      return;
    }

  if (self->enroll_count > 0)
    {
      gdouble score = fpc_match_score_set (frame, self->enroll_frames,
                                           self->enroll_count,
                                           &self->match_params, NULL);

      if (score >= FPCUSB_DUPLICATE_SCORE)
        {
          fp_info ("enrol frame rejected: duplicate (score %.3f)", score);
          fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
          fpcusb_capture_start (dev, TRUE);
          return;
        }
    }

  if (self->enroll_count < FPC_TPL_MAX_FRAMES)
    memcpy (self->enroll_frames + (gsize) self->enroll_count++ * pixels,
            frame, pixels);

  self->enroll_stage++;
  fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);

  if (self->enroll_stage < FPCUSB_ENROLL_STAGES)
    {
      fpcusb_capture_start (dev, TRUE);
      return;
    }

  {
    FpPrint *print = NULL;
    GVariant *data = fpcusb_template_to_variant (self);

    if (!data)
      {
        fpcusb_operation_failed (dev, fpi_device_error_new_msg
                                   (FP_DEVICE_ERROR_GENERAL,
                                   "cannot serialise the template"));
        return;
      }

    fpi_device_get_enroll_data (dev, &print);
    fpi_print_set_type (print, FPI_PRINT_RAW);
    fpi_print_set_device_stored (print, FALSE);
    g_object_set (print, "fpi-data", data, NULL);

    fpcusb_operation_reset (self);
    fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
  }
}

static void
fpcusb_verify_frame (FpDevice *dev, guint8 *frame)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  gdouble coverage = fpc_match_coverage (frame, self->width, self->height);
  GError *error = NULL;
  FpPrint *print = NULL;

  g_autoptr(GVariant) holder = NULL;
  const guint8 *frames;
  guint count = 0;
  guint best_frame = 0;
  gdouble score;

  /* Not enough skin on the sensor: this is a retry, never a no-match. */
  if (coverage < FPCUSB_MIN_COVERAGE)
    {
      fpcusb_operation_failed (dev, fpi_device_retry_new_msg
                                 (FP_DEVICE_RETRY_CENTER_FINGER,
                                 "too little contact with the sensor"));
      return;
    }

  fpi_device_get_verify_data (dev, &print);
  frames = fpcusb_template_frames (self, print, &holder, &count, &error);
  if (!frames)
    {
      fpcusb_operation_failed (dev, error);
      return;
    }

  score = fpc_match_score_set (frame, frames, count, &self->match_params,
                               &best_frame);
  /* Deliberately a warning: libfprint's info level is suppressed by
   * default, and this number is what a user needs to judge a rejection. */
  fp_warn ("verify score %.3f (threshold %.3f, best of %u frames: #%u, "
           "coverage %.2f)", score, self->threshold, count, best_frame,
           coverage);

  fpcusb_operation_reset (self);
  fpi_device_verify_report (dev,
                            score >= self->threshold ? FPI_MATCH_SUCCESS
                                                     : FPI_MATCH_FAIL,
                            NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
fpcusb_identify_frame (FpDevice *dev, guint8 *frame)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  gdouble coverage = fpc_match_coverage (frame, self->width, self->height);
  GPtrArray *prints = NULL;
  FpPrint *best = NULL;
  gdouble best_score = -1.0;

  if (coverage < FPCUSB_MIN_COVERAGE)
    {
      fpcusb_operation_failed (dev, fpi_device_retry_new_msg
                                 (FP_DEVICE_RETRY_CENTER_FINGER,
                                 "too little contact with the sensor"));
      return;
    }

  fpi_device_get_identify_data (dev, &prints);

  for (guint i = 0; prints && i < prints->len; i++)
    {
      FpPrint *print = g_ptr_array_index (prints, i);

      g_autoptr(GVariant) holder = NULL;
      g_autoptr(GError) error = NULL;
      const guint8 *frames;
      guint count = 0;
      gdouble score;

      frames = fpcusb_template_frames (self, print, &holder, &count, &error);
      if (!frames)
        {
          fp_dbg ("skipping print %u: %s", i, error->message);
          continue;
        }

      score = fpc_match_score_set (frame, frames, count,
                                   &self->match_params, NULL);
      if (score > best_score)
        {
          best_score = score;
          best = print;
        }
    }

  fp_warn ("identify best score %.3f (threshold %.3f)", best_score,
           self->threshold);

  fpcusb_operation_reset (self);
  fpi_device_identify_report (dev,
                              best_score >= self->threshold ? best : NULL,
                              NULL, NULL);
  fpi_device_identify_complete (dev, NULL);
}

static void
fpcusb_frame_captured (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  g_autofree guint8 *frame = fpcusb_take_frame (self);

  fpcusb_dump_frame (self, frame);

  switch (self->action)
    {
    case FPCUSB_ACTION_ENROLL:
      fpcusb_enroll_frame (dev, frame);
      break;

    case FPCUSB_ACTION_VERIFY:
      fpcusb_verify_frame (dev, frame);
      break;

    case FPCUSB_ACTION_IDENTIFY:
      fpcusb_identify_frame (dev, frame);
      break;

    case FPCUSB_ACTION_NONE:
    default:
      fp_warn ("frame captured without an active operation");
      break;
    }
}

/* The finger was lifted, the next enrolment stage can start. */
static void
fpcusb_lift_detected (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  if (self->action != FPCUSB_ACTION_ENROLL)
    return;

  fpcusb_capture_start (dev, FALSE);
}

static void
fpcusb_enroll (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  fpcusb_operation_reset (self);
  self->action = FPCUSB_ACTION_ENROLL;
  self->enroll_frames = g_malloc0 (fpcusb_pixels (self) * FPC_TPL_MAX_FRAMES);

  fpcusb_capture_start (dev, FALSE);
}

static void
fpcusb_verify (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  fpcusb_operation_reset (self);
  self->action = FPCUSB_ACTION_VERIFY;

  fpcusb_capture_start (dev, FALSE);
}

static void
fpcusb_identify (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);

  fpcusb_operation_reset (self);
  self->action = FPCUSB_ACTION_IDENTIFY;

  fpcusb_capture_start (dev, FALSE);
}

static void
fpcusb_cancel (FpDevice *dev)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (dev);
  FpiUsbTransfer *t;

  /* Unblock a pending image transfer: the firmware answers a pending
   * GET_IMAGE with an ABORT frame as soon as opcode 8 arrives. */
  self->seq++;
  t = fpcusb_cmd_transfer (dev, self, FPCUSB_CMD_ABORT);
  fpi_usb_transfer_submit (t, 1000, NULL, fpcusb_ignore_cb, NULL);
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
  self->threshold = FPC_MATCH_THRESHOLD_DEFAULT;
  fpc_match_default_params (&self->match_params, self->width, self->height);
}

static void
fpi_device_fpcusb_finalize (GObject *object)
{
  FpiDeviceFpcUsb *self = FPI_DEVICE_FPCUSB (object);

  g_clear_pointer (&self->frame, g_free);
  g_clear_pointer (&self->enroll_frames, g_free);
  g_clear_pointer (&self->dump_dir, g_free);
  G_OBJECT_CLASS (fpi_device_fpcusb_parent_class)->finalize (object);
}

static void
fpi_device_fpcusb_class_init (FpiDeviceFpcUsbClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = fpi_device_fpcusb_finalize;

  dev_class->id = "fpcusb";
  dev_class->full_name = "Microsoft Modern Keyboard with Fingerprint ID (FPC)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  /* One press covers so little of the finger that a verification easily
   * touches an area no enrolment press ever saw; the template therefore
   * collects many frames. */
  dev_class->nr_enroll_stages = FPCUSB_ENROLL_STAGES;

  /*
   * libfprint's temperature model assumes a sensor heats up while it is
   * active and refuses to work with FP_DEVICE_ERROR_TOO_HOT ("Device
   * disabled to prevent overheating", which fprintd reports as
   * enroll-disconnected).  A sixteen-stage enrolment of a press sensor
   * easily exceeds the default budget, while this sensor only images
   * for a few hundred milliseconds per press and is idle in between, so
   * it has no thermal budget to track - like every other non-image
   * driver here.
   */
  dev_class->temp_hot_seconds = -1;

  dev_class->open = fpcusb_open;
  dev_class->close = fpcusb_close;
  dev_class->enroll = fpcusb_enroll;
  dev_class->verify = fpcusb_verify;
  dev_class->identify = fpcusb_identify;
  dev_class->cancel = fpcusb_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
