#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>

#include "sim_main.h"
#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"
#include "address_translation.h"
#include "request_transform.h"
#include "sim_fio.h"

struct sim sim;

void init_sim_config() {
	sim.config.nhosts = 1;
	sim.config.parts_pcent = NULL;
	sim.config.precond = 0;
	sim.config.report = 0;
	sim.config.output_dir = NULL;
	sim.config.nops = 5000000;
	sim.config.fio_file = NULL;
	sim.config.fio_mode = 0;
}

static void show_usage(const char *program) {
	fprintf(stderr,
		"Usage:\n"
		"  %s --fio-job WORKLOAD.fio [-c] [-r] [-o DIR]\n"
		"  %s WORKLOAD.fio [-c] [-r] [-o DIR]\n"
		"  %s -n N -w \"PATTERN READ%% WRITE%% NBLOCKS\" ... [options]\n\n"
		"Options:\n"
		"  -f, --fio-job FILE    load a fio-compatible job file\n"
		"  -n, --nworkers N      number of legacy workers (1..10)\n"
		"  -w, --worker LIST     legacy worker definition (repeat N times)\n"
		"  -s, --size LIST       legacy worker partition percentages\n"
		"  -i, --inst N          total legacy I/O operations\n"
		"  -c, --condition       precondition FTL mappings\n"
		"  -r, --report          write lat.csv and perf.csv\n"
		"  -o, --outputdir DIR   report output directory\n"
		"  -h, --help            show this help\n",
		program, program, program);
}

static int parse_u64_arg(const char *text, uint64_t *value) {
	uint64_t parsed = 0;
	const unsigned char *cursor = (const unsigned char *)text;

	if (!text || !*text)
		return -1;
	while (*cursor) {
		unsigned int digit;

		if (*cursor < '0' || *cursor > '9')
			return -1;
		digit = (unsigned int)(*cursor - '0');
		if (parsed > (UINT64_MAX - digit) / 10)
			return -1;
		parsed = parsed * 10 + digit;
		cursor++;
	}
	*value = parsed;
	return 0;
}

int parse_listed_arg(const char *arg, int len, unsigned int *listed_arg) {
	int opt_idx = 0;
	char *copy;
	char *saveptr = NULL;
	char *token;

	if (!arg || len <= 0)
		return -1;
	copy = strdup(arg);
	if (!copy)
		return -1;

	token = strtok_r(copy, " \t", &saveptr);
	while (token) {
		uint64_t value;
		if (opt_idx >= len || parse_u64_arg(token, &value) ||
		    value > UINT_MAX) {
			free(copy);
			return -1;
		}
		listed_arg[opt_idx++] = (unsigned int)value;
		token = strtok_r(NULL, " \t", &saveptr);
	}
	free(copy);
	return opt_idx == len ? 0 : -1;
}

