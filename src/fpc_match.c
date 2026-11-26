/*
 * Shared image matcher and template codec for the FPC keyboard sensor.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "fpc_match.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FPC_HISTOGRAM_CLIP 50u
#define FPC_LOCAL_RADIUS 3
#define FPC_LOCAL_GAIN 2.0
#define FPC_PI 3.14159265358979323846
#define FPC_COARSE_STEP 4
#define FPC_COARSE_SAMPLE_STEP 4u
#define FPC_REFINE_CANDIDATES 2u
/*
 * Poses this close to the winning one are left out of the correlation
 * plane statistics: they still overlap the same ridges and would hide
 * how much the peak stands out.
 */
#define FPC_SHARP_EXCLUDE 8

static double fpc_match_score_internal(const uint8_t *probe, const uint8_t *ref,
				       const fpc_match_params *p, int *best_dx,
				       int *best_dy, double *best_deg,
				       double *sharpness);

static int
frame_size(unsigned width, unsigned height, size_t *size)
{
	if (width == 0 || height == 0 || (size_t)width > SIZE_MAX / height)
		return 0;
	*size = (size_t)width * height;
	return 1;
}

static uint16_t
read_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void
write_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

void
fpc_match_default_params(fpc_match_params *p, unsigned width, unsigned height)
{
	if (!p)
		return;
	if (width > INT32_MAX || height > INT32_MAX) {
		memset(p, 0, sizeof(*p));
		return;
	}

	p->width = width;
	p->height = height;
	p->max_shift_x = (int)(width / 3u);
	p->max_shift_y = (int)(height / 3u);
	p->rot_steps = 2;
	p->rot_step_deg = 2.0;
	p->border = width < height ? width / 32u : height / 32u;
	p->min_overlap_px = (unsigned)(((size_t)width * height) / 3u);
}

void
fpc_match_preprocess(uint8_t *frame, unsigned width, unsigned height)
{
	unsigned hist[256] = { 0 };
	size_t len;
	size_t cut;
	size_t acc = 0;
	int low = 0;
	int high = 255;
	uint8_t *stretched;
	uint64_t *integral;
	size_t stride;

	if (!frame || !frame_size(width, height, &len))
		return;

	cut = len / FPC_HISTOGRAM_CLIP;
	for (size_t i = 0; i < len; i++)
		hist[frame[i]]++;

	for (int i = 0; i < 256; i++) {
		acc += hist[i];
		if (acc > cut) {
			low = i;
			break;
		}
	}

	acc = 0;
	for (int i = 255; i >= 0; i--) {
		acc += hist[i];
		if (acc > cut) {
			high = i;
			break;
		}
	}

	if (high > low) {
		double scale = 255.0 / (double)(high - low);

		for (size_t i = 0; i < len; i++) {
			double value = ((double)frame[i] - low) * scale;

			if (value < 0.0)
				value = 0.0;
			else if (value > 255.0)
				value = 255.0;
			frame[i] = (uint8_t)value;
		}
	}

	if ((size_t)width + 1u > SIZE_MAX / ((size_t)height + 1u))
		return;
	stride = (size_t)width + 1u;
	if (stride * ((size_t)height + 1u) > SIZE_MAX / sizeof(*integral))
		return;
	stretched = malloc(len);
	integral = calloc(stride * ((size_t)height + 1u), sizeof(*integral));
	if (!stretched || !integral) {
		free(stretched);
		free(integral);
		return;
	}
	memcpy(stretched, frame, len);

	for (unsigned y = 0; y < height; y++) {
		uint64_t row_sum = 0;

		for (unsigned x = 0; x < width; x++) {
			row_sum += stretched[(size_t)y * width + x];
			integral[(size_t)(y + 1u) * stride + x + 1u] =
				integral[(size_t)y * stride + x + 1u] + row_sum;
		}
	}

	for (unsigned y = 0; y < height; y++) {
		unsigned y0 = y > FPC_LOCAL_RADIUS ? y - FPC_LOCAL_RADIUS : 0;
		unsigned y1 = y + FPC_LOCAL_RADIUS + 1u < height
			? y + FPC_LOCAL_RADIUS + 1u : height;

		for (unsigned x = 0; x < width; x++) {
			unsigned x0 = x > FPC_LOCAL_RADIUS ? x - FPC_LOCAL_RADIUS : 0;
			unsigned x1 = x + FPC_LOCAL_RADIUS + 1u < width
				? x + FPC_LOCAL_RADIUS + 1u : width;
			uint64_t sum = integral[(size_t)y1 * stride + x1]
				- integral[(size_t)y0 * stride + x1]
				- integral[(size_t)y1 * stride + x0]
				+ integral[(size_t)y0 * stride + x0];
			double mean = (double)sum / ((x1 - x0) * (y1 - y0));
			double value = 128.0 + FPC_LOCAL_GAIN *
				((double)stretched[(size_t)y * width + x] - mean);

			if (value < 0.0)
				value = 0.0;
			else if (value > 255.0)
				value = 255.0;
			frame[(size_t)y * width + x] = (uint8_t)(value + 0.5);
		}
	}

	free(integral);
	free(stretched);
}

