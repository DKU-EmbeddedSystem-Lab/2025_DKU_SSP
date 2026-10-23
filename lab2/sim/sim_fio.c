#include "sim_fio.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum rw_mode {
	RW_SEQ_READ,
	RW_SEQ_WRITE,
	RW_SEQ_MIXED,
	RW_RANDOM_READ,
	RW_RANDOM_WRITE,
	RW_RANDOM_MIXED
};

struct job_state {
	struct sim_fio_job job;
	unsigned int numjobs;
	enum rw_mode rw_mode;
	unsigned int mix_read;
	int mix_set;
};

static void set_error(char *err, size_t errsz, const char *format, ...)
{
	va_list ap;

	if (err == NULL || errsz == 0)
		return;

	va_start(ap, format);
	(void)vsnprintf(err, errsz, format, ap);
	va_end(ap);
}

static int char_equal_ci(char lhs, char rhs)
{
	return tolower((unsigned char)lhs) == tolower((unsigned char)rhs);
}

static int string_equal_ci(const char *lhs, const char *rhs)
{
	while (*lhs != '\0' && *rhs != '\0') {
		if (!char_equal_ci(*lhs, *rhs))
			return 0;
		lhs++;
		rhs++;
	}
	return *lhs == '\0' && *rhs == '\0';
}

static int string_starts_ci(const char *string, const char *prefix)
{
	while (*prefix != '\0') {
		if (*string == '\0' || !char_equal_ci(*string, *prefix))
			return 0;
		string++;
		prefix++;
	}
	return 1;
}

