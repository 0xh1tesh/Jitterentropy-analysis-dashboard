/* Unit test for entropy_stats.h/.c; run via ctest. Pure C, no Windows dependency. */
#include "entropy_stats.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL line %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

#define CHECK_NEAR(a, b, tol) do { \
	double _a = (a), _b = (b), _t = (tol); \
	if (fabs(_a - _b) > _t) { \
		printf("FAIL line %d: %s (%.6f) not within %.6f of %s (%.6f)\n", \
		       __LINE__, #a, _a, _t, #b, _b); \
		fails++; \
	} \
} while (0)

/* xorshift32, deterministic, good enough to stand in for "random-looking" bytes. */
static uint32_t rng_state = 0xC0FFEEu;
static unsigned char next_byte(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (unsigned char)(rng_state & 0xff);
}

int main(void)
{
	unsigned char zeros[1024], ones_ff[1024], counter[1024], rnd[4096], alt[1024];
	size_t i;

	for (i = 0; i < sizeof(zeros); i++) zeros[i] = 0x00;
	for (i = 0; i < sizeof(ones_ff); i++) ones_ff[i] = 0xff;
	for (i = 0; i < sizeof(counter); i++) counter[i] = (unsigned char)(i & 0xff);
	for (i = 0; i < sizeof(rnd); i++) rnd[i] = next_byte();
	for (i = 0; i < sizeof(alt); i++) alt[i] = (i % 2) ? 0xff : 0x00;

	/* ---- histogram / Shannon / min-entropy ---- */
	{
		entropy_histogram h;
		entropy_stats_histogram(zeros, sizeof(zeros), &h);
		CHECK(h.total == sizeof(zeros));
		CHECK(h.counts[0] == sizeof(zeros));
		CHECK_NEAR(entropy_stats_shannon(&h), 0.0, 1e-9);      /* one symbol: 0 bits */
		CHECK_NEAR(entropy_stats_min_entropy(&h), 0.0, 1e-9);

		entropy_stats_histogram(counter, sizeof(counter), &h); /* 4x each of 256 values */
		CHECK_NEAR(entropy_stats_shannon(&h), 8.0, 1e-9);       /* uniform: exactly 8 bits */
		CHECK_NEAR(entropy_stats_min_entropy(&h), 8.0, 1e-9);

		entropy_stats_histogram(rnd, sizeof(rnd), &h);
		double sh = entropy_stats_shannon(&h);
		double me = entropy_stats_min_entropy(&h);
		CHECK(sh > 7.5 && sh <= 8.0);           /* near-uniform pseudo-random data */
		CHECK(me > 0.0 && me <= sh);            /* min-entropy never exceeds Shannon */
	}

	/* ---- chi-square: monotone in distance from uniform, p in [0,1] ---- */
	{
		entropy_histogram h_uniform, h_skewed, h_empty = {{0}, 0};
		entropy_stats_histogram(counter, sizeof(counter), &h_uniform); /* perfectly uniform */
		entropy_stats_histogram(zeros, sizeof(zeros), &h_skewed);      /* all one bin */

		entropy_chi_square cu = entropy_stats_chi_square(&h_uniform);
		entropy_chi_square cs = entropy_stats_chi_square(&h_skewed);
		entropy_chi_square ce = entropy_stats_chi_square(&h_empty);

		CHECK_NEAR(cu.statistic, 0.0, 1e-9);          /* exactly uniform: statistic 0 */
		CHECK(cu.p_value > 0.9 && cu.p_value <= 1.0);  /* far in the "too good" tail */
		CHECK(cs.statistic > cu.statistic);            /* skewed is farther from uniform */
		CHECK(cs.p_value < 1e-6);                      /* essentially impossible under H0 */
		CHECK(ce.p_value == 1.0);                      /* no data: vacuously "no deviation" */

		/* p-value near the mean of the distribution (statistic ~= df) should be ~0.5 */
		entropy_histogram h_mean;
		entropy_stats_histogram(rnd, sizeof(rnd), &h_mean);
		entropy_chi_square cm = entropy_stats_chi_square(&h_mean);
		CHECK(cm.p_value > 0.01 && cm.p_value < 0.99); /* pseudo-random data should pass */
	}

	/* ---- monobit ---- */
	{
		entropy_monobit m_zeros = entropy_stats_monobit(zeros, sizeof(zeros));
		CHECK(m_zeros.ones == 0);
		CHECK(m_zeros.p_value < 1e-6);                 /* all-zero: fails badly */

		entropy_monobit m_alt = entropy_stats_monobit(alt, sizeof(alt));
		CHECK(m_alt.ones == m_alt.bit_total / 2);       /* exactly balanced */
		CHECK_NEAR(m_alt.p_value, 1.0, 1e-9);           /* perfectly balanced: p == 1 */

		entropy_monobit m_rnd = entropy_stats_monobit(rnd, sizeof(rnd));
		CHECK(m_rnd.p_value > 0.01);                    /* pseudo-random passes */
	}

	/* ---- serial correlation ---- */
	{
		CHECK(isnan(entropy_stats_serial_correlation(zeros, sizeof(zeros)))); /* constant */
		CHECK(isnan(entropy_stats_serial_correlation(zeros, 1)));             /* too short */

		double scc_alt = entropy_stats_serial_correlation(alt, sizeof(alt));
		CHECK(scc_alt < -0.9);                          /* strict alternation: strongly anti-correlated */

		double scc_rnd = entropy_stats_serial_correlation(rnd, sizeof(rnd));
		CHECK(fabs(scc_rnd) < 0.1);                      /* pseudo-random: near zero */
	}

	/* ---- runs test ---- */
	{
		entropy_runs r_alt = entropy_stats_runs(alt, sizeof(alt));
		CHECK(r_alt.applicable);                          /* exactly balanced bits */
		/* Each byte is 8 identical bits (0x00 or 0xff); a run only ends at a
		 * byte boundary, so there is exactly one run per byte. */
		CHECK(r_alt.observed_runs == (int64_t)sizeof(alt));

		entropy_runs r_zeros = entropy_stats_runs(zeros, sizeof(zeros));
		CHECK(!r_zeros.applicable);                        /* proportion is 0, pre-check fails */

		entropy_runs r_rnd = entropy_stats_runs(rnd, sizeof(rnd));
		CHECK(r_rnd.applicable);
		CHECK(r_rnd.p_value > 0.01);                       /* pseudo-random passes */
	}

	/* ---- aggregate report matches the individual calls ---- */
	{
		entropy_report rep;
		entropy_stats_report(rnd, sizeof(rnd), &rep);
		CHECK(rep.byte_count == sizeof(rnd));
		entropy_histogram h;
		entropy_stats_histogram(rnd, sizeof(rnd), &h);
		CHECK_NEAR(rep.shannon_bits_per_byte, entropy_stats_shannon(&h), 1e-9);
		CHECK(rep.histogram[rnd[0]] > 0);
	}

	/* ---- RCT: a stuck timer must fail, varying samples must pass ---- */
	{
		double stuck[64], varying[64];
		for (i = 0; i < 64; i++) { stuck[i] = 3.14159; varying[i] = 1.0 + (double)(i % 7); }

		entropy_rct_result r_stuck = entropy_stats_rct(stuck, 64, 1.0 / 1048576.0, 1.0);
		CHECK(!r_stuck.passed);
		CHECK(r_stuck.max_run == 64);

		entropy_rct_result r_varying = entropy_stats_rct(varying, 64, 1.0 / 1048576.0, 1.0);
		CHECK(r_varying.passed);
		CHECK(r_varying.max_run < r_stuck.max_run);

		/* alpha=2^-20, H=1 bit: C = 1 + ceil(20/1) = 21 */
		entropy_rct_result r_cutoff = entropy_stats_rct(NULL, 0, 1.0 / 1048576.0, 1.0);
		CHECK(r_cutoff.cutoff == 21);
	}

	/* ---- APT: cross-check the cutoff against the vendor library's own table.
	 * jitterentropy-health.c's osr=1, alpha=2^-30 cutoff (looked up from its own
	 * precomputed table, derived by exact binomial inversion) is 325. Our normal
	 * approximation is documented as accurate to within a count or two. ---- */
	{
		entropy_apt_result r = entropy_stats_apt(NULL, 0, 512, 1.0 / 1073741824.0 /* 2^-30 */, 1.0);
		CHECK(r.window_size == 512);
		CHECK(r.cutoff >= 322 && r.cutoff <= 328);

		/* A window that repeats its first value throughout must fail. */
		double stuck_window[512];
		for (i = 0; i < 512; i++) stuck_window[i] = 7.0;
		entropy_apt_result r_stuck = entropy_stats_apt(stuck_window, 512, 512, 1.0 / 1048576.0, 1.0);
		CHECK(!r_stuck.passed);
		CHECK(r_stuck.windows_failed == 1);

		/* Enough distinct values per window must pass. */
		double varying_windows[1024];
		for (i = 0; i < 1024; i++) varying_windows[i] = (double)(i % 37);
		entropy_apt_result r_ok = entropy_stats_apt(varying_windows, 1024, 512, 1.0 / 1048576.0, 1.0);
		CHECK(r_ok.windows_tested == 2);
		CHECK(r_ok.passed);
	}

	printf(fails ? "FAILED (%d)\n" : "ALL PASSED\n", fails);
	return fails ? 1 : 0;
}