double
fpc_match_coverage(const uint8_t *frame, unsigned width, unsigned height)
{
	size_t len;
	size_t usable = 0;
	double sum = 0.0;
	double sum_sq = 0.0;

	if (!frame || !frame_size(width, height, &len))
		return 0.0;

	for (size_t i = 0; i < len; i++) {
		double delta = (double)frame[i] - 128.0;

		if (fabs(delta) >= 8.0)
			usable++;
		sum += frame[i];
		sum_sq += (double)frame[i] * frame[i];
	}

	if (sum_sq - sum * sum / len < 1.0)
		return 0.0;
	return (double)usable / len;
}

static size_t
integral_index(unsigned x, unsigned y, size_t stride)
{
	return (size_t)y * stride + x;
}

static void
build_integrals(const uint8_t *image, unsigned width, unsigned height,
		double *sum, double *sum_sq)
{
	size_t stride = (size_t)width + 1u;

	memset(sum, 0, stride * ((size_t)height + 1u) * sizeof(*sum));
	memset(sum_sq, 0, stride * ((size_t)height + 1u) * sizeof(*sum_sq));
	for (unsigned y = 0; y < height; y++) {
		double row_sum = 0.0;
		double row_sq = 0.0;

		for (unsigned x = 0; x < width; x++) {
			double value = image[(size_t)y * width + x];
			row_sum += value;
			row_sq += value * value;
			sum[integral_index(x + 1u, y + 1u, stride)] =
				sum[integral_index(x + 1u, y, stride)] + row_sum;
			sum_sq[integral_index(x + 1u, y + 1u, stride)] =
				sum_sq[integral_index(x + 1u, y, stride)] + row_sq;
		}
	}
}

static double
rect_sum(const double *integral, size_t stride,
	 unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	return integral[integral_index(x1, y1, stride)]
		- integral[integral_index(x0, y1, stride)]
		- integral[integral_index(x1, y0, stride)]
		+ integral[integral_index(x0, y0, stride)];
}

static int
safe_rotation_margin(unsigned width, unsigned height, double radians,
		     unsigned *margin_x, unsigned *margin_y)
{
	double c = cos(radians);
	double s = sin(radians);
	double half_x = (width - 1u) / 2.0;
	double half_y = (height - 1u) / 2.0;
	double mx = fabs(1.0 - c) * half_x + fabs(s) * half_y;
	double my = fabs(s) * half_x + fabs(1.0 - c) * half_y;

	*margin_x = (unsigned)ceil(mx) + 1u;
	*margin_y = (unsigned)ceil(my) + 1u;
	return *margin_x * 2u < width && *margin_y * 2u < height;
}

