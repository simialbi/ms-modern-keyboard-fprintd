/*
 * Evaluate the shared FPC correlation matcher on labelled PGM captures.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "fpc_match.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <glob.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	char *path;
	uint8_t *pixels;
} eval_sample;

typedef struct {
	char *label;
	eval_sample *samples;
	size_t count;
	size_t capacity;
} eval_finger;

typedef struct {
	double *values;
	size_t count;
	size_t capacity;
} score_list;

static unsigned image_width;
static unsigned image_height;
static size_t image_size;

static char *
copy_string(const char *text)
{
	size_t len = strlen(text) + 1u;
	char *copy = malloc(len);

	if (copy)
		memcpy(copy, text, len);
	return copy;
}

/*
 * PGM header tokens: whitespace separated, '#' starts a comment that runs
 * to the end of the line.  Every read checks for EOF before the next one,
 * so the stream is never used after a failed operation.
 */
static int
read_token(FILE *file, char *token, size_t token_size)
{
	size_t len = 0;
	int c;

	for (;;) {
		c = fgetc(file);
		if (c == EOF)
			return -1;
		if (c == '#') {
			do {
				c = fgetc(file);
				if (c == EOF)
					return -1;
			} while (c != '\n');
			continue;
		}
		if (!isspace((unsigned char)c))
			break;
	}

	while (!isspace((unsigned char)c)) {
		if (len + 1u >= token_size)
			return -1;
		token[len++] = (char)c;
		c = fgetc(file);
		if (c == EOF)
			return -1;
	}
	if (c == '\r') {
		/* CRLF: swallow the LF.  Nothing behind it means the pixel
		 * data is missing, which the caller reports as malformed. */
		c = fgetc(file);
		if (c == EOF)
			return -1;
		if (c != '\n')
			ungetc(c, file);
	}
	token[len] = '\0';
	return 0;
}

static int
parse_unsigned(const char *text, unsigned long *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || end == text || *end != '\0')
		return -1;
	*value = parsed;
	return 0;
}

static int
read_pgm(const char *path, uint8_t **pixels, unsigned *width,
	 unsigned *height)
{
	char token[64];
	unsigned long w, h, maximum;
	FILE *file = fopen(path, "rb");
	uint8_t *data = NULL;
	size_t len;
	int result = -1;

	if (!file) {
		perror(path);
		return -1;
	}
	if (read_token(file, token, sizeof(token)) != 0 || strcmp(token, "P5")) {
		fprintf(stderr, "%s: expected binary PGM (P5)\n", path);
		goto done;
	}
	if (read_token(file, token, sizeof(token)) != 0 ||
	    parse_unsigned(token, &w) != 0 || w == 0 || w > UINT32_MAX ||
	    read_token(file, token, sizeof(token)) != 0 ||
	    parse_unsigned(token, &h) != 0 || h == 0 || h > UINT32_MAX ||
	    read_token(file, token, sizeof(token)) != 0 ||
	    parse_unsigned(token, &maximum) != 0 || maximum != 255) {
		fprintf(stderr, "%s: malformed or unsupported PGM header\n", path);
		goto done;
	}
	if ((size_t)w > SIZE_MAX / (size_t)h) {
		fprintf(stderr, "%s: image dimensions overflow\n", path);
		goto done;
	}
	len = (size_t)w * (size_t)h;
	data = malloc(len);
	if (!data) {
		fprintf(stderr, "%s: out of memory\n", path);
		goto done;
	}
	if (fread(data, 1, len, file) != len || ferror(file)) {
		fprintf(stderr, "%s: truncated image data\n", path);
		goto done;
	}
	*pixels = data;
	*width = (unsigned)w;
	*height = (unsigned)h;
	data = NULL;
	result = 0;

done:
	free(data);
	fclose(file);
	return result;
}

static int
append_sample(eval_finger *finger, const char *path)
{
	eval_sample sample = { 0 };
	unsigned width, height;

	if (read_pgm(path, &sample.pixels, &width, &height) != 0)
		return -1;
	if (image_size == 0) {
		image_width = width;
		image_height = height;
		image_size = (size_t)width * height;
	} else if (width != image_width || height != image_height) {
		fprintf(stderr, "%s: %ux%u does not match %ux%u\n", path,
			width, height, image_width, image_height);
		free(sample.pixels);
		return -1;
	}
	fpc_match_preprocess(sample.pixels, image_width, image_height);
	sample.path = copy_string(path);
	if (!sample.path) {
		free(sample.pixels);
		return -1;
	}
	if (finger->count == finger->capacity) {
		size_t capacity = finger->capacity ? finger->capacity * 2u : 8u;
		eval_sample *samples = realloc(finger->samples,
						capacity * sizeof(*samples));

		if (!samples) {
			free(sample.path);
			free(sample.pixels);
			return -1;
		}
		finger->samples = samples;
		finger->capacity = capacity;
	}
	finger->samples[finger->count++] = sample;
	return 0;
}

