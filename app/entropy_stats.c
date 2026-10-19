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
 * SP 800-90B CRITBINOM(n, p, 1 - alpha): the smallest k with
 * P(Binomial(n, p) <= k) >= 1 - alpha, i.e. P(X >= k + 1) <= alpha.
 * Exact inversion: the upper tail is summed from k = n downwards in log
 * space, so tiny alphas (2^-30 and below) and large windows neither
 * underflow nor lose precision. Not part of the public API.
 */
static unsigned int crit_binom(unsigned int n, double p, double alpha)
{
	double log_p, log_q, log_n_fact, tail = 0.0;
	unsigned int k;

	if (p <= 0.0)
		return 0;
	if (p >= 1.0)
		return n;

	log_p = log(p);
	log_q = log1p(-p);
	log_n_fact = lgamma((double)n + 1.0);

	/* Invariant at the top of each pass: tail = P(X >= k + 1) <= alpha. */
	for (k = n;; k--) {
		double log_pmf = log_n_fact - lgamma((double)k + 1.0) -
				 lgamma((double)(n - k) + 1.0) +
				 (double)k * log_p + (double)(n - k) * log_q;

		tail += exp(log_pmf); /* tail = P(X >= k) */
		if (tail > alpha || k == 0)
			return k;
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
	size_t start;

	if (!window_size)
		window_size = 512;
	alpha = normalize_alpha(alpha);
	h_bits = normalize_h(h_bits);
	r.window_size = window_size;

	/*
	 * SP 800-90B section 4.4.2: C = 1 + CRITBINOM(W, 2^-H, 1 - alpha).
	 * A window fails when its first value occurs C or more times in it
	 * (the count includes the first sample itself).
	 */
	r.cutoff = 1 + crit_binom(window_size, pow(2.0, -h_bits), alpha);

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