static char *trim(char *string)
{
	char *end;

	while (isspace((unsigned char)*string))
		string++;

	end = string + strlen(string);
	while (end != string && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return string;
}

/* A comment begins at the start of a line or after whitespace. */
static void strip_comment(char *line)
{
	char *cursor;

	for (cursor = line; *cursor != '\0'; cursor++) {
		if ((*cursor == '#' || *cursor == ';') &&
		    (cursor == line || isspace((unsigned char)cursor[-1]))) {
			*cursor = '\0';
			return;
		}
	}
}

/*
 * A small standard-C replacement for getline().  The returned buffer is
 * reused between calls and includes neither the newline nor a trailing CR.
 */
static int read_line(FILE *file, char **buffer, size_t *capacity,
		     size_t *length)
{
	int ch;
	size_t used = 0;

	if (*buffer == NULL || *capacity == 0) {
		*capacity = 256;
		*buffer = (char *)malloc(*capacity);
		if (*buffer == NULL)
			return -1;
	}

	while ((ch = fgetc(file)) != EOF) {
		if (ch == '\n')
			break;
		if (ch == '\0')
			return -2;
		if (used + 1 >= *capacity) {
			size_t next_capacity;
			char *next_buffer;

			if (*capacity > SIZE_MAX / 2)
				return -1;
			next_capacity = *capacity * 2;
			next_buffer = (char *)realloc(*buffer, next_capacity);
			if (next_buffer == NULL)
				return -1;
			*buffer = next_buffer;
			*capacity = next_capacity;
		}
		(*buffer)[used++] = (char)ch;
	}

	if (ch == EOF && used == 0)
		return 0;
	if (used != 0 && (*buffer)[used - 1] == '\r')
		used--;
	(*buffer)[used] = '\0';
	*length = used;
	return 1;
}

static int parse_decimal_u64(const char *value, uint64_t *result)
{
	uint64_t number = 0;
	const unsigned char *cursor = (const unsigned char *)value;

	if (*cursor == '\0')
		return -1;
	while (*cursor != '\0') {
		unsigned int digit;

		if (!isdigit(*cursor))
			return -1;
		digit = (unsigned int)(*cursor - '0');
		if (number > (UINT64_MAX - digit) / 10)
			return -2;
		number = number * 10 + digit;
		cursor++;
	}
	*result = number;
	return 0;
}

static int suffix_equal_ci(const char *suffix, const char *expected)
{
	return string_equal_ci(suffix, expected);
}

/* Seeds are integers, not byte sizes; accept decimal and explicit hex. */
static int parse_seed_u64(const char *value, uint64_t *result)
{
	const unsigned char *cursor = (const unsigned char *)value;
	uint64_t number = 0;

	if (cursor[0] != '0' || (cursor[1] != 'x' && cursor[1] != 'X'))
		return parse_decimal_u64(value, result);
	cursor += 2;
	if (*cursor == '\0')
		return -1;
	while (*cursor != '\0') {
		unsigned int digit;

		if (*cursor >= '0' && *cursor <= '9')
			digit = *cursor - '0';
		else if (*cursor >= 'a' && *cursor <= 'f')
			digit = *cursor - 'a' + 10;
		else if (*cursor >= 'A' && *cursor <= 'F')
			digit = *cursor - 'A' + 10;
		else
			return -1;
		if (number > (UINT64_MAX - digit) / 16)
			return -2;
		number = number * 16 + digit;
		cursor++;
	}
	*result = number;
	return 0;
}

/* Parse fio sizes using its default kb_base=1024 compatibility rules. */
static int parse_size_bytes(const char *value, int allow_zero,
			    uint64_t *result)
{
	char digits[32];
	size_t digit_count = 0;
	const char *suffix;
	uint64_t number;
	uint64_t multiplier;
	int status;

	while (isdigit((unsigned char)value[digit_count])) {
		if (digit_count + 1 >= sizeof(digits))
			return -2;
		digits[digit_count] = value[digit_count];
		digit_count++;
	}
	if (digit_count == 0)
		return -1;
	digits[digit_count] = '\0';
	suffix = value + digit_count;

	status = parse_decimal_u64(digits, &number);
	if (status != 0)
		return status;
	if (!allow_zero && number == 0)
		return -3;

	if (*suffix == '\0' || suffix_equal_ci(suffix, "b"))
		multiplier = UINT64_C(1);
	else if (suffix_equal_ci(suffix, "k") ||
		 suffix_equal_ci(suffix, "kb"))
		multiplier = UINT64_C(1024);
	else if (suffix_equal_ci(suffix, "ki") ||
		 suffix_equal_ci(suffix, "kib"))
		multiplier = UINT64_C(1000);
	else if (suffix_equal_ci(suffix, "m") ||
		 suffix_equal_ci(suffix, "mb"))
		multiplier = UINT64_C(1024) * UINT64_C(1024);
	else if (suffix_equal_ci(suffix, "mi") ||
		 suffix_equal_ci(suffix, "mib"))
		multiplier = UINT64_C(1000) * UINT64_C(1000);
	else if (suffix_equal_ci(suffix, "g") ||
		 suffix_equal_ci(suffix, "gb"))
		multiplier = UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024);
	else if (suffix_equal_ci(suffix, "gi") ||
		 suffix_equal_ci(suffix, "gib"))
		multiplier = UINT64_C(1000) * UINT64_C(1000) * UINT64_C(1000);
	else if (suffix_equal_ci(suffix, "t") ||
		 suffix_equal_ci(suffix, "tb"))
		multiplier = UINT64_C(1024) * UINT64_C(1024) *
			     UINT64_C(1024) * UINT64_C(1024);
	else if (suffix_equal_ci(suffix, "ti") ||
		 suffix_equal_ci(suffix, "tib"))
		multiplier = UINT64_C(1000) * UINT64_C(1000) *
			     UINT64_C(1000) * UINT64_C(1000);
	else
		return -1;

	if (number > UINT64_MAX / multiplier)
		return -2;
	*result = number * multiplier;
	return 0;
}

