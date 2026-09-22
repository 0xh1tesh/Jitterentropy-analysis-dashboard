#include "entropy_stats.h"

#include <math.h>
#include <string.h>

void entropy_stats_histogram(const unsigned char *data, size_t len,
			     entropy_histogram *out)
{
	size_t i;

	memset(out, 0, sizeof(*out));
	if (!data)
		return;
	for (i = 0; i < len; i++)
		out->counts[data[i]]++;
	out->total = len;
}

double entropy_stats_shannon(const entropy_histogram *h)
{
	double bits = 0.0;
	int i;

	if (!h->total)
		return 0.0;

	for (i = 0; i < ENTROPY_STATS_BINS; i++) {
		double p;

		if (!h->counts[i])
			continue;
		p = (double)h->counts[i] / (double)h->total;
		bits -= p * log2(p);
	}
	return bits;
}

double entropy_stats_min_entropy(const entropy_histogram *h)
{
	uint64_t max_count = 0;
	double p_max;
	int i;

	if (!h->total)
		return 0.0;

	for (i = 0; i < ENTROPY_STATS_BINS; i++) {
		if (h->counts[i] > max_count)
			max_count = h->counts[i];
	}
	p_max = (double)max_count / (double)h->total;
	return -log2(p_max);
}

entropy_chi_square entropy_stats_chi_square(const entropy_histogram *h)
{
	entropy_chi_square r = {0.0, 1.0};
	double expected, df, ratio, term, denom, z;
	int i;

	if (!h->total)
		return r;

	expected = (double)h->total / ENTROPY_STATS_BINS;
	for (i = 0; i < ENTROPY_STATS_BINS; i++) {
		double diff = (double)h->counts[i] - expected;

		r.statistic += diff * diff / expected;
	}

	/* Wilson-Hilferty cube-root normal approximation; see entropy_stats.h. */
	df = (double)(ENTROPY_STATS_BINS - 1);
	ratio = r.statistic / df;
	term = 1.0 - 2.0 / (9.0 * df);
	denom = sqrt(2.0 / (9.0 * df));
	z = (pow(ratio, 1.0 / 3.0) - term) / denom;
	r.p_value = 0.5 * erfc(z / sqrt(2.0));
	return r;
}

entropy_monobit entropy_stats_monobit(const unsigned char *data, size_t len)
{
	entropy_monobit r = {0, 0, 0.0, 1.0};
	double s;
	size_t i;

	r.bit_total = (int64_t)len * 8;
	if (!r.bit_total)
		return r;

	for (i = 0; i < len; i++) {
		unsigned char b = data[i];
		int bit;

		for (bit = 0; bit < 8; bit++)
			r.ones += (b >> bit) & 1;
	}

	/* SP 800-22 2.1: S_n = (#ones - #zeros); s_obs = |S_n| / sqrt(n). */
	s = (double)(2 * r.ones - r.bit_total);
	r.statistic = fabs(s) / sqrt((double)r.bit_total);
	r.p_value = erfc(r.statistic / sqrt(2.0));
	return r;
}

double entropy_stats_serial_correlation(const unsigned char *data, size_t len)
{
	double sum_xy = 0.0, sum_x = 0.0, sum_x2 = 0.0, num, den;
	size_t i, n = len;

	if (len < 2)
		return NAN;

	/* Knuth vol. 2 / the `ent` tool's formula, with wraparound. */
	for (i = 0; i < n; i++) {
		double xi = (double)data[i];
		double xi1 = (double)data[(i + 1) % n];

		sum_xy += xi * xi1;
		sum_x += xi;
		sum_x2 += xi * xi;
	}

	num = (double)n * sum_xy - sum_x * sum_x;
	den = (double)n * sum_x2 - sum_x * sum_x;
	if (den == 0.0)
		return NAN; /* every byte identical: correlation is undefined */
	return num / den;
}

entropy_runs entropy_stats_runs(const unsigned char *data, size_t len)
{
	entropy_runs r = {0, 0, 0.0, false, 0, 0.0};
	int prev_bit = -1;
	double pi, tau, vobs, num, den;
	size_t i;

	r.bit_total = (int64_t)len * 8;
	if (r.bit_total < 2)
		return r;

	for (i = 0; i < len; i++) {
		unsigned char byte = data[i];
		int bitpos;

		for (bitpos = 0; bitpos < 8; bitpos++) {
			int bit = (byte >> bitpos) & 1;

			r.ones += bit;
			if (prev_bit < 0)
				r.observed_runs = 1;
			else if (bit != prev_bit)
				r.observed_runs++;
			prev_bit = bit;
		}
	}

	pi = (double)r.ones / (double)r.bit_total;
	r.proportion = pi;

	/* SP 800-22 2.3's own pre-condition on the runs test's applicability. */
	tau = 2.0 / sqrt((double)r.bit_total);
	if (fabs(pi - 0.5) >= tau) {
		r.applicable = false;
		r.p_value = 0.0;
		return r;
	}
	r.applicable = true;

	vobs = (double)r.observed_runs;
	num = fabs(vobs - 2.0 * (double)r.bit_total * pi * (1.0 - pi));
	den = 2.0 * sqrt(2.0 * (double)r.bit_total) * pi * (1.0 - pi);
	r.p_value = erfc(num / den);
	return r;
}