static void
free_fingers(eval_finger *fingers, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		for (size_t j = 0; j < fingers[i].count; j++) {
			free(fingers[i].samples[j].path);
			free(fingers[i].samples[j].pixels);
		}
		free(fingers[i].samples);
		free(fingers[i].label);
	}
	free(fingers);
}

static int
add_finger(eval_finger **fingers, size_t *count, const char *specification)
{
	const char *colon = strchr(specification, ':');
	eval_finger finger = { 0 };
	char *pattern;
	glob_t paths = { 0 };
	int glob_result;

	if (!colon || colon == specification || colon[1] == '\0') {
		fprintf(stderr, "invalid --finger argument: %s\n", specification);
		return -1;
	}
	finger.label = malloc((size_t)(colon - specification) + 1u);
	pattern = copy_string(colon + 1);
	if (!finger.label || !pattern) {
		free(finger.label);
		free(pattern);
		return -1;
	}
	memcpy(finger.label, specification, (size_t)(colon - specification));
	finger.label[colon - specification] = '\0';
	for (size_t i = 0; i < *count; i++) {
		if (strcmp((*fingers)[i].label, finger.label) == 0) {
			fprintf(stderr, "duplicate finger label: %s\n", finger.label);
			free(finger.label);
			free(pattern);
			return -1;
		}
	}

	glob_result = glob(pattern, 0, NULL, &paths);
	if (glob_result != 0 || paths.gl_pathc == 0) {
		fprintf(stderr, "%s: no files matched %s\n", finger.label, pattern);
		globfree(&paths);
		free(finger.label);
		free(pattern);
		return -1;
	}
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		if (append_sample(&finger, paths.gl_pathv[i]) != 0) {
			globfree(&paths);
			free(pattern);
			for (size_t j = 0; j < finger.count; j++) {
				free(finger.samples[j].path);
				free(finger.samples[j].pixels);
			}
			free(finger.samples);
			free(finger.label);
			return -1;
		}
	}
	globfree(&paths);
	free(pattern);
	{
		eval_finger *all = realloc(*fingers,
						  (*count + 1u) * sizeof(**fingers));

		if (!all) {
			for (size_t j = 0; j < finger.count; j++) {
				free(finger.samples[j].path);
				free(finger.samples[j].pixels);
			}
			free(finger.samples);
			free(finger.label);
			return -1;
		}
		*fingers = all;
		(*fingers)[(*count)++] = finger;
	}
	return 0;
}

static int
push_score(score_list *list, double score)
{
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2u : 64u;
		double *values = realloc(list->values, capacity * sizeof(*values));

		if (!values)
			return -1;
		list->values = values;
		list->capacity = capacity;
	}
	list->values[list->count++] = score;
	return 0;
}

static unsigned
copy_template(const eval_finger *finger, const eval_sample *exclude,
	      unsigned limit, uint8_t *frames)
{
	unsigned count = 0;

	for (size_t i = 0; i < finger->count && count < limit; i++) {
		if (&finger->samples[i] == exclude)
			continue;
		memcpy(frames + (size_t)count * image_size,
		       finger->samples[i].pixels, image_size);
		count++;
	}
	return count;
}

static int
evaluate(const eval_finger *fingers, size_t finger_count,
	 const fpc_match_params *params, unsigned frame_limit,
	 score_list *genuine, score_list *impostor)
{
	uint8_t *template_frames = malloc((size_t)frame_limit * image_size);
	int result = -1;

	if (!template_frames)
		return -1;
	for (size_t f = 0; f < finger_count; f++) {
		for (size_t s = 0; s < fingers[f].count; s++) {
			const eval_sample *probe = &fingers[f].samples[s];
			unsigned count = copy_template(&fingers[f], probe, frame_limit,
					       template_frames);
			double score;

			if (count == 0)
				goto done;
			score = fpc_match_score_set(probe->pixels, template_frames,
						    count, params, NULL);
			if (push_score(genuine, score) != 0)
				goto done;
			for (size_t other = 0; other < finger_count; other++) {
				if (other == f)
					continue;
				count = copy_template(&fingers[other], NULL, frame_limit,
						     template_frames);
				if (count == 0)
					goto done;
				score = fpc_match_score_set(probe->pixels, template_frames,
							    count, params, NULL);
				if (push_score(impostor, score) != 0)
					goto done;
			}
		}
	}
	result = 0;

done:
	free(template_frames);
	return result;
}