static int parse_runtime_us(const char *value, uint64_t *result)
{
	char digits[32];
	size_t digit_count = 0;
	const char *suffix;
	uint64_t number;
	uint64_t multiplier;
	int status;

	while (isdigit((unsigned char)value[digit_count])) {
		if (digit_count + 1 >= sizeof(digits))
			return -2;
		digits[digit_count] = value[digit_count];
		digit_count++;
	}
	if (digit_count == 0)
		return -1;
	digits[digit_count] = '\0';
	suffix = value + digit_count;
	status = parse_decimal_u64(digits, &number);
	if (status != 0)
		return status;
	if (number == 0)
		return -3;

	/* fio treats an unsuffixed runtime as seconds. */
	if (*suffix == '\0' || suffix_equal_ci(suffix, "s") ||
	    suffix_equal_ci(suffix, "sec"))
		multiplier = UINT64_C(1000000);
	else if (suffix_equal_ci(suffix, "us") ||
		 suffix_equal_ci(suffix, "usec"))
		multiplier = UINT64_C(1);
	else if (suffix_equal_ci(suffix, "ms") ||
		 suffix_equal_ci(suffix, "msec"))
		multiplier = UINT64_C(1000);
	else if (suffix_equal_ci(suffix, "m") ||
		 suffix_equal_ci(suffix, "min"))
		multiplier = UINT64_C(60) * UINT64_C(1000000);
	else if (suffix_equal_ci(suffix, "h") ||
		 suffix_equal_ci(suffix, "hour"))
		multiplier = UINT64_C(60) * UINT64_C(60) * UINT64_C(1000000);
	else if (suffix_equal_ci(suffix, "d") ||
		 suffix_equal_ci(suffix, "day"))
		multiplier = UINT64_C(24) * UINT64_C(60) * UINT64_C(60) *
			     UINT64_C(1000000);
	else
		return -1;

	if (number > UINT64_MAX / multiplier)
		return -2;
	*result = number * multiplier;
	return 0;
}

static int parse_bool(const char *value, int bare, int *result)
{
	if (bare || *value == '\0' || string_equal_ci(value, "1") ||
	    string_equal_ci(value, "true") || string_equal_ci(value, "yes") ||
	    string_equal_ci(value, "on")) {
		*result = 1;
		return 0;
	}
	if (string_equal_ci(value, "0") || string_equal_ci(value, "false") ||
	    string_equal_ci(value, "no") || string_equal_ci(value, "off")) {
		*result = 0;
		return 0;
	}
	return -1;
}

static void refresh_rw_mix(struct job_state *state)
{
	unsigned int mixed_read = state->mix_set ? state->mix_read : 50;

	switch (state->rw_mode) {
	case RW_SEQ_READ:
		state->job.pattern = 0;
		state->job.read_percent = 100;
		state->job.write_percent = 0;
		break;
	case RW_SEQ_WRITE:
		state->job.pattern = 0;
		state->job.read_percent = 0;
		state->job.write_percent = 100;
		break;
	case RW_SEQ_MIXED:
		state->job.pattern = 0;
		state->job.read_percent = (int)mixed_read;
		state->job.write_percent = (int)(100 - mixed_read);
		break;
	case RW_RANDOM_READ:
		state->job.pattern = 1;
		state->job.read_percent = 100;
		state->job.write_percent = 0;
		break;
	case RW_RANDOM_WRITE:
		state->job.pattern = 1;
		state->job.read_percent = 0;
		state->job.write_percent = 100;
		break;
	case RW_RANDOM_MIXED:
		state->job.pattern = 1;
		state->job.read_percent = (int)mixed_read;
		state->job.write_percent = (int)(100 - mixed_read);
		break;
	}
}

static void init_job_state(struct job_state *state)
{
	memset(state, 0, sizeof(*state));
	state->numjobs = 1;
	state->rw_mode = RW_SEQ_READ;
	state->job.block_size_bytes = UINT64_C(4096);
	state->job.iodepth = 1;
	state->job.randrepeat = 1;
	refresh_rw_mix(state);
}