static void
rotate_probe(const uint8_t *probe, uint8_t *rotated,
	     unsigned width, unsigned height, double radians,
	     unsigned margin_x, unsigned margin_y)
{
	double c = cos(radians), s = sin(radians);
	double center_x = (width - 1u) / 2.0;
	double center_y = (height - 1u) / 2.0;

	memset(rotated, 128, (size_t)width * height);
	for (unsigned y = margin_y; y < height - margin_y; y++) {
		for (unsigned x = margin_x; x < width - margin_x; x++) {
			double ox = x - center_x;
			double oy = y - center_y;
			int sx = (int)floor(center_x + c * ox + s * oy + 0.5);
			int sy = (int)floor(center_y - s * ox + c * oy + 0.5);

			rotated[(size_t)y * width + x] =
				probe[(size_t)sy * width + (unsigned)sx];
		}
	}
}

static double
score_rotation(const uint8_t *rotated, const uint8_t *ref,
	       const fpc_match_params *p, int shift_x, int shift_y,
	       const double *probe_integral, const double *probe_sq_integral,
	       const double *ref_integral, const double *ref_sq_integral,
	       unsigned margin_x, unsigned margin_y, unsigned sample_step)
{
	unsigned width = p->width;
	unsigned height = p->height;
	unsigned border = p->border;
	unsigned x0, y0, x1, y1;
	size_t stride = (size_t)width + 1u;
	size_t overlap;
	unsigned rx0, ry0, rx1, ry1;
	double sum_probe = 0.0, sq_probe = 0.0;
	double sum_ref = 0.0, sq_ref = 0.0, cross = 0.0;
	double variance_probe, variance_ref, covariance;
	size_t sample_count = 0;

	x0 = border;
	y0 = border;
	if (x0 < margin_x)
		x0 = margin_x;
	if (y0 < margin_y)
		y0 = margin_y;
	if (shift_x < 0 && x0 < border + (unsigned)(-shift_x))
		x0 = border + (unsigned)(-shift_x);
	if (shift_y < 0 && y0 < border + (unsigned)(-shift_y))
		y0 = border + (unsigned)(-shift_y);
	x1 = width - border;
	y1 = height - border;
	if (x1 > width - margin_x)
		x1 = width - margin_x;
	if (y1 > height - margin_y)
		y1 = height - margin_y;
	if (shift_x > 0 && x1 > width - border - (unsigned)shift_x)
		x1 = width - border - (unsigned)shift_x;
	if (shift_y > 0 && y1 > height - border - (unsigned)shift_y)
		y1 = height - border - (unsigned)shift_y;
	if (x1 <= x0 || y1 <= y0)
		return -1.0;

	overlap = (size_t)(x1 - x0) * (y1 - y0);
	if (overlap < p->min_overlap_px)
		return -1.0;
	rx0 = (unsigned)((int)x0 + shift_x);
	ry0 = (unsigned)((int)y0 + shift_y);
	rx1 = (unsigned)((int)x1 + shift_x);
	ry1 = (unsigned)((int)y1 + shift_y);
	if (sample_step == 1u) {
		sum_probe = rect_sum(probe_integral, stride, x0, y0, x1, y1);
		sq_probe = rect_sum(probe_sq_integral, stride, x0, y0, x1, y1);
		sum_ref = rect_sum(ref_integral, stride, rx0, ry0, rx1, ry1);
		sq_ref = rect_sum(ref_sq_integral, stride, rx0, ry0, rx1, ry1);
		for (unsigned y = y0; y < y1; y++) {
			size_t pi = (size_t)y * width + x0;
			size_t ri = (size_t)(ry0 + y - y0) * width + rx0;

			for (unsigned x = x0; x < x1; x++)
				cross += (double)rotated[pi++] * ref[ri++];
		}
		sample_count = overlap;
	} else {
		for (unsigned y = y0; y < y1; y += sample_step) {
			size_t pi = (size_t)y * width + x0;
			size_t ri = (size_t)(ry0 + y - y0) * width + rx0;

			for (unsigned x = x0; x < x1; x += sample_step) {
				double pv = rotated[pi + x - x0];
				double rv = ref[ri + x - x0];

				sum_probe += pv;
				sq_probe += pv * pv;
				sum_ref += rv;
				sq_ref += rv * rv;
				cross += pv * rv;
				sample_count++;
			}
		}
	}
	if (sample_count == 0)
		return -1.0;
	variance_probe = sq_probe - sum_probe * sum_probe / sample_count;
	variance_ref = sq_ref - sum_ref * sum_ref / sample_count;
	if (variance_probe <= 1e-9 || variance_ref <= 1e-9)
		return 0.0;
	covariance = cross - sum_probe * sum_ref / sample_count;
	{
		double score = covariance / sqrt(variance_probe * variance_ref);

		if (score > 1.0)
			return 1.0;
		if (score < -1.0)
			return -1.0;
		return score;
	}
}

