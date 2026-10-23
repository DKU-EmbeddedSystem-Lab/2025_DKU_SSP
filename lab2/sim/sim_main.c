#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "sim_main.h"
#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"
#include "address_translation.h"
#include "request_transform.h"
#include "sim_fio.h"

struct sim sim;
static struct sim_fio_workload fio_workload;

static void show_usage(FILE *stream, const char *program) {
	fprintf(stream,
		"Usage: %s [options] WORKLOAD.fio\n\n"
		"Run GreedyFTL with a fio job file. Workload settings belong in the file.\n\n"
		"Options:\n"
		"  -c, --condition      Precondition FTL mappings before running\n"
		"  -r, --report         Save lat.csv and perf.csv in the current directory\n"
		"  -o, --outputdir DIR  Save reports in an existing directory (enables -r)\n"
		"  -h, --help           Show this help\n\n"
		"Examples:\n"
		"  %s workloads/example.fio\n"
		"  %s -c -o ./out workloads/example.fio\n",
		program, program, program);
}

/* Parse before allocating NAND memory or initializing the FTL. */
void parse_sim_args(int argc, char *argv[]) {
	int opt;
	char error[512];
	static const struct option long_options[] = {
		{"condition", no_argument, NULL, 'c'},
		{"report", no_argument, NULL, 'r'},
		{"outputdir", required_argument, NULL, 'o'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	memset(&sim.config, 0, sizeof(sim.config));
	opterr = 0;
	while ((opt = getopt_long(argc, argv, ":cro:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			sim.config.precond = 1;
			break;
		case 'r':
			sim.config.report = 1;
			break;
		case 'o':
			if (!*optarg) {
				fprintf(stderr, "Error: output directory must not be empty.\n");
				exit(EXIT_FAILURE);
			}
			sim.config.output_dir = optarg;
			sim.config.report = 1;
			break;
		case 'h':
			show_usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		case ':':
			fprintf(stderr, "Error: option '%s' requires an argument.\n",
				argv[optind - 1]);
			fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
			exit(EXIT_FAILURE);
		default:
			if (optopt)
				fprintf(stderr, "Error: unknown option '-%c'.\n", optopt);
			else
				fprintf(stderr, "Error: unknown option '%s'.\n", argv[optind - 1]);
			fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (argc - optind != 1 || !*argv[optind]) {
		fprintf(stderr, "Error: provide exactly one fio job file.\n\n");
		show_usage(stderr, argv[0]);
		exit(EXIT_FAILURE);
	}
	sim.config.fio_file = argv[optind];
	if (sim_fio_load(sim.config.fio_file, &fio_workload, error, sizeof(error))) {
		fprintf(stderr, "Error: %s: %s\n", sim.config.fio_file, error);
		exit(EXIT_FAILURE);
	}
}

/* Use the largest exact binary unit so small regions never round to 0 GB. */
static void format_bytes(uint64_t bytes, char *buffer, size_t size) {
	static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	size_t unit = 0;

	while (bytes && bytes % 1024 == 0 &&
	       unit + 1 < sizeof(units) / sizeof(units[0])) {
		bytes /= 1024;
		unit++;
	}
	snprintf(buffer, size, "%" PRIu64 " %s", bytes, units[unit]);
}

static const char *fio_rw_name(const struct host_config *config) {
	if (config->op_read_pcent == 100)
		return config->pattern ? "randread" : "read";
	if (config->op_write_pcent == 100)
		return config->pattern ? "randwrite" : "write";
	return config->pattern ? "randrw" : "rw";
}

static void show_configs(void) {
	printf("\nFIO workload: %s\n", sim.config.fio_file);
	printf("  Jobs: %d  |  Total I/Os: %" PRIu64 "\n",
		sim.config.nhosts, sim.config.nops);
	printf("  Precondition: %s  |  Reports: %s\n",
		sim.config.precond ? "on" : "off",
		sim.config.report ?
			(sim.config.output_dir ? sim.config.output_dir : ".") : "off");

	for (int i = 0; i < sim.config.nhosts; i++) {
		const struct host *host = &sim.hosts[i];
		const struct host_config *config = &host->config;
		uint64_t offset = (uint64_t)config->min_lba * BYTES_PER_NVME_BLOCK;
		uint64_t region_size =
			((uint64_t)config->max_lba + config->nblks - config->min_lba) *
			BYTES_PER_NVME_BLOCK;
		char block_text[32], offset_text[32], size_text[32];

		format_bytes((uint64_t)config->nblks * BYTES_PER_NVME_BLOCK,
			     block_text, sizeof(block_text));
		format_bytes(offset, offset_text, sizeof(offset_text));
		format_bytes(region_size, size_text, sizeof(size_text));
		printf("\n  [%d] %s\n", i, config->name);
		printf("      rw=%s  bs=%s  iodepth=%u  read/write=%u/%u%%\n",
			fio_rw_name(config), block_text, config->iodepth,
			config->op_read_pcent, config->op_write_pcent);
		printf("      offset=%s  size=%s  ios=%" PRIu64 "\n",
			offset_text, size_text, host->remaining_jobs);
		if (config->pattern)
			printf("      random_generator=%s  norandommap=%d  randseed=%" PRIu64 "\n",
				config->random_generator ? "tausworthe64" : "tausworthe",
				config->norandommap, config->randseed);
	}
	printf("\n");
}

void flush_hist_to_file(int idx) {
	if (sim.config.report) {
		for (int i = 0; i < sim.hist_idx[idx]; i++) {
			fprintf(sim.fp[idx], "%llu,%llu,%llu,%llu,%llu,%llu\n", sim.hist[idx][i][0], sim.hist[idx][i][1], sim.hist[idx][i][2], sim.hist[idx][i][3], sim.hist[idx][i][4], sim.hist[idx][i][5]);
		}
	}
}

void sim_cleanup() {
	if (sim.config.report) {
		for (int i = 0; i < 2; i++) {
			if (sim.fp[i]) {
				flush_hist_to_file(i);
				fflush(sim.fp[i]);
				fclose(sim.fp[i]);
				sim.fp[i] = NULL;
			}
		}
	}
	if (sim.hosts) {
		for (int i = 0; i < sim.config.nhosts; i++)
			cleanup_host_pattern(&sim.hosts[i]);
		free(sim.hosts);
		sim.hosts = NULL;
	}
}

void precond_mappings() {
	int i, j, k;
	
	for(i = 0; i < LOGICAL_SLICE_MAX; i++) {
		logicalSliceMapPtr->logicalSlice[i].virtualSliceAddr = i;
		virtualSliceMapPtr->virtualSlice[i].logicalSliceAddr = i;
	}

	for(i = 0; i < USER_DIES; i++) {
		virtualDieMapPtr->die[i].currentBlock = LOGICAL_BLOCKS_PER_DIE_MAX;
		virtualDieMapPtr->die[i].headFreeBlock = LOGICAL_BLOCKS_PER_DIE_MAX + 1;
		virtualDieMapPtr->die[i].tailFreeBlock = USER_BLOCKS_PER_DIE - 1;
		virtualDieMapPtr->die[i].freeBlockCnt = USER_BLOCKS_PER_DIE - LOGICAL_BLOCKS_PER_DIE_MAX - 1;

		virtualDieMapPtr->die[i].prevDie = DIE_NONE;
		virtualDieMapPtr->die[i].nextDie = DIE_NONE;
	}

	for(i = 0; i < USER_CHANNELS; i++) {
		for(j = 0; j < USER_WAYS; j++) {
			for (k = 0; k < LOGICAL_BLOCKS_PER_DIE_MAX; k++){
				rowAddrDependencyTablePtr->block[i][j][k].permittedProgPage = USER_PAGES_PER_BLOCK;
				rowAddrDependencyTablePtr->block[i][j][k].blockedReadReqCnt = 0;
				rowAddrDependencyTablePtr->block[i][j][k].blockedEraseReqFlag = 0;
			}
			int remains = LOGICAL_SLICE_MAX - LOGICAL_BLOCKS_PER_DIE_MAX * USER_PAGES_PER_BLOCK * USER_CHANNELS * USER_WAYS;
			if (remains) {
				rowAddrDependencyTablePtr->block[i][j][k].permittedProgPage = remains / (USER_CHANNELS * USER_WAYS);
				rowAddrDependencyTablePtr->block[i][j][k].blockedReadReqCnt = 0;
				rowAddrDependencyTablePtr->block[i][j][k].blockedEraseReqFlag = 0;
			}
		}
	}

	for(i = 0; i < USER_DIES; i++) {
		for(j = 0; j < LOGICAL_BLOCKS_PER_DIE_MAX; j++) {
			virtualBlockMapPtr->block[i][j].free = 0;
			virtualBlockMapPtr->block[i][j].invalidSliceCnt = 0;
			virtualBlockMapPtr->block[i][j].currentPage = USER_PAGES_PER_BLOCK;

			virtualBlockMapPtr->block[i][j].prevBlock = BLOCK_NONE;
			virtualBlockMapPtr->block[i][j].nextBlock = BLOCK_NONE;
		}

		int remains = LOGICAL_SLICE_MAX - LOGICAL_BLOCKS_PER_DIE_MAX * USER_PAGES_PER_BLOCK * USER_CHANNELS * USER_WAYS;
		if (remains) {
			virtualBlockMapPtr->block[i][j].free = USER_PAGES_PER_BLOCK - remains / (USER_CHANNELS * USER_WAYS);
			virtualBlockMapPtr->block[i][j].invalidSliceCnt = 0;
			virtualBlockMapPtr->block[i][j].currentPage = remains / (USER_CHANNELS * USER_WAYS);

			virtualBlockMapPtr->block[i][j].prevBlock = BLOCK_NONE;
			virtualBlockMapPtr->block[i][j].nextBlock = BLOCK_NONE;
		}
	}
}

void create_log_files () {
	FILE *fp1, *fp2;
	char path1[512], path2[512];
	const char *prefix = sim.config.output_dir ? sim.config.output_dir : ".";

	if (snprintf(path1, sizeof(path1), "%s/lat.csv", prefix) >= sizeof(path1) ||
	    snprintf(path2, sizeof(path2), "%s/perf.csv", prefix) >= sizeof(path2)) {
		fprintf(stderr, "Error: report output path is too long.\n");
		exit(EXIT_FAILURE);
	}
	fp1 = fopen(path1, "w");
	if (fp1 == NULL) {
		perror(path1);
		exit(1);
	}
	fp2 = fopen(path2, "w");
	if (fp2 == NULL) {
		perror(path2);
		fclose(fp1);
		exit(1);
	}
	sim.fp[0] = fp1;
	sim.fp[1] = fp2;
}

static uint64_t resolve_random_seed(const struct sim_fio_job *job)
{
	uint64_t seed;
	FILE *entropy;
	if (job->randseed_set)
		return job->randseed;
	if (job->randrepeat || !job->pattern)
		return UINT64_C(137);
	entropy = fopen("/dev/urandom", "rb");
	if (!entropy) {
		perror("Cannot initialize random seed");
		exit(EXIT_FAILURE);
	}
	if (fread(&seed, sizeof(seed), 1, entropy) != 1) {
		fprintf(stderr, "Cannot read random seed from /dev/urandom.\n");
		fclose(entropy);
		exit(EXIT_FAILURE);
	}
	fclose(entropy);
	return seed;
}

static void load_fio_config(void) {
	uint64_t capacity_bytes = (uint64_t)MAX_LBA * BYTES_PER_NVME_BLOCK;
	uint64_t total_ops = 0;

	sim.config.nhosts = (int)fio_workload.njobs;
	sim.hosts = calloc(sim.config.nhosts, sizeof(*sim.hosts));
	if (!sim.hosts) {
		perror("calloc");
		exit(1);
	}

	for (unsigned int i = 0; i < fio_workload.njobs; i++) {
		const struct sim_fio_job *fio_job = &fio_workload.jobs[i];
		struct host *host = &sim.hosts[i];
		uint64_t offset = fio_job->offset_set ? fio_job->offset_bytes : 0;
		uint64_t region_size;
		uint64_t transfer_size;
		uint64_t operations;
		uint64_t region_blocks;

		if (fio_job->runtime_set || fio_job->time_based) {
			fprintf(stderr,
				"fio job '%s': runtime/time_based is not supported; "
				"use io_size or number_ios.\n",
				fio_job->name);
			exit(1);
		}
		if (!fio_job->block_size_bytes ||
		    fio_job->block_size_bytes % BYTES_PER_NVME_BLOCK ||
		    fio_job->block_size_bytes / BYTES_PER_NVME_BLOCK > MAX_NUM_OF_NLB) {
			fprintf(stderr,
				"fio job '%s': bs must be a 4 KiB multiple between 4 KiB and %d KiB.\n",
				fio_job->name,
				(MAX_NUM_OF_NLB * BYTES_PER_NVME_BLOCK) / 1024);
			exit(1);
		}
		if (offset % BYTES_PER_NVME_BLOCK || offset >= capacity_bytes) {
			fprintf(stderr,
				"fio job '%s': offset must be 4 KiB aligned and inside the namespace.\n",
				fio_job->name);
			exit(1);
		}

		if (!fio_job->size_set) {
			region_size = capacity_bytes - offset;
		} else if (fio_job->region_size_percent) {
			region_size = capacity_bytes * fio_job->region_size_percent / 100;
			if (region_size > capacity_bytes - offset)
				region_size = capacity_bytes - offset;
			region_size = region_size / BYTES_PER_NVME_BLOCK * BYTES_PER_NVME_BLOCK;
		} else {
			region_size = fio_job->region_size_bytes;
		}

		if (!region_size || region_size > capacity_bytes ||
		    region_size % BYTES_PER_NVME_BLOCK ||
		    region_size < fio_job->block_size_bytes ||
		    offset > capacity_bytes - region_size) {
			fprintf(stderr,
				"fio job '%s': offset/size region is unaligned, too small, or outside "
				"the simulated namespace (%" PRIu64 " bytes).\n",
				fio_job->name, capacity_bytes);
			exit(1);
		}
		if (!fio_job->iodepth || fio_job->iodepth > MAX_QUEUE_DEPTH) {
			fprintf(stderr, "fio job '%s': iodepth must be between 1 and %d.\n",
				fio_job->name, MAX_QUEUE_DEPTH);
			exit(1);
		}

		transfer_size = fio_job->io_size_set ?
			fio_job->io_size_bytes : region_size;
		if (!transfer_size || transfer_size % fio_job->block_size_bytes) {
			fprintf(stderr,
				"fio job '%s': io_size (or size) must be an exact multiple of bs.\n",
				fio_job->name);
			exit(1);
		}
		operations = transfer_size / fio_job->block_size_bytes;
		if (fio_job->number_ios_set && fio_job->number_ios < operations)
			operations = fio_job->number_ios;
		if (!operations || UINT64_MAX - total_ops < operations) {
			fprintf(stderr, "fio job '%s': invalid or overflowing I/O count.\n",
				fio_job->name);
			exit(1);
		}

		region_blocks = region_size / BYTES_PER_NVME_BLOCK;
		snprintf(host->config.name, sizeof(host->config.name), "%s", fio_job->name);
		host->config.pattern = (unsigned int)fio_job->pattern;
		host->config.op_read_pcent = (unsigned int)fio_job->read_percent;
		host->config.op_write_pcent = (unsigned int)fio_job->write_percent;
		host->config.nblks =
			(unsigned int)(fio_job->block_size_bytes / BYTES_PER_NVME_BLOCK);
		host->config.iodepth = fio_job->iodepth;
		host->config.norandommap = fio_job->norandommap;
		host->config.random_generator = fio_job->random_generator;
		host->config.randseed = resolve_random_seed(fio_job);
		host->config.min_lba = (unsigned int)(offset / BYTES_PER_NVME_BLOCK);
		host->config.max_lba =
			(unsigned int)(host->config.min_lba + region_blocks - host->config.nblks);
		host->remaining_jobs = operations;
		total_ops += operations;
	}

	sim.config.nops = total_ops;
}

void init_sim(void) {
	load_fio_config();

	if (sim.config.report)
		create_log_files();
	sim.last_report_time = 0;
	sim.initial_report_time = 0;
	sim.next_hid = 0;
	sim.remaining_jobs = sim.config.nops;
	if (sim.config.precond)
		precond_mappings();

	show_configs();

	init_hosts();
	init_fe();
}
