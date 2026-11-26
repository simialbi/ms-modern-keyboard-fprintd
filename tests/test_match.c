/*
 * Hardware-free tests for the shared FPC image matcher.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_match.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 112u
#define HEIGHT 88u
#define PIXELS (WIDTH * HEIGHT)

static unsigned failures;

static void
check(int condition, const char *description)
{
	if (condition)
		printf("ok   %s\n", description);
	else {
		printf("FAIL %s\n", description);
		failures++;
	}
}

static unsigned
noise(unsigned *state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state >> 24;
}

static void
make_fingerprint(uint8_t *frame, unsigned seed)
{
	unsigned state = seed;
	for (unsigned y = 0; y < HEIGHT; y++) {
		for (unsigned x = 0; x < WIDTH; x++) {
			double phase = x * 0.34 + 5.0 * sin(y * 0.045 + seed * 0.7)
				+ 1.3 * sin(x * 0.035 + y * 0.027 + seed);
			double ridge = cos(phase);
			double texture = ((int)noise(&state) - 128) * 0.08;
			double value = 128.0 + ridge * 42.0 + texture;

			frame[(size_t)y * WIDTH + x] = (uint8_t)value;
		}
	}
	fpc_match_preprocess(frame, WIDTH, HEIGHT);
}

static void
shift_frame(const uint8_t *source, uint8_t *dest, int dx, int dy)
{
	memset(dest, 128, PIXELS);
	for (int y = 0; y < (int)HEIGHT; y++) {
		for (int x = 0; x < (int)WIDTH; x++) {
			int sx = x - dx;
			int sy = y - dy;

			if (sx >= 0 && sy >= 0 && sx < (int)WIDTH && sy < (int)HEIGHT)
				dest[(size_t)y * WIDTH + x] = source[(size_t)sy * WIDTH + sx];
		}
	}
}

static void
rotate_frame(const uint8_t *source, uint8_t *dest, double degrees)
{
	double radians = degrees * 3.14159265358979323846 / 180.0;
	double c = cos(radians), s = sin(radians);
	double center_x = (WIDTH - 1u) / 2.0;
	double center_y = (HEIGHT - 1u) / 2.0;

	memset(dest, 128, PIXELS);
	for (unsigned y = 0; y < HEIGHT; y++) {
		for (unsigned x = 0; x < WIDTH; x++) {
			double ox = x - center_x;
			double oy = y - center_y;
			int sx = (int)floor(center_x + c * ox + s * oy + 0.5);
			int sy = (int)floor(center_y - s * ox + c * oy + 0.5);

			if (sx >= 0 && sy >= 0 && sx < (int)WIDTH && sy < (int)HEIGHT)
				dest[(size_t)y * WIDTH + x] =
					source[(size_t)sy * WIDTH + (unsigned)sx];
		}
	}
}

static void
test_matcher(void)
{
	uint8_t reference[PIXELS], similar[PIXELS], shifted[PIXELS], rotated[PIXELS];
	uint8_t impostor[PIXELS];
	fpc_match_params params;
	int dx = 0, dy = 0;
	double score;

	make_fingerprint(reference, 13u);
	fpc_match_default_params(&params, WIDTH, HEIGHT);
	score = fpc_match_score(reference, reference, &params, &dx, &dy, NULL);
	check(score > 0.999, "self-match scores approximately 1.0");
	for (size_t i = 0; i < PIXELS; i++) {
		int value = reference[i] + (int)(i % 5u) - 2;
		similar[i] = (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
	}
	score = fpc_match_score(similar, reference, &params, NULL, NULL, NULL);
	check(score > 0.90, "small image noise retains a high NCC score");

	shift_frame(reference, shifted, 7, -5);
	score = fpc_match_score(shifted, reference, &params, &dx, &dy, NULL);
	check(score > 0.90, "translated frame retains a high NCC score");
	if (dx != -7 || dy != 5)
		printf("     got offset (%d,%d), score %.4f\n", dx, dy, score);
	check(dx == -7 && dy == 5, "known translation is recovered");

	rotate_frame(reference, rotated, 4.0);
	score = fpc_match_score(rotated, reference, &params, NULL, NULL, NULL);
	check(score > 0.70, "small rotation is recovered by the pose search");

	make_fingerprint(impostor, 91u);
	score = fpc_match_score(impostor, reference, &params, NULL, NULL, NULL);
	check(score < 0.50, "unrelated synthetic ridge patterns stay below threshold");
}

static void
test_set_and_template(void)
{
	uint8_t frames[PIXELS * 2], probe[PIXELS];
	uint8_t max_frames[PIXELS * FPC_TPL_MAX_FRAMES];
	uint8_t *blob;
	const uint8_t *parsed_frames = NULL;
	fpc_tpl_header header;
	fpc_match_params params;
	unsigned index = 99;
	double score;
	size_t blob_len;

	make_fingerprint(frames, 7u);
	make_fingerprint(frames + PIXELS, 37u);
	memcpy(probe, frames + PIXELS, PIXELS);
	fpc_match_default_params(&params, WIDTH, HEIGHT);
	score = fpc_match_score_set(probe, frames, 2, &params, &index);
	check(score > 0.999 && index == 1,
	      "set scoring returns the matching frame and its index");
	for (unsigned i = 0; i < FPC_TPL_MAX_FRAMES; i++)
		make_fingerprint(max_frames + (size_t)i * PIXELS, 100u + i * 7u);
	memcpy(probe, max_frames + PIXELS * (FPC_TPL_MAX_FRAMES - 1u), PIXELS);
	score = fpc_match_score_set(probe, max_frames, FPC_TPL_MAX_FRAMES,
				    &params, &index);
	check(score > 0.999 && index == FPC_TPL_MAX_FRAMES - 1u,
	      "set scoring handles the maximum sixteen-frame template");

	blob_len = fpc_match_template_size(WIDTH, HEIGHT, 2);
	check(fpc_match_template_size(WIDTH, HEIGHT, FPC_TPL_MAX_FRAMES) > 0 &&
	      fpc_match_template_size(WIDTH, HEIGHT, FPC_TPL_MAX_FRAMES + 1u) == 0,
	      "template size enforces the sixteen-frame cap");
	blob = malloc(blob_len);
	check(blob != NULL, "template buffer allocation");
	if (!blob)
		return;
	check(fpc_match_template_serialize(blob, blob_len, 0x0121, WIDTH, HEIGHT,
					   2, frames) == 0,
	      "template serialization succeeds");
	check(fpc_match_template_parse(blob, blob_len, 0x0121, WIDTH, HEIGHT,
					  &header, &parsed_frames) == 0,
	      "template parses with matching sensor and geometry");
	check(header.count == 2 && header.hwid == 0x0121 &&
	      memcmp(parsed_frames, frames, sizeof frames) == 0,
	      "template round-trip preserves header and frames");

	blob[0] = 'X';
	check(fpc_match_template_parse(blob, blob_len, 0x0121, WIDTH, HEIGHT,
					  NULL, NULL) != 0,
	      "template with wrong magic is rejected");
	blob[0] = 'F';
	blob[4]++;
	check(fpc_match_template_parse(blob, blob_len, 0x0121, WIDTH, HEIGHT,
					  NULL, NULL) != 0,
	      "template with wrong version is rejected");
	blob[4] = FPC_TPL_VERSION;
	blob[5] = 0;
	check(fpc_match_template_parse(blob, blob_len, 0x0121, WIDTH, HEIGHT,
					  NULL, NULL) != 0,
	      "template missing the preprocessed flag is rejected");
	blob[5] = 1;
	check(fpc_match_template_parse(blob, blob_len, 0x0122, WIDTH, HEIGHT,
					  NULL, NULL) != 0,
	      "template from a different sensor is rejected");
	check(fpc_match_template_parse(blob, blob_len, 0x0121, WIDTH + 1u, HEIGHT,
					  NULL, NULL) != 0,
	      "template with wrong geometry is rejected");
	check(fpc_match_template_parse(blob, blob_len - 1u, 0x0121, WIDTH, HEIGHT,
					  NULL, NULL) != 0,
	      "truncated template is rejected");
	blob[12] = (uint8_t)(FPC_TPL_MAX_FRAMES + 1u);
	blob[13] = 0;
	check(fpc_match_template_parse(blob, blob_len, 0x0121, WIDTH, HEIGHT,
					  NULL, NULL) != 0,
	      "template declaring more than sixteen frames is rejected");
	free(blob);
}

static void
test_degenerate_inputs(void)
{
	uint8_t black[PIXELS] = { 0 };
	uint8_t white[PIXELS];
	fpc_match_params params;
	double score;

	memset(white, 255, sizeof white);
	fpc_match_default_params(&params, WIDTH, HEIGHT);
	fpc_match_preprocess(black, WIDTH, HEIGHT);
	fpc_match_preprocess(white, WIDTH, HEIGHT);
	check(fpc_match_coverage(black, WIDTH, HEIGHT) == 0.0 &&
	      fpc_match_coverage(white, WIDTH, HEIGHT) == 0.0,
	      "constant frames have no usable coverage");
	score = fpc_match_score(black, white, &params, NULL, NULL, NULL);
	check(score == 0.0 && isfinite(score),
	      "constant frames produce finite zero score");

	params.max_shift_x = 10000;
	params.max_shift_y = 10000;
	params.rot_steps = 0;
	params.min_overlap_px = 1;
	memset(black, 96, sizeof black);
	memset(white, 96, sizeof white);
	for (unsigned y = 12; y < HEIGHT - 12; y++)
		for (unsigned x = 12; x < WIDTH - 12; x++)
			black[(size_t)y * WIDTH + x] = white[(size_t)y * WIDTH + x] =
				(uint8_t)(80u + (x * 19u + y * 31u) % 96u);
	score = fpc_match_score(black, white, &params, NULL, NULL, NULL);
	check(score > 0.99,
	      "oversized translation search is clamped safely to the image");
}

/*
 * fpc_match_score_detail() must agree with fpc_match_score() on the peak
 * and report how far that peak rises above the rest of the correlation
 * plane - the evaluator decides on these two numbers.
 */
