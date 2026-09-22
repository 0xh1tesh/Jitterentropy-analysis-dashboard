#ifndef RNG_WRAPPER_H
#define RNG_WRAPPER_H

#include <stdbool.h>
#include <stddef.h>

#define RNG_ERR_SELF_TEST_FAILED   (-1001)
#define RNG_ERR_ALLOC_FAILED       (-1002)
#define RNG_ERR_NOT_INITIALIZED    (-1003)
#define RNG_ERR_SHORT_READ         (-1004)
#define RNG_ERR_INVALID_ARGUMENT   (-1005)
#define RNG_ERR_GENERATION_FAILED  (-1006)
#define RNG_ERR_BUSY               (-1007)
#define RNG_ERR_ALREADY_RUNNING    (-1008)

/* A job stopped cooperatively on request; not a failure of the RNG itself. */
#define RNG_RESULT_CANCELLED       (-2001)

int init_rng(void);
int get_random_bytes(unsigned char *buffer, size_t len);
void shutdown_rng(void);

/* Wipe key material; not optimised away like memset before free(). */
void rng_secure_zero(void *ptr, size_t len);

/*
 * Collector configuration. jent_entropy_collector_alloc(0, 0), the previous
 * hardcoded call, is osr=0 (library picks a default) with every optional
 * flag off; rng_settings_defaults() reproduces exactly that.
 */
typedef struct rng_settings {
	unsigned int osr;             /* 0 = library default oversampling rate */
	bool force_fips;               /* JENT_FORCE_FIPS: full SP800-90B enforcement */
	bool ntg1;                     /* JENT_NTG1: AIS 20/31 NTG.1 compliance */
	bool disable_memory_access;    /* JENT_DISABLE_MEMORY_ACCESS: less entropy, less RAM */
} rng_settings;

void rng_settings_defaults(rng_settings *out);

/*
 * Takes effect on the next init_rng() that actually (re)allocates a
 * collector. Returns RNG_ERR_ALREADY_RUNNING and leaves the stored settings
 * unchanged if a collector is currently allocated -- shut it down first.
 */
int rng_configure(const rng_settings *settings);
void rng_get_settings(rng_settings *out);

/* jent_version()'s packed MAJOR*1000000+MINOR*10000+PATCH*100; 0 before init. */
unsigned int rng_get_library_version(void);

/*
 * jent_status()'s own JSON status report for the current collector (UUID,
 * reinit count, output accounting, RCT/APT/Lag intermittent/permanent
 * flags, runtime environment, configuration) -- the library's authoritative
 * self-report, not an estimate: this is RCT/APT/Lag state from the
 * library's real internal noise-source tests, not the entropy_stats.h
 * module's diagnostic overlay on a proxy signal. Returns 0 and a
 * NUL-terminated JSON object in buf on success, -1 (buf untouched) if there
 * is no collector or buflen is too small.
 */
int rng_get_status_json(char *buf, size_t buflen);

#endif
