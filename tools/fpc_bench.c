/*
 * fpc_bench - score captured fingerprint images with libfprint's own
 *             matcher, without fprintd and without root.
 *
 * libfprint turns every image into NBIS minutiae (mindtct) and compares
 * two of them with bozorth3; a verification succeeds when the score
 * reaches the driver's bz3_threshold.  Enrolment never compares anything,
 * which is why a sensor can enrol happily and still never verify.
 *
 * This tool runs exactly that pipeline over PGM files written by
 * fpc-capture, so the match scores of a real finger can be measured and
 * the preprocessing of the driver can be tuned with numbers instead of
 * guesses.
 *
 *   fpc-bench finger*.pgm                 as the driver sees them
 *   fpc-bench --scale 2 finger*.pgm       try the enlarged variant
 *   fpc-bench --normalize finger*.pgm     try contrast normalisation
 *
 * It is built against the NBIS copy inside a libfprint checkout:
 *
 *   make bench                            uses build/libfprint
 *   make bench LIBFPRINT=/path/to/libfprint
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include <lfs.h>
#include <bozorth.h>

#define FPC_BENCH_DPI 508.0

typedef struct {
	const char *name;
	unsigned    width;
	unsigned    height;
	guint8     *data;

	int         minutiae;
	int         low_contrast_pct;
	struct xyt_struct xyt;
} sample;

/* ------------------------------------------------------------------ */
/* PGM input                                                           */
/* ------------------------------------------------------------------ */

static int next_token(FILE *f, char *buf, size_t len)
{
	size_t n = 0;
	int c;

	do {
		c = fgetc(f);
		if (c == '#') {
			while (c != '\n' && c != EOF)
				c = fgetc(f);
		}
	} while (c != EOF && isspace(c));

	if (c == EOF)
		return -1;

	while (c != EOF && !isspace(c) && n + 1 < len) {
		buf[n++] = (char)c;
		c = fgetc(f);
	}
	buf[n] = '\0';
	return 0;
}

