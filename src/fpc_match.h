/*
 * Shared image matcher and template format for the FPC keyboard sensor.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef FPC_MATCH_H
#define FPC_MATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FPC_TPL_MAGIC "FPCT"
#define FPC_TPL_VERSION 1
#define FPC_TPL_MAX_FRAMES 16
#define FPC_TPL_HEADER_SIZE 14
/*
 * Measured end to end through fprintd on the FPC1022 of the keyboard
 * (2025-11-26, 17 presses over a 12- and a 16-frame template): the
 * enrolled finger scores 0.412...0.854, a finger that was never enrolled
 * 0.117...0.389.  The two groups barely touch, so the threshold has to
 * sit between 0.39 and 0.41 to be perfect on this sample; 0.45 keeps a
 * margin on the impostor side and still accepts 9 of 11 genuine presses.
 * Overridable at run time with FPCUSB_THRESHOLD.
 */
#define FPC_MATCH_THRESHOLD_DEFAULT 0.45

/*
 * Logical template header. The serialized representation is a 14-byte,
 * little-endian header followed by count tightly packed width*height frames;
 * this struct is not written directly to disk.
 */
typedef struct {
	uint8_t  magic[4];
	uint8_t  version;
	uint8_t  flags;
	uint16_t hwid;
	uint16_t width;
	uint16_t height;
	uint16_t count;
} fpc_tpl_header;

typedef struct {
	unsigned width;
	unsigned height;
	int      max_shift_x;
	int      max_shift_y;
	int      rot_steps;
	double   rot_step_deg;
	unsigned border;
	unsigned min_overlap_px;
} fpc_match_params;

void fpc_match_default_params(fpc_match_params *p, unsigned width,
			      unsigned height);
void fpc_match_preprocess(uint8_t *frame, unsigned width, unsigned height);
double fpc_match_coverage(const uint8_t *frame, unsigned width,
			  unsigned height);
/* dx/dy are the probe-coordinate offsets into ref at the best pose. */
double fpc_match_score(const uint8_t *probe, const uint8_t *ref,
		       const fpc_match_params *p, int *dx, int *dy, double *deg);
double fpc_match_score_set(const uint8_t *probe, const uint8_t *frames,
			   unsigned count, const fpc_match_params *p,
			   unsigned *best_index);

size_t fpc_match_template_size(unsigned width, unsigned height,
			       unsigned count);
int fpc_match_template_serialize(uint8_t *blob, size_t blob_len,
				 uint16_t hwid, unsigned width,
				 unsigned height, unsigned count,
				 const uint8_t *frames);
int fpc_match_template_parse(const uint8_t *blob, size_t blob_len,
			     uint16_t expected_hwid, unsigned expected_width,
			     unsigned expected_height, fpc_tpl_header *header,
			     const uint8_t **frames);

#ifdef __cplusplus
}
#endif

#endif