/*
 * Spread of the correlation plane around the winning pose.  A genuine
 * pair produces one sharp peak and poor scores everywhere else, two
 * different fingers of generic ridge texture correlate mediocrely
 * almost everywhere - so the peak alone cannot tell them apart, but how
 * far it rises above its own plane can.
 */
static double
plane_sharpness(const uint8_t *probe, const uint8_t *ref,
		const fpc_match_params *p, uint8_t *rotated,
		double *probe_integral, double *probe_sq_integral,
		const double *ref_integral, const double *ref_sq_integral,
		unsigned max_shift_x, unsigned max_shift_y,
		int peak_x, int peak_y, double peak_deg, double peak)
{
	double radians = peak_deg * FPC_PI / 180.0;
	unsigned margin_x, margin_y;
	double sum = 0.0, sum_sq = 0.0;
	size_t n = 0;
	double mean, variance;

	if (!safe_rotation_margin(p->width, p->height, radians,
				  &margin_x, &margin_y))
		return 0.0;
	rotate_probe(probe, rotated, p->width, p->height, radians,
		     margin_x, margin_y);
	build_integrals(rotated, p->width, p->height,
			probe_integral, probe_sq_integral);

	for (int dy = -(int)max_shift_y; dy <= (int)max_shift_y;
	     dy += FPC_COARSE_STEP) {
		for (int dx = -(int)max_shift_x; dx <= (int)max_shift_x;
		     dx += FPC_COARSE_STEP) {
			double score;

			if (abs(dx - peak_x) <= FPC_SHARP_EXCLUDE &&
			    abs(dy - peak_y) <= FPC_SHARP_EXCLUDE)
				continue;
			score = score_rotation(rotated, ref, p, dx, dy,
					       probe_integral, probe_sq_integral,
					       ref_integral, ref_sq_integral,
					       margin_x, margin_y,
					       FPC_COARSE_SAMPLE_STEP);
			if (score < -1.0)
				continue;
			sum += score;
			sum_sq += score * score;
			n++;
		}
	}

	if (n < 8)
		return 0.0;
	mean = sum / n;
	variance = sum_sq / n - mean * mean;
	if (variance < 1e-9)
		return 0.0;
	return (peak - mean) / sqrt(variance);
}

double
fpc_match_score_detail(const uint8_t *probe, const uint8_t *ref,
		       const fpc_match_params *p, fpc_match_detail *detail)
{
	int best_dx_store = 0, best_dy_store = 0;
	double best_deg_store = 0.0;
	int *best_dx = &best_dx_store;
	int *best_dy = &best_dy_store;
	double *best_deg = &best_deg_store;
	double sharpness = 0.0;
	double score;

	if (detail) {
		detail->peak = 0.0;
		detail->sharpness = 0.0;
		detail->dx = 0;
		detail->dy = 0;
		detail->deg = 0.0;
	}
	score = fpc_match_score_internal(probe, ref, p, best_dx, best_dy,
					 best_deg, detail ? &sharpness : NULL);
	if (detail) {
		detail->peak = score;
		detail->sharpness = sharpness;
		detail->dx = best_dx_store;
		detail->dy = best_dy_store;
		detail->deg = best_deg_store;
	}
	return score;
}