static int set_rw_mode(struct job_state *state, const char *value)
{
	if (string_equal_ci(value, "read"))
		state->rw_mode = RW_SEQ_READ;
	else if (string_equal_ci(value, "write"))
		state->rw_mode = RW_SEQ_WRITE;
	else if (string_equal_ci(value, "rw") ||
		 string_equal_ci(value, "readwrite"))
		state->rw_mode = RW_SEQ_MIXED;
	else if (string_equal_ci(value, "randread"))
		state->rw_mode = RW_RANDOM_READ;
	else if (string_equal_ci(value, "randwrite"))
		state->rw_mode = RW_RANDOM_WRITE;
	else if (string_equal_ci(value, "randrw"))
		state->rw_mode = RW_RANDOM_MIXED;
	else
		return -1;
	refresh_rw_mix(state);
	return 0;
}

static int option_in_list(const char *key, const char *const *list,
			  size_t list_length)
{
	size_t index;

	for (index = 0; index < list_length; index++) {
		if (string_equal_ci(key, list[index]))
			return 1;
	}
	return 0;
}

/* Options which do not change the simulator's generated block workload. */
static int is_ignored_option(const char *key)
{
	static const char *const ignored[] = {
		"allow_file_create", "atomic", "buffer_compress_chunk",
		"buffer_compress_percentage", "buffer_pattern", "buffered",
		"bwavgtime", "clat_percentiles", "clocksource", "cmdprio",
		"cmdprio_bssplit", "cmdprio_class", "cmdprio_percentage",
		"continue_on_error", "cpus_allowed", "cpus_allowed_policy",
		"create_on_open", "description", "direct", "disable_bw_measurement",
		"disable_clat", "disable_lat", "disable_slat", "disk_util",
		"directory", "error_dump", "eta", "eta_newline",
		"exitall_on_error", "fadvise_hint", "fallocate", "filename",
		"filename_format", "fixedbufs",
		"gid", "group_reporting", "gtod_cpu", "gtod_reduce", "hipri",
		"hugepage_size", "ignore_error", "invalidate", "ioengine",
		"iomem", "iomem_align", "iopsavgtime", "lat_percentiles",
		"lockfile", "log_alternate_epoch", "log_avg_msec",
		"log_compression", "log_entries", "log_hist_coarseness",
		"log_hist_msec", "log_store_compressed", "log_unix_epoch", "mem",
		"mmaphuge", "nice", "numa_cpu_nodes",
		"numa_mem_policy", "output", "output-format",
		"output_format", "overwrite", "per_job_logs", "percentile_list",
		"prio", "prioclass", "refill_buffers",
		"registerfiles", "scramble_buffers", "significant_figures",
		"slat_percentiles", "sqthread_poll", "sqthread_poll_cpu",
		"status_interval", "terse_version", "thread", "uid", "unified_rw_reporting",
		"unique_filename", "unit_base", "unlink", "unlink_each_loop",
		"userspace_reap", "write_bw_log", "write_hint", "write_hist_log",
		"write_iops_log", "write_lat_log", "zero_buffers"
	};

	return option_in_list(key, ignored, sizeof(ignored) / sizeof(ignored[0]));
}