static int read_pgm(const char *path, sample *s)
{
	char tok[64];
	FILE *f = fopen(path, "rb");
	unsigned long w, h, max;

	if (!f) {
		perror(path);
		return -1;
	}
	if (next_token(f, tok, sizeof tok) || strcmp(tok, "P5")) {
		fprintf(stderr, "%s: not a binary PGM\n", path);
		fclose(f);
		return -1;
	}
	if (next_token(f, tok, sizeof tok))
		goto bad;
	w = strtoul(tok, NULL, 10);
	if (next_token(f, tok, sizeof tok))
		goto bad;
	h = strtoul(tok, NULL, 10);
	if (next_token(f, tok, sizeof tok))
		goto bad;
	max = strtoul(tok, NULL, 10);
	if (!w || !h || max != 255) {
		fprintf(stderr, "%s: only 8 bit PGM is supported\n", path);
		fclose(f);
		return -1;
	}

	s->name = path;
	s->width = (unsigned)w;
	s->height = (unsigned)h;
	s->data = g_malloc(w * h);
	if (fread(s->data, 1, w * h, f) != w * h) {
		fprintf(stderr, "%s: truncated image\n", path);
		g_clear_pointer(&s->data, g_free);
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;

bad:
	fprintf(stderr, "%s: malformed header\n", path);
	fclose(f);
	return -1;
}

/* ------------------------------------------------------------------ */
/* preprocessing - keep in sync with the driver                        */
/* ------------------------------------------------------------------ */

static void invert(guint8 *d, size_t len)
{
	for (size_t i = 0; i < len; i++)
		d[i] = (guint8)(0xff - d[i]);
}

/*
 * Stretch the central part of the histogram over the full range.  The
 * sensor only uses a fraction of the 8 bit range, and NBIS discards every
 * block whose contrast stays below a fixed threshold.
 */
static void normalize(guint8 *d, size_t len, double clip)
{
	unsigned hist[256] = { 0 };
	size_t cut = (size_t)(len * clip);
	size_t acc = 0;
	int lo = 0, hi = 255;
	double scale;

	for (size_t i = 0; i < len; i++)
		hist[d[i]]++;

	for (int i = 0; i < 256; i++) {
		acc += hist[i];
		if (acc > cut) {
			lo = i;
			break;
		}
	}
	acc = 0;
	for (int i = 255; i >= 0; i--) {
		acc += hist[i];
		if (acc > cut) {
			hi = i;
			break;
		}
	}
	if (hi <= lo)
		return;

	scale = 255.0 / (hi - lo);
	for (size_t i = 0; i < len; i++) {
		double v = (d[i] - lo) * scale;

		d[i] = (guint8)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
}

/* Bilinear enlargement by an integer factor, like fpi_image_resize(). */
static guint8 *enlarge(const guint8 *src, unsigned w, unsigned h, unsigned f,
		       unsigned *ow, unsigned *oh)
{
	unsigned nw = w * f, nh = h * f;
	guint8 *dst = g_malloc((size_t)nw * nh);

	for (unsigned y = 0; y < nh; y++) {
		double sy = (y + 0.5) / f - 0.5;
		int y0 = (int)(sy < 0 ? 0 : sy);
		int y1 = y0 + 1 < (int)h ? y0 + 1 : (int)h - 1;
		double fy = sy - y0 < 0 ? 0 : sy - y0;

		for (unsigned x = 0; x < nw; x++) {
			double sx = (x + 0.5) / f - 0.5;
			int x0 = (int)(sx < 0 ? 0 : sx);
			int x1 = x0 + 1 < (int)w ? x0 + 1 : (int)w - 1;
			double fx = sx - x0 < 0 ? 0 : sx - x0;
			double v =
				src[y0 * w + x0] * (1 - fx) * (1 - fy) +
				src[y0 * w + x1] * fx * (1 - fy) +
				src[y1 * w + x0] * (1 - fx) * fy +
				src[y1 * w + x1] * fx * fy;

			dst[(size_t)y * nw + x] = (guint8)(v + 0.5);
		}
	}
	*ow = nw;
	*oh = nh;
	return dst;
}

/* ------------------------------------------------------------------ */
/* minutiae and matching                                               */
/* ------------------------------------------------------------------ */

/* The conversion libfprint does in minutiae_to_xyt(). */
static void to_xyt(MINUTIAE *minutiae, int w, int h, struct xyt_struct *xyt)
{
	struct minutiae_struct c[MAX_BOZORTH_MINUTIAE];
	int n = minutiae->num < MAX_BOZORTH_MINUTIAE ?
		minutiae->num : MAX_BOZORTH_MINUTIAE;

	for (int i = 0; i < n; i++) {
		MINUTIA *m = minutiae->list[i];

		lfs2nist_minutia_XYT(&c[i].col[0], &c[i].col[1], &c[i].col[2],
				     m, w, h);
		c[i].col[3] = (int)(m->reliability * 100.0 + 0.5);
		if (c[i].col[2] > 180)
			c[i].col[2] -= 360;
	}

	qsort(c, (size_t)n, sizeof c[0], sort_x_y);

	for (int i = 0; i < n; i++) {
		xyt->xcol[i] = c[i].col[0];
		xyt->ycol[i] = c[i].col[1];
		xyt->thetacol[i] = c[i].col[2];
	}
	xyt->nrows = n;
}

static int analyse(sample *s, int partial)
{
	MINUTIAE *minutiae = NULL;
	int *qmap = NULL, *dmap = NULL, *lcmap = NULL, *lfmap = NULL;
	int *hcmap = NULL;
	unsigned char *bdata = NULL;
	int mw = 0, mh = 0, bw = 0, bh = 0, bd = 0;
	LFSPARMS lfsparms = g_lfsparms_V2;
	int lc = 0, r;

	lfsparms.remove_perimeter_pts = partial ? TRUE : FALSE;

	r = get_minutiae(&minutiae, &qmap, &dmap, &lcmap, &lfmap, &hcmap,
			 &mw, &mh, &bdata, &bw, &bh, &bd,
			 s->data, (int)s->width, (int)s->height, 8,
			 FPC_BENCH_DPI / 25.4, &lfsparms);
	if (r) {
		fprintf(stderr, "%s: mindtct failed (%d)\n", s->name, r);
		return -1;
	}

	for (int i = 0; i < mw * mh; i++)
		lc += lcmap[i] ? 1 : 0;

	s->minutiae = minutiae->num;
	s->low_contrast_pct = (mw * mh) > 0 ? 100 * lc / (mw * mh) : 0;
	to_xyt(minutiae, (int)s->width, (int)s->height, &s->xyt);

	free_minutiae(minutiae);
	g_free(qmap);
	g_free(dmap);
	g_free(lcmap);
	g_free(lfmap);
	g_free(hcmap);
	g_free(bdata);
	return 0;
}

/* ------------------------------------------------------------------ */

/*
 * Load every file with one set of preprocessing options.  Returns the
 * number of usable images.
 */
static int load_all(char **paths, int count, sample *out,
		    int scale, int do_normalize, int do_invert, int partial)
{
	int n = 0;

	for (int i = 0; i < count; i++) {
		sample *s = &out[n];

		memset(s, 0, sizeof *s);
		if (read_pgm(paths[i], s))
			continue;

		if (do_normalize)
			normalize(s->data, (size_t)s->width * s->height, 0.02);
		if (do_invert)
			invert(s->data, (size_t)s->width * s->height);
		if (scale > 1) {
			unsigned w, h;
			guint8 *big = enlarge(s->data, s->width, s->height,
					      (unsigned)scale, &w, &h);

			g_free(s->data);
			s->data = big;
			s->width = w;
			s->height = h;
		}
		if (analyse(s, partial)) {
			g_clear_pointer(&s->data, g_free);
			continue;
		}
		n++;
	}
	return n;
}

static void free_all(sample *s, int n)
{
	for (int i = 0; i < n; i++)
		g_free(s[i].data);
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/*
 * Try the preprocessing variants the driver could use and report which
 * one gets the images over the two hurdles of the libfprint pipeline:
 * at least MIN_COMPUTABLE_BOZORTH_MINUTIAE minutiae per image, and a
 * bozorth3 score that reaches the threshold.
 */
static void sweep(char **paths, int count, int threshold)
{
	sample *s = g_new0(sample, (size_t)count);
	int *scores = g_new0(int, (size_t)count * count);

	printf("%-34s %8s %9s %9s %9s\n", "variant", "min.med",
	       ">=10 min.", "score.med", ">=thr");

	for (int scale = 1; scale <= 3; scale++) {
		for (int norm = 0; norm <= 1; norm++) {
			for (int partial = 0; partial <= 1; partial++) {
				char name[64];
				int n = load_all(paths, count, s, scale,
						 norm, 1, partial);
				int enough = 0, pairs = 0, good = 0;
				int min_med, score_med;
				int *mins;

				if (n < 2) {
					free_all(s, n);
					continue;
				}

				mins = g_new0(int, (size_t)n);
				for (int i = 0; i < n; i++) {
					mins[i] = s[i].minutiae;
					if (s[i].minutiae >=
					    MIN_COMPUTABLE_BOZORTH_MINUTIAE)
						enough++;
				}
				qsort(mins, (size_t)n, sizeof *mins, cmp_int);
				min_med = mins[n / 2];

				for (int i = 0; i < n; i++) {
					int len = bozorth_probe_init(&s[i].xyt);

					for (int j = i + 1; j < n; j++) {
						int sc = bozorth_to_gallery(
							len, &s[i].xyt,
							&s[j].xyt);

						scores[pairs++] = sc;
						if (sc >= threshold)
							good++;
					}
				}
				qsort(scores, (size_t)pairs, sizeof *scores,
				      cmp_int);
				score_med = pairs ? scores[pairs / 2] : 0;

				snprintf(name, sizeof name, "scale %dx %-10s %s",
					 scale, norm ? "normalized" : "raw",
					 partial ? "partial" : "all minutiae");
				printf("%-34s %8d %6d/%-2d %9d %4d/%-3d\n",
				       name, min_med, enough, n, score_med,
				       good, pairs);

				g_free(mins);
				free_all(s, n);
			}
		}
	}

	printf("\nAll images have to be of the same finger for this to mean\n"
	       "anything.  An image with fewer than %d minutiae can never\n"
	       "match, whatever the threshold is.\n",
	       MIN_COMPUTABLE_BOZORTH_MINUTIAE);

	g_free(scores);
	g_free(s);
}

static void usage(void)
{
	fputs("usage: fpc-bench [options] IMAGE.pgm...\n"
	      "\n"
	      "  --sweep         try every preprocessing variant and report\n"
	      "                  which one works (all images must be of the\n"
	      "                  same finger)\n"
	      "  --scale N       enlarge the images N times (default 1)\n"
	      "  --normalize     stretch the histogram before analysing\n"
	      "  --no-invert     keep the polarity of the file (the driver\n"
	      "                  inverts, so inverting is the default)\n"
	      "  --partial       drop minutiae on the image perimeter\n"
	      "  --threshold N   mark scores >= N as a match (default 25)\n"
	      "\n"
	      "Capture a few images first:  fpc-capture loop right-\n",
	      stderr);
}

int main(int argc, char **argv)
{
	int scale = 1, do_normalize = 0, do_invert = 1, partial = 0;
	int threshold = 25, argi = 1, do_sweep = 0, n = 0;
	sample *samples;

	while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
		const char *opt = argv[argi++];

		if (!strcmp(opt, "--scale") && argi < argc)
			scale = atoi(argv[argi++]);
		else if (!strcmp(opt, "--threshold") && argi < argc)
			threshold = atoi(argv[argi++]);
		else if (!strcmp(opt, "--sweep"))
			do_sweep = 1;
		else if (!strcmp(opt, "--normalize"))
			do_normalize = 1;
		else if (!strcmp(opt, "--no-invert"))
			do_invert = 0;
		else if (!strcmp(opt, "--partial"))
			partial = 1;
		else {
			usage();
			return 2;
		}
	}
	if (argi >= argc || scale < 1) {
		usage();
		return 2;
	}

	if (do_sweep) {
		sweep(argv + argi, argc - argi, threshold);
		return 0;
	}

	samples = g_new0(sample, (size_t)(argc - argi));
	n = load_all(argv + argi, argc - argi, samples, scale, do_normalize,
		     do_invert, partial);

	if (!n) {
		fprintf(stderr, "no usable image\n");
		return 1;
	}

	printf("preprocessing: scale %dx, %s, %s, %s\n", scale,
	       do_normalize ? "normalized" : "raw contrast",
	       do_invert ? "inverted" : "as captured",
	       partial ? "perimeter points removed" : "all minutiae");
	printf("\n%-28s %6s %8s %12s\n", "image", "size", "minutiae",
	       "low contrast");
	for (int i = 0; i < n; i++)
		printf("%-28s %3ux%-3u %8d %11d%%\n", samples[i].name,
		       samples[i].width, samples[i].height,
		       samples[i].minutiae, samples[i].low_contrast_pct);

	printf("\nbozorth3 scores (threshold %d)\n\n%28s", threshold, "");
	for (int i = 0; i < n; i++)
		printf("%6d", i);
	putchar('\n');

	for (int i = 0; i < n; i++) {
		int probe_len = bozorth_probe_init(&samples[i].xyt);

		printf("%2d %-25.25s", i, samples[i].name);
		for (int j = 0; j < n; j++) {
			int score = bozorth_to_gallery(probe_len,
						       &samples[i].xyt,
						       &samples[j].xyt);

			printf("%6d", score);
		}
		putchar('\n');
	}

	printf("\nScores of two images of the same finger have to reach the\n"
	       "threshold, scores of different fingers have to stay below it.\n");

	for (int i = 0; i < n; i++)
		g_free(samples[i].data);
	g_free(samples);
	return 0;
}