double
fpc_match_score(const uint8_t *probe, const uint8_t *ref,
		const fpc_match_params *p, int *best_dx, int *best_dy,
		double *best_deg)
{
	return fpc_match_score_internal(probe, ref, p, best_dx, best_dy,
					best_deg, NULL);
}

static double
fpc_match_score_internal(const uint8_t *probe, const uint8_t *ref,
			 const fpc_match_params *p, int *best_dx, int *best_dy,
			 double *best_deg, double *sharpness)
{
	size_t len, integral_len;
	size_t stride;
	uint8_t *rotated;
	double *integrals;
	double *probe_integral, *probe_sq_integral;
	double *ref_integral, *ref_sq_integral;
	unsigned max_shift_x, max_shift_y;
	unsigned radius_x, radius_y;
	int coarse_best_x[FPC_REFINE_CANDIDATES] = { 0 };
	int coarse_best_y[FPC_REFINE_CANDIDATES] = { 0 };
	int coarse_best_rotation[FPC_REFINE_CANDIDATES] = { 0 };
	double coarse_scores[FPC_REFINE_CANDIDATES] = { -1.0, -1.0 };
	double best = -1.0;
	int result_x = 0, result_y = 0;
	double result_deg = 0.0;

	if (best_dx)
		*best_dx = 0;
	if (best_dy)
		*best_dy = 0;
	if (best_deg)
		*best_deg = 0.0;
	if (!probe || !ref || !p || !frame_size(p->width, p->height, &len) ||
	    p->width > INT32_MAX || p->height > INT32_MAX ||
	    p->max_shift_x < 0 || p->max_shift_y < 0 || p->rot_steps < 0 ||
	    p->rot_steps > 45 || !isfinite(p->rot_step_deg) ||
	    p->rot_step_deg < 0.0 || p->rot_step_deg > 45.0 ||
	    p->border > (p->width - 1u) / 2u ||
	    p->border > (p->height - 1u) / 2u)
		return 0.0;

	stride = (size_t)p->width + 1u;
	if (stride > SIZE_MAX / ((size_t)p->height + 1u))
		return 0.0;
	integral_len = stride * ((size_t)p->height + 1u);
	if (integral_len > SIZE_MAX / (4u * sizeof(*integrals)))
		return 0.0;
	integrals = malloc(integral_len * 4u * sizeof(*integrals));
	rotated = malloc(len);
	if (!integrals || !rotated) {
		free(integrals);
		free(rotated);
		return 0.0;
	}
	probe_integral = integrals;
	probe_sq_integral = probe_integral + integral_len;
	ref_integral = probe_sq_integral + integral_len;
	ref_sq_integral = ref_integral + integral_len;
	build_integrals(ref, p->width, p->height, ref_integral, ref_sq_integral);

	max_shift_x = (unsigned)p->max_shift_x;
	max_shift_y = (unsigned)p->max_shift_y;
	radius_x = p->width - 1u - p->border * 2u;
	radius_y = p->height - 1u - p->border * 2u;
	if (max_shift_x > radius_x)
		max_shift_x = radius_x;
	if (max_shift_y > radius_y)
		max_shift_y = radius_y;

	for (int r = -p->rot_steps; r <= p->rot_steps; r++) {
		double radians = r * p->rot_step_deg * FPC_PI / 180.0;
		unsigned margin_x, margin_y;
		int coarse_x = 0, coarse_y = 0;
		double coarse_best = -1.0;

		if (!safe_rotation_margin(p->width, p->height, radians,
					  &margin_x, &margin_y))
			continue;
		rotate_probe(probe, rotated, p->width, p->height, radians,
			     margin_x, margin_y);
		build_integrals(rotated, p->width, p->height,
				probe_integral, probe_sq_integral);
		for (int dy = -((int)max_shift_y / FPC_COARSE_STEP) *
			     FPC_COARSE_STEP; ; ) {
			for (int dx = -((int)max_shift_x / FPC_COARSE_STEP) *
				     FPC_COARSE_STEP; ; ) {
				double score = score_rotation(rotated, ref, p, dx, dy,
							      probe_integral, probe_sq_integral,
							      ref_integral, ref_sq_integral,
							      margin_x, margin_y,
							      FPC_COARSE_SAMPLE_STEP);

				if (score > coarse_best) {
					coarse_best = score;
					coarse_x = dx;
					coarse_y = dy;
				}
				if (dx == (int)max_shift_x)
					break;
				dx = dx + FPC_COARSE_STEP > (int)max_shift_x
					? (int)max_shift_x : dx + FPC_COARSE_STEP;
			}
			if (dy == (int)max_shift_y)
				break;
			dy = dy + FPC_COARSE_STEP > (int)max_shift_y
				? (int)max_shift_y : dy + FPC_COARSE_STEP;
		}
		if (coarse_best < 0.0)
			continue;

		for (unsigned i = 0; i < FPC_REFINE_CANDIDATES; i++) {
			if (coarse_best <= coarse_scores[i])
				continue;
			for (unsigned j = FPC_REFINE_CANDIDATES - 1u; j > i; j--) {
				coarse_scores[j] = coarse_scores[j - 1u];
				coarse_best_x[j] = coarse_best_x[j - 1u];
				coarse_best_y[j] = coarse_best_y[j - 1u];
				coarse_best_rotation[j] = coarse_best_rotation[j - 1u];
			}
			coarse_scores[i] = coarse_best;
			coarse_best_x[i] = coarse_x;
			coarse_best_y[i] = coarse_y;
			coarse_best_rotation[i] = r;
			break;
		}
	}

	for (unsigned i = 0; i < FPC_REFINE_CANDIDATES; i++) {
		unsigned margin_x, margin_y;
		int r = coarse_best_rotation[i];
		double degrees, radians;

		if (coarse_scores[i] < 0.0)
			continue;
		degrees = r * p->rot_step_deg;
		radians = degrees * FPC_PI / 180.0;
		if (!safe_rotation_margin(p->width, p->height, radians,
					  &margin_x, &margin_y))
			continue;
		rotate_probe(probe, rotated, p->width, p->height, radians,
			     margin_x, margin_y);
		build_integrals(rotated, p->width, p->height,
				probe_integral, probe_sq_integral);

		for (int dy = coarse_best_y[i] - (FPC_COARSE_STEP - 1);
		     dy <= coarse_best_y[i] + (FPC_COARSE_STEP - 1); dy++) {
			if (dy < -(int)max_shift_y || dy > (int)max_shift_y)
				continue;
			for (int dx = coarse_best_x[i] - (FPC_COARSE_STEP - 1);
			     dx <= coarse_best_x[i] + (FPC_COARSE_STEP - 1); dx++) {
				double score;

				if (dx < -(int)max_shift_x || dx > (int)max_shift_x)
					continue;
				score = score_rotation(rotated, ref, p, dx, dy,
						      probe_integral, probe_sq_integral,
						      ref_integral, ref_sq_integral,
						      margin_x, margin_y, 1u);
				if (score > best) {
					best = score;
					result_x = dx;
					result_y = dy;
					result_deg = degrees;
					if (best >= 1.0)
						goto done;
				}
			}
		}
	}

done:
	if (sharpness) {
		*sharpness = best > 0.0
			? plane_sharpness(probe, ref, p, rotated,
					  probe_integral, probe_sq_integral,
					  ref_integral, ref_sq_integral,
					  max_shift_x, max_shift_y,
					  result_x, result_y, result_deg, best)
			: 0.0;
	}
	free(integrals);
	free(rotated);
	if (best < 0.0)
		best = 0.0;
	if (best_dx)
		*best_dx = result_x;
	if (best_dy)
		*best_dy = result_y;
	if (best_deg)
		*best_deg = result_deg;
	return best;
}

