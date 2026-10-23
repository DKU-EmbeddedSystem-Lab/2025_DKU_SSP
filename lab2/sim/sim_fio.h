#ifndef SIM_FIO_H_
#define SIM_FIO_H_

#include <stddef.h>
#include <stdint.h>

#define SIM_FIO_MAX_JOBS 10
#define SIM_FIO_NAME_MAX 64

/*
 * Values produced by the small fio job-file reader in sim_fio.c.  Sizes are
 * kept in bytes and times in microseconds so that callers do not need to know
 * which suffix was used in the input file.
 */
struct sim_fio_job {
	char name[SIM_FIO_NAME_MAX];
	int pattern;                 /* 0: sequential, 1: random */
	int read_percent;
	int write_percent;
	uint64_t block_size_bytes;
	int norandommap;             /* Permit repeated random offsets. */
	int randrepeat;              /* Reproducible random streams by default. */
	uint64_t randseed;
	int randseed_set;
	int random_generator;        /* 0: tausworthe, 1: tausworthe64 */

	uint64_t region_size_bytes;
	unsigned int region_size_percent;
	int size_set;

	uint64_t offset_bytes;
	int offset_set;
	uint64_t offset_increment_bytes;
	int offset_increment_set;

	uint64_t io_size_bytes;
	int io_size_set;
	uint64_t number_ios;
	int number_ios_set;

	unsigned int iodepth;
	uint64_t runtime_us;
	int runtime_set;
	int time_based;
};

struct sim_fio_workload {
	unsigned int njobs;
	struct sim_fio_job jobs[SIM_FIO_MAX_JOBS];
};

/*
 * Read a supported subset of a fio INI job file.  Returns zero on success and
 * -1 on failure.  When err is non-NULL and errsz is non-zero, a diagnostic is
 * written to err.
 */
int sim_fio_load(const char *path, struct sim_fio_workload *workload,
		 char *err, size_t errsz);

#endif /* SIM_FIO_H_ */