/* Recognized fio controls which this simulator cannot faithfully represent. */
static int is_unsupported_workload_option(const char *key)
{
	static const char *const unsupported[] = {
		"ba", "blockalign", "bsrange", "bssplit", "exitall",
		"end_fsync", "fdatasync", "file_append", "file_service_type",
		"flow", "flow_id", "flow_sleep",
		"flow_watermark", "fsync", "iodepth_batch", "iodepth_batch_complete",
		"iodepth_batch_submit", "iodepth_low", "kb_base", "latency_percentile",
		"latency_target", "latency_window", "loops", "max_latency",
		"new_group", "nrfiles", "offset_align", "openfiles",
		"percentage_random", "pre_read", "ramp_time", "readonly",
		"rate", "rate_cycle", "rate_ignore_thinktime", "rate_iops",
		"rate_iops_min", "rate_min", "rate_process", "rw_sequencer",
		"serialize_overlap", "startdelay", "stonewall", "sync_file_range",
		"thinktime", "thinktime_blocks", "thinktime_iotime", "thinktime_spin",
		"write_barrier", "zonemode", "zonerange", "zonesize", "zoneskip"
	};

	if (option_in_list(key, unsupported,
			   sizeof(unsupported) / sizeof(unsupported[0])))
		return 1;
	/* fio has several log/report keys; do not mistake them for pacing keys. */
	if (string_starts_ci(key, "rate_") ||
	    string_starts_ci(key, "thinktime_") ||
	    string_starts_ci(key, "flow_") ||
	    string_starts_ci(key, "zone_"))
		return 1;
	return 0;
}

static int parse_small_unsigned(const char *value, unsigned int minimum,
				unsigned int maximum, unsigned int *result)
{
	uint64_t number;
	int status = parse_decimal_u64(value, &number);

	if (status != 0)
		return status;
	if (number < minimum || number > maximum)
		return -3;
	*result = (unsigned int)number;
	return 0;
}