void argparser(int argc, char *argv[]) {
	int opt;
	int opt_idx = 0;
	int host_idx = 0;
	unsigned int arrarg[10];
	int legacy_workload_option = 0;
	struct option long_options[] = {
		{"size", required_argument, NULL, 's'},
		{"nworkers", required_argument, NULL, 'n'},
		{"worker", required_argument, NULL, 'w'}, // "pattern, rp, wp, nblks"
		{"inst", required_argument, NULL, 'i'}, 
		{"condition", no_argument, NULL, 'c'},
		{"outputdir", required_argument, NULL, 'o'},
		{"report", no_argument, NULL, 'r'},
		{"fio", required_argument, NULL, 'f'},
		{"fio-job", required_argument, NULL, 'f'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "n:w:s:i:o:f:crh", long_options, &opt_idx)) != -1) {
		switch (opt) {
			case 'n':
				{
					uint64_t workers;
					if (parse_u64_arg(optarg, &workers) || workers < 1 ||
					    workers > SIM_FIO_MAX_JOBS) {
						fprintf(stderr, "nworkers must be between 1 and %d.\n",
							SIM_FIO_MAX_JOBS);
						exit(1);
					}
					if (sim.hosts) {
						fprintf(stderr, "nworkers may only be specified once.\n");
						exit(1);
					}
					sim.config.nhosts = (int)workers;
					sim.hosts = calloc(sim.config.nhosts, sizeof(*sim.hosts));
					if (!sim.hosts) {
						perror("calloc");
						exit(1);
					}
					legacy_workload_option = 1;
				}
				break;
			case 'w':
				if (!sim.hosts) {
					fprintf(stderr, "Set --nworkers before --worker.\n");
					exit(1);
				}
				if (host_idx == sim.config.nhosts) {
					fprintf(stderr, "Can't create worker[%d]; nworkers is %d.\n",
						host_idx, sim.config.nhosts);
					exit(1);
				}
				memset(arrarg, 0, sizeof(arrarg));
				if (parse_listed_arg(optarg, 4, arrarg)) {
					fprintf(stderr, "Invalid worker '%s'; expected four unsigned integers.\n",
						optarg);
					exit(1);
				}
				if (arrarg[0] > 1 || arrarg[1] > 100 || arrarg[2] > 100 ||
				    arrarg[1] + arrarg[2] != 100 ||
				    arrarg[3] < 1 || arrarg[3] > MAX_NUM_OF_NLB) {
					fprintf(stderr,
						"Invalid worker[%d]: pattern must be 0/1, read+write 100, "
						"and nblks 1..%u.\n",
						host_idx, (unsigned int)MAX_NUM_OF_NLB);
					exit(1);
				}
				init_host_config(&(sim.hosts[host_idx].config), arrarg);
				host_idx++;
				legacy_workload_option = 1;
				break;
			case 's':
				if (!sim.hosts) {
					fprintf(stderr, "Set --nworkers before --size.\n");
					exit(1);
				}
				memset(arrarg, 0, sizeof(arrarg));
				if (parse_listed_arg(optarg, sim.config.nhosts, arrarg)) {
					fprintf(stderr, "Invalid partition list '%s'.\n", optarg);
					exit(1);
				}
				{
					uint64_t total = 0;
					for (int i = 0; i < sim.config.nhosts; i++) {
						if (arrarg[i] > 100) {
							fprintf(stderr, "Partition percentages must be from 0 to 100.\n");
							exit(1);
						}
						total += arrarg[i];
					}
					if (total != 100) {
						fprintf(stderr, "Worker partition percentages must total 100.\n");
						exit(1);
					}
				}
				free(sim.config.parts_pcent);
				sim.config.parts_pcent = calloc(sim.config.nhosts,
								      sizeof(*sim.config.parts_pcent));
				if (!sim.config.parts_pcent) {
					perror("calloc");
					exit(1);
				}
				for (int i = 0; i < sim.config.nhosts; i++)
					sim.config.parts_pcent[i] = (int)arrarg[i];
				legacy_workload_option = 1;
				break;
            case 'c':
				sim.config.precond = 1;
				break;
            case 'r':
				sim.config.report = 1;
				break;
			case 'o':
				free(sim.config.output_dir);
				sim.config.output_dir = strdup(optarg);
				if (!sim.config.output_dir) {
					perror("strdup");
					exit(1);
				}
				break;
			case 'i':
				if (parse_u64_arg(optarg, &sim.config.nops) || !sim.config.nops) {
					fprintf(stderr, "inst must be a positive integer.\n");
					exit(1);
				}
				legacy_workload_option = 1;
				break;
			case 'f':
				if (sim.config.fio_file) {
					fprintf(stderr, "Only one fio job file may be specified.\n");
					exit(1);
				}
				sim.config.fio_file = strdup(optarg);
				if (!sim.config.fio_file) {
					perror("strdup");
					exit(1);
				}
				break;
			case 'h':
				show_usage(argv[0]);
				exit(0);
				break;
			default:
				show_usage(argv[0]);
				exit(1);
		}
	}

	if (optind < argc) {
		if (sim.config.fio_file || optind + 1 != argc) {
			fprintf(stderr, "Unexpected positional argument.\n");
			show_usage(argv[0]);
			exit(1);
		}
		sim.config.fio_file = strdup(argv[optind]);
		if (!sim.config.fio_file) {
			perror("strdup");
			exit(1);
		}
	}
	if (sim.config.fio_file && legacy_workload_option) {
		fprintf(stderr,
			"--fio-job cannot be combined with -n, -w, -s, or -i.\n");
		exit(1);
	}
	if (sim.hosts && host_idx != sim.config.nhosts) {
		fprintf(stderr, "Expected %d worker definitions, got %d.\n",
			sim.config.nhosts, host_idx);
		exit(1);
	}
}

void fill_host_config() {
	int i;
	unsigned int cumulative_percent = 0;

	if (!sim.config.parts_pcent) {
		sim.config.parts_pcent = calloc(sim.config.nhosts,
						 sizeof(*sim.config.parts_pcent));
		if (!sim.config.parts_pcent) {
			perror("calloc");
			exit(1);
		}
		for (i = 0; i < sim.config.nhosts; i++) {
			sim.config.parts_pcent[i] = 100 / sim.config.nhosts;
			if (i < 100 % sim.config.nhosts)
				sim.config.parts_pcent[i]++;
		}
	}

	for (i = 0; i < sim.config.nhosts; i++) {
		unsigned int region_end;
		sim.hosts[i].config.min_lba =
			(unsigned int)((uint64_t)MAX_LBA * cumulative_percent / 100);
		cumulative_percent += sim.config.parts_pcent[i];
		region_end = (unsigned int)((uint64_t)MAX_LBA * cumulative_percent / 100);
		if (region_end - sim.hosts[i].config.min_lba <
		    sim.hosts[i].config.nblks) {
			fprintf(stderr, "Worker[%d] partition is smaller than its block size.\n", i);
			exit(1);
		}
		sim.hosts[i].config.max_lba = region_end - sim.hosts[i].config.nblks;
	}
}