double
fpc_match_score_set(const uint8_t *probe, const uint8_t *frames,
		    unsigned count, const fpc_match_params *p,
		    unsigned *best_index)
{
	size_t len;
	double best = -1.0;
	unsigned index = 0;

	if (best_index)
		*best_index = 0;
	if (!probe || !frames || !p || count == 0 ||
	    count > FPC_TPL_MAX_FRAMES || !frame_size(p->width, p->height, &len) ||
	    len > SIZE_MAX / count)
		return 0.0;

	for (unsigned i = 0; i < count; i++) {
		double score = fpc_match_score(probe, frames + (size_t)i * len,
					       p, NULL, NULL, NULL);

		if (score > best) {
			best = score;
			index = i;
			/* One is the mathematical upper bound, so this is exact. */
			if (best >= 1.0)
				break;
		}
	}
	if (best_index)
		*best_index = index;
	return best;
}

size_t
fpc_match_template_size(unsigned width, unsigned height, unsigned count)
{
	size_t len;

	if (width > UINT16_MAX || height > UINT16_MAX || count == 0 ||
	    count > FPC_TPL_MAX_FRAMES || !frame_size(width, height, &len) ||
	    len > (SIZE_MAX - FPC_TPL_HEADER_SIZE) / count)
		return 0;
	return FPC_TPL_HEADER_SIZE + len * count;
}