static int parse_option(struct job_state *state, const char *key,
			const char *value, int bare, size_t line_number,
			char *err, size_t errsz)
{
	uint64_t number;
	unsigned int small;
	int status;

	if (string_equal_ci(key, "rw") || string_equal_ci(key, "readwrite")) {
		if (bare || *value == '\0' || set_rw_mode(state, value) != 0) {
			set_error(err, errsz, "line %lu: unsupported rw value '%s'",
				  (unsigned long)line_number, value);
			return -1;
		}
		return 0;
	}

	if (string_equal_ci(key, "rwmixread") ||
	    string_equal_ci(key, "rwmixwrite")) {
		status = parse_small_unsigned(value, 0, 100, &small);
		if (bare || status != 0) {
			set_error(err, errsz,
				  "line %lu: %s must be an integer from 0 to 100",
				  (unsigned long)line_number, key);
			return -1;
		}
		state->mix_read = string_equal_ci(key, "rwmixread") ? small : 100 - small;
		state->mix_set = 1;
		refresh_rw_mix(state);
		return 0;
	}

	if (string_equal_ci(key, "bs") || string_equal_ci(key, "blocksize")) {
		status = parse_size_bytes(value, 0, &number);
		if (bare || status != 0) {
			set_error(err, errsz,
				  "line %lu: %s must be one non-zero binary byte size",
				  (unsigned long)line_number, key);
			return -1;
		}
		state->job.block_size_bytes = number;
		return 0;
	}

	if (string_equal_ci(key, "size")) {
		size_t value_length = strlen(value);

		if (!bare && value_length > 1 && value[value_length - 1] == '%') {
			char percent[32];

			if (value_length >= sizeof(percent))
				status = -1;
			else {
				memcpy(percent, value, value_length - 1);
				percent[value_length - 1] = '\0';
				status = parse_small_unsigned(percent, 1, 100, &small);
			}
			if (status != 0) {
				set_error(err, errsz,
					  "line %lu: size percentage must be from 1%% to 100%%",
					  (unsigned long)line_number);
				return -1;
			}
			state->job.region_size_bytes = 0;
			state->job.region_size_percent = small;
			state->job.size_set = 1;
			return 0;
		}
		status = parse_size_bytes(value, 0, &number);
		if (bare || status != 0) {
			set_error(err, errsz,
				  "line %lu: size must be a non-zero byte size or percentage",
				  (unsigned long)line_number);
			return -1;
		}
		state->job.region_size_bytes = number;
		state->job.region_size_percent = 0;
		state->job.size_set = 1;
		return 0;
	}

	if (string_equal_ci(key, "io_size") || string_equal_ci(key, "io_limit")) {
		status = parse_size_bytes(value, 0, &number);
		if (bare || status != 0) {
			set_error(err, errsz, "line %lu: %s must be a non-zero byte size",
				  (unsigned long)line_number, key);
			return -1;
		}
		state->job.io_size_bytes = number;
		state->job.io_size_set = 1;
		return 0;
	}

	if (string_equal_ci(key, "offset") ||
	    string_equal_ci(key, "offset_increment")) {
		int allow_zero = string_equal_ci(key, "offset");

		status = parse_size_bytes(value, allow_zero, &number);
		if (bare || status != 0) {
			set_error(err, errsz, "line %lu: %s must be %sa valid byte size",
				  (unsigned long)line_number, key,
				  allow_zero ? "" : "a non-zero ");
			return -1;
		}
		if (allow_zero) {
			state->job.offset_bytes = number;
			state->job.offset_set = 1;
		} else {
			state->job.offset_increment_bytes = number;
			state->job.offset_increment_set = 1;
		}
		return 0;
	}

	if (string_equal_ci(key, "number_ios")) {
		status = parse_decimal_u64(value, &number);
		if (bare || status != 0 || number == 0) {
			set_error(err, errsz,
				  "line %lu: number_ios must be a non-zero integer",
				  (unsigned long)line_number);
			return -1;
		}
		state->job.number_ios = number;
		state->job.number_ios_set = 1;
		return 0;
	}

	if (string_equal_ci(key, "iodepth")) {
		status = parse_small_unsigned(value, 1, UINT_MAX, &small);
		if (bare || status != 0) {
			set_error(err, errsz, "line %lu: iodepth must be a non-zero integer",
				  (unsigned long)line_number);
			return -1;
		}
		state->job.iodepth = small;
		return 0;
	}

	if (string_equal_ci(key, "numjobs")) {
		status = parse_small_unsigned(value, 1, SIM_FIO_MAX_JOBS, &small);
		if (bare || status != 0) {
			set_error(err, errsz, "line %lu: numjobs must be from 1 to %d",
				  (unsigned long)line_number, SIM_FIO_MAX_JOBS);
			return -1;
		}
		state->numjobs = small;
		return 0;
	}

	if (string_equal_ci(key, "runtime")) {
		status = parse_runtime_us(value, &number);
		if (bare || status != 0) {
			set_error(err, errsz,
				  "line %lu: runtime must be a non-zero time (us, ms, s, m, h, or d)",
				  (unsigned long)line_number);
			return -1;
		}
		state->job.runtime_us = number;
		state->job.runtime_set = 1;
		return 0;
	}

	if (string_equal_ci(key, "time_based")) {
		status = parse_bool(value, bare, &state->job.time_based);
		if (status != 0) {
			set_error(err, errsz, "line %lu: time_based expects a boolean",
				  (unsigned long)line_number);
			return -1;
		}
		return 0;
	}

	if (string_equal_ci(key, "norandommap") ||
	    string_equal_ci(key, "randrepeat") ||
	    string_equal_ci(key, "allrandrepeat")) {
		int *destination = string_equal_ci(key, "norandommap") ?
			&state->job.norandommap : &state->job.randrepeat;

		if (parse_bool(value, bare, destination) != 0) {
			set_error(err, errsz, "line %lu: %s expects a boolean",
				  (unsigned long)line_number, key);
			return -1;
		}
		return 0;
	}

	if (string_equal_ci(key, "randseed")) {
		if (bare || parse_seed_u64(value, &number) != 0) {
			set_error(err, errsz,
				  "line %lu: randseed must be an unsigned 64-bit integer (decimal or 0x hexadecimal)",
				  (unsigned long)line_number);
			return -1;
		}
		state->job.randseed = number;
		state->job.randseed_set = 1;
		return 0;
	}

	if (string_equal_ci(key, "random_generator")) {
		if (!bare && string_equal_ci(value, "tausworthe"))
			state->job.random_generator = 0;
		else if (!bare && string_equal_ci(value, "tausworthe64"))
			state->job.random_generator = 1;
		else {
			set_error(err, errsz,
				  "line %lu: unsupported random_generator '%s' (use tausworthe or tausworthe64)",
				  (unsigned long)line_number, value);
			return -1;
		}
		return 0;
	}

	if (string_equal_ci(key, "random_distribution")) {
		if (bare || !string_equal_ci(value, "random")) {
			set_error(err, errsz,
				  "line %lu: unsupported random_distribution '%s' (only uniform 'random' is supported)",
				  (unsigned long)line_number, value);
			return -1;
		}
		return 0;
	}

	if (is_unsupported_workload_option(key)) {
		set_error(err, errsz,
			  "line %lu: unsupported workload-shaping option '%s'",
			  (unsigned long)line_number, key);
		return -1;
	}
	if (is_ignored_option(key))
		return 0;

	set_error(err, errsz, "line %lu: unknown option '%s'",
		  (unsigned long)line_number, key);
	return -1;
}