static void
rates(const score_list *genuine, const score_list *impostor,
	  double threshold, double *far, double *frr)
{
	size_t false_accepts = 0;
	size_t false_rejects = 0;

	for (size_t i = 0; i < impostor->count; i++)
		false_accepts += impostor->values[i] >= threshold;
	for (size_t i = 0; i < genuine->count; i++)
		false_rejects += genuine->values[i] < threshold;
	*far = impostor->count ? (double)false_accepts / impostor->count : 0.0;
	*frr = genuine->count ? (double)false_rejects / genuine->count : 0.0;
}

static void
print_histogram(const char *label, const score_list *list)
{
	unsigned bins[20] = { 0 };

	for (size_t i = 0; i < list->count; i++) {
		int bin = (int)((list->values[i] + 1.0) * 10.0);

		if (bin < 0)
			bin = 0;
		if (bin > 19)
			bin = 19;
		bins[bin]++;
	}
	printf("%s score histogram ([-1,1], 0.1 bins):\n", label);
	for (unsigned i = 0; i < 20; i++)
		printf("  %5.1f..%4.1f  %u\n", -1.0 + i * 0.1,
		       -0.9 + i * 0.1, bins[i]);
}

static void
print_report(const score_list *genuine, const score_list *impostor,
	     int full_curve)
{
	double eer_difference = DBL_MAX;
	double eer = 0.0;
	double eer_threshold = 0.0;

	for (unsigned i = 0; i <= 1000; i++) {
		double threshold = i / 1000.0;
		double far, frr, difference;

		rates(genuine, impostor, threshold, &far, &frr);
		difference = fabs(far - frr);
		if (difference < eer_difference) {
			eer_difference = difference;
			eer = (far + frr) / 2.0;
			eer_threshold = threshold;
		}
	}
	{
		double far, frr;

		rates(genuine, impostor, FPC_MATCH_THRESHOLD_DEFAULT, &far, &frr);
		printf("genuine comparisons: %zu; impostor comparisons: %zu\n",
		       genuine->count, impostor->count);
		printf("threshold %.2f: FAR %.4f, FRR %.4f\n",
		       FPC_MATCH_THRESHOLD_DEFAULT, far, frr);
		printf("EER %.4f at threshold %.3f\n", eer, eer_threshold);
	}
	if (full_curve) {
		puts("threshold  FAR       FRR");
		for (unsigned i = 0; i <= 20; i++) {
			double threshold = i * 0.05;
			double far, frr;

			rates(genuine, impostor, threshold, &far, &frr);
			printf("%9.2f  %8.4f  %8.4f\n", threshold, far, frr);
		}
		print_histogram("genuine", genuine);
		print_histogram("impostor", impostor);
	}
}

static int
run_configuration(const eval_finger *fingers, size_t finger_count,
		  int max_shift_x, int max_shift_y, int rot_steps,
		  unsigned frame_count, int full_curve)
{
	fpc_match_params params;
	score_list genuine = { 0 }, impostor = { 0 };
	int result = -1;

	fpc_match_default_params(&params, image_width, image_height);
	params.max_shift_x = max_shift_x;
	params.max_shift_y = max_shift_y;
	params.rot_steps = rot_steps;
	if (evaluate(fingers, finger_count, &params, frame_count,
		     &genuine, &impostor) != 0) {
		fprintf(stderr, "evaluation failed (out of memory or empty template)\n");
		goto done;
	}
	print_report(&genuine, &impostor, full_curve);
	result = 0;

done:
	free(genuine.values);
	free(impostor.values);
	return result;
}

static int
parse_option_number(const char *option, const char *text, unsigned long max,
		    unsigned long *value)
{
	if (parse_unsigned(text, value) != 0 || *value > max) {
		fprintf(stderr, "%s expects an integer from 0 to %lu\n", option, max);
		return -1;
	}
	return 0;
}

static void
usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"usage: %s [--sweep] [--max-shift N | --max-shift-x N "
		"--max-shift-y N] \\\n"
		"          [--rot-steps N] [--frames N] \\\n"
		"          --finger LABEL:PGM-GLOB --finger LABEL:PGM-GLOB ...\n"
		"\n"
		"Leave one capture out as the probe; build each labelled template from\n"
		"the other captures. At least two labels and two images per label are\n"
		"required. Quote glob patterns so the evaluator expands them.\n"
		"--sweep tests shift fractions 1/8, 1/4, 1/3; rotation steps 0, 2, 4;\n"
		"and template sizes 4, 8, 12, 16.\n",
		program);
}