static void
test_score_detail(void)
{
	uint8_t a[PIXELS], b[PIXELS];
	uint8_t flat[PIXELS];
	fpc_match_params params;
	fpc_match_detail detail;
	double peak;

	fpc_match_default_params(&params, WIDTH, HEIGHT);
	for (unsigned y = 0; y < HEIGHT; y++) {
		for (unsigned x = 0; x < WIDTH; x++) {
			double value = 128.0 + 90.0 * sin((x + y * 0.4) * 0.7) *
				cos(y * 0.3);

			a[(size_t)y * WIDTH + x] = (uint8_t)value;
			b[(size_t)y * WIDTH + x] = (uint8_t)value;
		}
	}
	memset(flat, 128, sizeof flat);

	peak = fpc_match_score(a, b, &params, NULL, NULL, NULL);
	check(fpc_match_score_detail(a, b, &params, &detail) == peak &&
	      detail.peak == peak,
	      "score detail reports the same peak as the plain score");
	check(detail.sharpness > 0.0 && isfinite(detail.sharpness),
	      "an identical frame produces a peak above its own plane");
	check(fpc_match_score_detail(flat, flat, &params, &detail) == 0.0 &&
	      detail.sharpness == 0.0,
	      "a constant frame has neither peak nor sharpness");
}

int
main(void)
{
	test_matcher();
	test_set_and_template();
	test_degenerate_inputs();
	test_score_detail();
	printf(failures ? "\n%u matcher check(s) failed\n" :
	       "\nall matcher checks passed\n", failures);
	return failures ? 1 : 0;
}