int
fpc_match_template_serialize(uint8_t *blob, size_t blob_len, uint16_t hwid,
			     unsigned width, unsigned height, unsigned count,
			     const uint8_t *frames)
{
	size_t required = fpc_match_template_size(width, height, count);

	if (!blob || !frames || required == 0 || blob_len != required)
		return -1;
	memcpy(blob, FPC_TPL_MAGIC, 4);
	blob[4] = FPC_TPL_VERSION;
	blob[5] = 1u;
	write_le16(blob + 6, hwid);
	write_le16(blob + 8, (uint16_t)width);
	write_le16(blob + 10, (uint16_t)height);
	write_le16(blob + 12, (uint16_t)count);
	memcpy(blob + FPC_TPL_HEADER_SIZE, frames, required - FPC_TPL_HEADER_SIZE);
	return 0;
}

int
fpc_match_template_parse(const uint8_t *blob, size_t blob_len,
			 uint16_t expected_hwid, unsigned expected_width,
			 unsigned expected_height, fpc_tpl_header *header,
			 const uint8_t **frames)
{
	fpc_tpl_header parsed;
	size_t required;

	if (!blob || blob_len < FPC_TPL_HEADER_SIZE ||
	    memcmp(blob, FPC_TPL_MAGIC, 4) != 0 ||
	    blob[4] != FPC_TPL_VERSION || blob[5] != 1u)
		return -1;
	memcpy(parsed.magic, blob, 4);
	parsed.version = blob[4];
	parsed.flags = blob[5];
	parsed.hwid = read_le16(blob + 6);
	parsed.width = read_le16(blob + 8);
	parsed.height = read_le16(blob + 10);
	parsed.count = read_le16(blob + 12);
	if ((expected_hwid && parsed.hwid != expected_hwid) ||
	    (expected_width && parsed.width != expected_width) ||
	    (expected_height && parsed.height != expected_height))
		return -1;
	required = fpc_match_template_size(parsed.width, parsed.height,
					   parsed.count);
	if (required == 0 || blob_len != required)
		return -1;
	if (header)
		*header = parsed;
	if (frames)
		*frames = blob + FPC_TPL_HEADER_SIZE;
	return 0;
}