void show_configs() {
	printf("%d workers\n", sim.config.nhosts);
	for (int i = 0; i < sim.config.nhosts; i++) {
		printf("  Worker[%d]%s%s: %s r%d w%d nblks%d qd%d ops=%" PRIu64 "\n",
			i, sim.hosts[i].config.name[0] ? " " : "",
			sim.hosts[i].config.name,
			sim.hosts[i].config.pattern ? "random" : "sequential",
			sim.hosts[i].config.op_read_pcent,
			sim.hosts[i].config.op_write_pcent,
			sim.hosts[i].config.nblks,
			sim.hosts[i].config.iodepth,
			sim.hosts[i].remaining_jobs);
		printf("   - partition: %d GB ~ %d GB\n", sim.hosts[i].config.min_lba / (1000000000 / 4096), (sim.hosts[i].config.max_lba + sim.hosts[i].config.nblks) / (1000000000 / 4096));
	}
	printf("Precondition %d\n", sim.config.precond);
	printf("Report %d\n", sim.config.report);
	printf("Total OPs %" PRIu64 "\n", sim.config.nops);
}

void flush_hist_to_file(int idx) {
	if (sim.config.report) {
		for (int i = 0; i < sim.hist_idx[idx]; i++) {
			fprintf(sim.fp[idx], "%llu,%llu,%llu,%llu,%llu,%llu\n", sim.hist[idx][i][0], sim.hist[idx][i][1], sim.hist[idx][i][2], sim.hist[idx][i][3], sim.hist[idx][i][4], sim.hist[idx][i][5]);
		}
	}
}

void sim_cleanup() {
	if (!sim.config.report)
		return;

	for (int i = 0; i < 2; i++) {
		flush_hist_to_file(i);
		fflush(sim.fp[i]);
		fclose(sim.fp[i]);
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
	char *prefix;

	if (!sim.config.output_dir)
		prefix = strdup(".");
	else 
		prefix = strdup(sim.config.output_dir);

	snprintf(path1, sizeof(path1), "%s/lat.csv", prefix);
	fp1 = fopen(path1, "w");
	if (fp1 == NULL) {
		fprintf(stderr, "Failed to open %s.\n", path1);
		exit(1);
	}
	snprintf(path2, sizeof(path2), "%s/perf.csv", prefix);
	fp2 = fopen(path2, "w");
	if (fp2 == NULL) {
		fprintf(stderr, "Failed to open %s.\n", path1);
		fclose(fp1);
		exit(1);
	}
	sim.fp[0] = fp1;
	sim.fp[1] = fp2;

	free(prefix);
}

static void load_fio_config(const char *path) {
	struct sim_fio_workload workload;
	uint64_t capacity_bytes = (uint64_t)MAX_LBA * BYTES_PER_NVME_BLOCK;
	uint64_t total_ops = 0;
	char error[512];

	if (sim_fio_load(path, &workload, error, sizeof(error))) {
		fprintf(stderr, "Failed to load fio job file: %s\n", error);
		exit(1);
	}

	sim.config.nhosts = (int)workload.njobs;
	sim.hosts = calloc(sim.config.nhosts, sizeof(*sim.hosts));
	if (!sim.hosts) {
		perror("calloc");
		exit(1);
	}

	for (unsigned int i = 0; i < workload.njobs; i++) {
		const struct sim_fio_job *fio_job = &workload.jobs[i];
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
		host->config.min_lba = (unsigned int)(offset / BYTES_PER_NVME_BLOCK);
		host->config.max_lba =
			(unsigned int)(host->config.min_lba + region_blocks - host->config.nblks);
		host->remaining_jobs = operations;
		total_ops += operations;
	}

	sim.config.fio_mode = 1;
	sim.config.nops = total_ops;
}

void init_sim(int argc, char *argv[]) {
	sim.hosts = NULL;
	init_sim_config();
	argparser(argc, argv);
	if (sim.config.fio_file) {
		load_fio_config(sim.config.fio_file);
	} else if (!sim.hosts) {
		if (sim.config.nhosts != 1) {
			fprintf(stderr, "Unset option: w\n");
			exit(1);
		}
		unsigned int seq_write_16k[4] = {0, 0, 100, 4};
		sim.hosts = calloc(1, sizeof(*sim.hosts));
		if (!sim.hosts) {
			perror("calloc");
			exit(1);
		}
		for (int i = 0; i < sim.config.nhosts; i++)
			init_host_config(&(sim.hosts[i].config), seq_write_16k);
	}
	if (!sim.config.fio_mode) {
		fill_host_config();
		for (int i = 0; i < sim.config.nhosts; i++) {
			snprintf(sim.hosts[i].config.name,
				 sizeof(sim.hosts[i].config.name), "worker%d", i);
			sim.hosts[i].remaining_jobs = sim.config.nops / sim.config.nhosts;
			if ((uint64_t)i < sim.config.nops % sim.config.nhosts)
				sim.hosts[i].remaining_jobs++;
		}
	}

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