static int append_job_clones(struct sim_fio_workload *workload,
			     const struct job_state *state,
			     size_t line_number, char *err, size_t errsz)
{
	unsigned int index;
	uint64_t base_offset = state->job.offset_set ? state->job.offset_bytes : 0;

	if (state->numjobs > SIM_FIO_MAX_JOBS - workload->njobs) {
		set_error(err, errsz,
			  "line %lu: section '%s' expands past the %d-job limit",
			  (unsigned long)line_number, state->job.name,
			  SIM_FIO_MAX_JOBS);
		return -1;
	}
	if (state->job.offset_increment_set && state->numjobs > 1) {
		uint64_t last_index = state->numjobs - 1;

		if (state->job.offset_increment_bytes > UINT64_MAX / last_index ||
		    base_offset > UINT64_MAX -
			state->job.offset_increment_bytes * last_index) {
			set_error(err, errsz,
				  "line %lu: offset_increment overflows for section '%s'",
				  (unsigned long)line_number, state->job.name);
			return -1;
		}
	}

	for (index = 0; index < state->numjobs; index++) {
		struct sim_fio_job clone = state->job;

		if (state->job.offset_increment_set) {
			clone.offset_bytes = base_offset +
				state->job.offset_increment_bytes * index;
			clone.offset_set = 1;
		}
		if (state->numjobs > 1) {
			char suffix[16];
			int suffix_length;
			size_t base_length;
			size_t copy_length;

			suffix_length = snprintf(suffix, sizeof(suffix), ".%u", index);
			if (suffix_length < 0 || (size_t)suffix_length >= sizeof(suffix)) {
				set_error(err, errsz,
					  "line %lu: cannot construct clone name for '%s'",
					  (unsigned long)line_number, state->job.name);
				return -1;
			}
			base_length = strlen(state->job.name);
			copy_length = sizeof(clone.name) - 1 - (size_t)suffix_length;
			if (base_length < copy_length)
				copy_length = base_length;
			memcpy(clone.name, state->job.name, copy_length);
			memcpy(clone.name + copy_length, suffix,
			       (size_t)suffix_length + 1);
		}
		workload->jobs[workload->njobs++] = clone;
	}
	return 0;
}