void entropy_stats_report(const unsigned char *data, size_t len,
			  entropy_report *out)
{
	entropy_histogram h;

	memset(out, 0, sizeof(*out));
	out->byte_count = len;

	entropy_stats_histogram(data, len, &h);
	memcpy(out->histogram, h.counts, sizeof(out->histogram));

	out->shannon_bits_per_byte = entropy_stats_shannon(&h);
	out->min_entropy_bits_per_byte = entropy_stats_min_entropy(&h);
	out->chi_square = entropy_stats_chi_square(&h);
	out->monobit = entropy_stats_monobit(data, len);
	out->serial_correlation = entropy_stats_serial_correlation(data, len);
	out->runs = entropy_stats_runs(data, len);
}

/*
 * Peter Acklam's rational approximation to the inverse standard normal CDF
 * (relative error <= 1.15e-9). Used only to turn an alpha into a z-score for
 * the APT cutoff below; not part of the public API.
 */
static double inv_norm_cdf(double p)
{
	static const double a[6] = {
		-3.969683028665376e+01, 2.209460984245205e+02,
		-2.759285104469687e+02, 1.383577518672690e+02,
		-3.066479806614716e+01, 2.506628277459239e+00
	};
	static const double b[5] = {
		-5.447609879822406e+01, 1.615858368580409e+02,
		-1.556989798598866e+02, 6.680131188771972e+01,
		-1.328068155288572e+01
	};
	static const double c[6] = {
		-7.784894002430293e-03, -3.223964580411365e-01,
		-2.400758277161838e+00, -2.549732539343734e+00,
		4.374664141464968e+00, 2.938163982698783e+00
	};
	static const double d[4] = {
		7.784695709041462e-03, 3.224671290700398e-01,
		2.445134137142996e+00, 3.754408661907416e+00
	};
	const double p_low = 0.02425, p_high = 1.0 - p_low;
	double q, r;

	if (p <= 0.0)
		return -HUGE_VAL;
	if (p >= 1.0)
		return HUGE_VAL;

	if (p < p_low) {
		q = sqrt(-2.0 * log(p));
		return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
		       ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	} else if (p <= p_high) {
		q = p - 0.5;
		r = q * q;
		return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
		       (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
	} else {
		q = sqrt(-2.0 * log(1.0 - p));
		return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
		       ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
}

static double normalize_alpha(double alpha)
{
	return (alpha > 0.0 && alpha < 1.0) ? alpha : (1.0 / 1048576.0); /* 2^-20 */
}

static double normalize_h(double h_bits)
{
	return h_bits > 0.0 ? h_bits : 1.0;
}

entropy_rct_result entropy_stats_rct(const double *samples, size_t n,
				     double alpha, double h_bits)
{
	entropy_rct_result r = {0};
	unsigned int run, max_run;
	size_t i;

	alpha = normalize_alpha(alpha);
	h_bits = normalize_h(h_bits);

	/* SP 800-90B section 4.4.1: C = 1 + ceil(-log2(alpha) / H). */
	r.cutoff = (unsigned int)(1.0 + ceil(-log2(alpha) / h_bits));

	if (!samples || n == 0) {
		r.passed = true;
		return r;
	}

	run = 1;
	max_run = 1;
	for (i = 1; i < n; i++) {
		if (samples[i] == samples[i - 1]) {
			run++;
			if (run > max_run)
				max_run = run;
		} else {
			run = 1;
		}
	}
	r.max_run = max_run;
	r.passed = max_run < r.cutoff;
	return r;
}

entropy_apt_result entropy_stats_apt(const double *samples, size_t n,
				     unsigned int window_size, double alpha,
				     double h_bits)
{
	entropy_apt_result r = {0};
	double p, trials, mean, sd, z, cutoff_d;
	size_t start;

	if (!window_size)
		window_size = 512;
	alpha = normalize_alpha(alpha);
	h_bits = normalize_h(h_bits);
	r.window_size = window_size;

	/*
	 * Normal approximation to the exact SP 800-90B binomial quantile (see
	 * entropy_stats.h): p is the worst-case probability that any two
	 * samples collide, given h_bits of min-entropy per sample.
	 */
	p = pow(2.0, -h_bits);
	trials = (double)(window_size - 1);
	mean = trials * p;
	sd = sqrt(trials * p * (1.0 - p));
	z = inv_norm_cdf(1.0 - alpha);
	cutoff_d = mean + z * sd + 0.5; /* continuity correction */
	if (cutoff_d < 1.0)
		cutoff_d = 1.0;
	r.cutoff = (unsigned int)ceil(cutoff_d);

	if (!samples || n < window_size) {
		r.passed = true;
		return r;
	}

	for (start = 0; start + window_size <= n; start += window_size) {
		double first = samples[start];
		unsigned int count = 0;
		size_t i;

		for (i = start; i < start + window_size; i++) {
			if (samples[i] == first)
				count++;
		}
		r.windows_tested++;
		if (count >= r.cutoff)
			r.windows_failed++;
	}
	r.passed = (r.windows_failed == 0);
	return r;
}
