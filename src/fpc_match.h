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
 * Measured on 26 real captures of two index fingers of the same hand
 * (15 + 11 presses, 165 cross-finger and 160 same-finger comparisons,
 * 2025-11-26): two presses of the same finger that hit the same spot
 * reach 0.96, but two *different* fingers still reach 0.744 - a 5.6 x
 * 4.4 mm patch of plain ridge flow is simply not very individual.
 * Against a 16-frame template the best score is 0.59...0.96 for the
 * enrolled finger and at most 0.744 for the other one, so the decision
 * boundary has to sit above 0.744: at 0.78 no impostor press of this
 * set is accepted (FAR 0/26) at the cost of rejecting genuine presses
 * that land on skin the template has not seen (FRR 4/26 here, more in
 * practice - press again).  The earlier 0.45 was calibrated against a
 * single, far less similar finger and let the other index finger verify
 * successfully.  Overridable at run time with FPCUSB_THRESHOLD.
 */
#define FPC_MATCH_THRESHOLD_DEFAULT 0.78
/*
 * Reference for fpc_match_detail.sharpness, which the evaluator can use
 * instead of the plain peak (--score sharp|mix).  On the set above it
 * did not separate the two fingers any better than the peak, so the
 * driver decides on the peak alone; the measure is kept because it is
 * what showed that.
 */
#define FPC_MATCH_SHARPNESS_REFERENCE 6.0

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

/*
 * Result of one comparison: the height of the correlation peak and how
 * far it rises above the rest of the correlation plane, in standard
 * deviations.  Two different fingers correlate mediocrely at almost
 * every pose, so the peak alone separates them badly while the
 * sharpness does it well - see the table in README.md.
 */
typedef struct {
	double peak;
	double sharpness;
	int    dx;
	int    dy;
	double deg;
} fpc_match_detail;

void fpc_match_default_params(fpc_match_params *p, unsigned width,
			      unsigned height);
void fpc_match_preprocess(uint8_t *frame, unsigned width, unsigned height);
double fpc_match_coverage(const uint8_t *frame, unsigned width,
			  unsigned height);
/* dx/dy are the probe-coordinate offsets into ref at the best pose. */
double fpc_match_score(const uint8_t *probe, const uint8_t *ref,
		       const fpc_match_params *p, int *dx, int *dy, double *deg);
/* Same comparison, but also reports the correlation-plane sharpness. */
double fpc_match_score_detail(const uint8_t *probe, const uint8_t *ref,
			      const fpc_match_params *p,
			      fpc_match_detail *detail);
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