int
main(int argc, char **argv)
{
	eval_finger *fingers = NULL;
	size_t finger_count = 0;
	int sweep = 0;
	int max_shift = -1;
	int max_shift_x = -1;
	int max_shift_y = -1;
	int rot_steps = -1;
	unsigned frame_count = 12;
	int result = 1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			usage(stdout, argv[0]);
			result = 0;
			goto done;
		} else if (strcmp(argv[i], "--sweep") == 0) {
			sweep = 1;
		} else if (strcmp(argv[i], "--finger") == 0) {
			if (++i >= argc || add_finger(&fingers, &finger_count,
							       argv[i]) != 0)
				goto done;
		} else if (strcmp(argv[i], "--max-shift") == 0 ||
			   strcmp(argv[i], "--max-shift-x") == 0 ||
			   strcmp(argv[i], "--max-shift-y") == 0 ||
			   strcmp(argv[i], "--rot-steps") == 0 ||
			   strcmp(argv[i], "--frames") == 0) {
			const char *option = argv[i];
			unsigned long value;
			unsigned long maximum = strcmp(option, "--rot-steps") == 0
				? 45u : strcmp(option, "--frames") == 0
				? FPC_TPL_MAX_FRAMES : INT32_MAX;

			if (++i >= argc || parse_option_number(option, argv[i], maximum,
							      &value) != 0)
				goto done;
			if (strcmp(option, "--max-shift") == 0)
				max_shift = (int)value;
			else if (strcmp(option, "--max-shift-x") == 0)
				max_shift_x = (int)value;
			else if (strcmp(option, "--max-shift-y") == 0)
				max_shift_y = (int)value;
			else if (strcmp(option, "--rot-steps") == 0)
				rot_steps = (int)value;
			else {
				if (value == 0) {
					fputs("--frames must be from 1 to 16\n", stderr);
					goto done;
				}
				frame_count = (unsigned)value;
			}
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(stderr, argv[0]);
			goto done;
		}
	}

	if (finger_count < 2) {
		fputs("provide at least two --finger labelled capture sets\n", stderr);
		usage(stderr, argv[0]);
		goto done;
	}
	for (size_t i = 0; i < finger_count; i++) {
		if (fingers[i].count < 2) {
			fprintf(stderr, "%s: need at least two captures for hold-out scoring\n",
				fingers[i].label);
			goto done;
		}
		printf("finger %s: %zu captures\n", fingers[i].label,
		       fingers[i].count);
	}
	printf("image geometry: %ux%u\n", image_width, image_height);
	if (sweep) {
		static const unsigned denominators[] = { 8, 4, 3 };
		static const int rotations[] = { 0, 2, 4 };
		static const unsigned frame_counts[] = { 4, 8, 12, 16 };

		for (size_t shift = 0; shift < sizeof(denominators) /
						     sizeof(denominators[0]); shift++) {
			int shift_x = (int)(image_width / denominators[shift]);
			int shift_y = (int)(image_height / denominators[shift]);

			for (size_t rotation = 0; rotation < sizeof(rotations) /
						       sizeof(rotations[0]); rotation++) {
				for (size_t frames = 0; frames < sizeof(frame_counts) /
						    sizeof(frame_counts[0]); frames++) {
					unsigned count = frame_counts[frames];
					score_list genuine = { 0 }, impostor = { 0 };
					fpc_match_params params;
					double far, frr, eer = 0.0, eer_threshold = 0.0;
					double difference_best = DBL_MAX;

					if (count > FPC_TPL_MAX_FRAMES)
						count = FPC_TPL_MAX_FRAMES;
					fpc_match_default_params(&params, image_width, image_height);
					params.max_shift_x = shift_x;
					params.max_shift_y = shift_y;
					params.rot_steps = rotations[rotation];
					if (evaluate(fingers, finger_count, &params, count,
						     &genuine, &impostor) != 0) {
						free(genuine.values);
						free(impostor.values);
						goto done;
					}
					for (unsigned t = 0; t <= 1000; t++) {
						double threshold = t / 1000.0;
						double difference;

						rates(&genuine, &impostor, threshold, &far, &frr);
						difference = fabs(far - frr);
						if (difference < difference_best) {
							difference_best = difference;
							eer = (far + frr) / 2.0;
							eer_threshold = threshold;
						}
					}
					printf("shift=%d/%d rotation=%d*2deg frames=%u "
					       "EER=%.4f threshold=%.3f\n", shift_x, shift_y,
					       rotations[rotation], count, eer, eer_threshold);
					free(genuine.values);
					free(impostor.values);
				}
			}
		}
		result = 0;
	} else {
		int shift_x = max_shift_x >= 0 ? max_shift_x :
			max_shift >= 0 ? max_shift : (int)(image_width / 3u);
		int shift_y = max_shift_y >= 0 ? max_shift_y :
			max_shift >= 0 ? max_shift : (int)(image_height / 3u);

		if (run_configuration(fingers, finger_count, shift_x, shift_y,
				      rot_steps >= 0 ? rot_steps : 2,
				      frame_count, 1) == 0)
			result = 0;
	}

done:
	free_fingers(fingers, finger_count);
	return result;
}