int sim_fio_load(const char *path, struct sim_fio_workload *workload,
		 char *err, size_t errsz)
{
	FILE *file;
	struct sim_fio_workload parsed;
	struct job_state global_state;
	struct job_state current_state;
	char *line = NULL;
	size_t line_capacity = 0;
	size_t line_length = 0;
	size_t line_number = 0;
	size_t current_section_line = 0;
	int in_section = 0;
	int in_global = 0;
	int read_status;
	int result = -1;

	if (err != NULL && errsz != 0)
		err[0] = '\0';
	if (path == NULL || workload == NULL) {
		set_error(err, errsz, "sim_fio_load: path and workload are required");
		return -1;
	}

	memset(&parsed, 0, sizeof(parsed));
	memset(workload, 0, sizeof(*workload));
	init_job_state(&global_state);
	memset(&current_state, 0, sizeof(current_state));

	file = fopen(path, "rb");
	if (file == NULL) {
		set_error(err, errsz, "cannot open '%s': %s", path, strerror(errno));
		return -1;
	}

	while ((read_status = read_line(file, &line, &line_capacity,
					&line_length)) > 0) {
		char *text;
		char *equals;
		char *key;
		char *value;
		int bare;

		line_number++;
		if (line_number == 1 && line_length >= 3 &&
		    (unsigned char)line[0] == 0xef &&
		    (unsigned char)line[1] == 0xbb &&
		    (unsigned char)line[2] == 0xbf)
			memmove(line, line + 3, line_length - 2);

		strip_comment(line);
		text = trim(line);
		if (*text == '\0')
			continue;

		if (*text == '[') {
			char *close = strchr(text + 1, ']');
			char *section_name;
			size_t name_length;

			if (close == NULL || *trim(close + 1) != '\0') {
				set_error(err, errsz, "line %lu: malformed section header",
					  (unsigned long)line_number);
				goto done;
			}
			*close = '\0';
			section_name = trim(text + 1);
			name_length = strlen(section_name);
			if (name_length == 0) {
				set_error(err, errsz, "line %lu: empty section name",
					  (unsigned long)line_number);
				goto done;
			}

			if (in_section && !in_global &&
			    append_job_clones(&parsed, &current_state,
					      current_section_line, err, errsz) != 0)
				goto done;

			in_section = 1;
			current_section_line = line_number;
			if (string_equal_ci(section_name, "global")) {
				in_global = 1;
			} else {
				if (name_length >= SIM_FIO_NAME_MAX) {
					set_error(err, errsz,
						  "line %lu: job name exceeds %d characters",
						  (unsigned long)line_number,
						  SIM_FIO_NAME_MAX - 1);
					goto done;
				}
				in_global = 0;
				current_state = global_state;
				memcpy(current_state.job.name, section_name,
				       name_length + 1);
			}
			continue;
		}

		if (!in_section) {
			set_error(err, errsz, "line %lu: option outside a section",
				  (unsigned long)line_number);
			goto done;
		}

		equals = strchr(text, '=');
		bare = equals == NULL;
		if (bare) {
			key = trim(text);
			value = text + strlen(text);
		} else {
			*equals = '\0';
			key = trim(text);
			value = trim(equals + 1);
		}
		if (*key == '\0') {
			set_error(err, errsz, "line %lu: empty option name",
				  (unsigned long)line_number);
			goto done;
		}

		if (in_global) {
			if (parse_option(&global_state, key, value, bare,
					 line_number, err, errsz) != 0)
				goto done;
		} else if (parse_option(&current_state, key, value, bare,
					line_number, err, errsz) != 0) {
			goto done;
		}
	}

	if (read_status == -1) {
		set_error(err, errsz, "out of memory while reading line %lu",
			  (unsigned long)(line_number + 1));
		goto done;
	}
	if (read_status == -2) {
		set_error(err, errsz, "line %lu: embedded NUL byte",
			  (unsigned long)(line_number + 1));
		goto done;
	}
	if (ferror(file)) {
		set_error(err, errsz, "error reading '%s': %s", path,
			  strerror(errno));
		goto done;
	}

	if (in_section && !in_global &&
	    append_job_clones(&parsed, &current_state,
			      current_section_line, err, errsz) != 0)
		goto done;
	if (parsed.njobs == 0) {
		set_error(err, errsz, "fio file contains no job sections");
		goto done;
	}

	*workload = parsed;
	result = 0;

done:
	free(line);
	(void)fclose(file);
	return result;
}
