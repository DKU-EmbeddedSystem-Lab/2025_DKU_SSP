#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>

#include "sim_main.h"
#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"
#include "address_translation.h"
#include "request_transform.h"

struct sim sim;

void init_sim_config() {
	sim.config.nhosts = 0;
	sim.config.parts_pcent = NULL;
	sim.config.precond = 0;
	sim.config.report = 0;
	sim.config.output_dir = NULL;
}

void parse_listed_arg(char *optarg, int len, unsigned int *listed_arg) {
	int opt_idx = 0;
	char *token;
   
	token = strtok(optarg, " ");
	while (token) {
		if (opt_idx > len - 1) {
			printf("Too many args %s.\n", optarg);
			break;
		}
		listed_arg[opt_idx++] = atoi(token);
		token = strtok(NULL, " ");
	}
}

void argparser(int argc, char *argv[]) {
    int opt;
	int opt_idx = 0;
	int host_idx = 0;
	unsigned int arrarg[10];
	unsigned int aarg;
	struct option long_options[] = {
		{"size", optional_argument, "", 's'},
		{"nworkers", required_argument, 1, 'n'},
		{"worker", optional_argument, "", 'w'}, // "pattern, rp, wp, nblks"
		{"inst", required_argument, 1000000, 'i'}, 
		{"condition", no_argument, "", 'c'},
		{"outputdir", optional_argument, "", 'o'},
		{"report", no_argument, "", 'r'},
		{0, 0, 0, 0}
	};

    while ((opt = getopt_long(argc, argv, "n:w:s:c:i:o:r", long_options, &opt_idx)) != -1) {
        switch (opt) {
            case 'n':
				aarg = atoi(optarg);
				if (aarg > 10) {
					fprintf(stderr, "Too many workers %d.\n", aarg);
					exit(1);
				}
				sim.config.nhosts = aarg;
				sim.hosts = (struct host *)calloc(sizeof(struct host), sim.config.nhosts);
                break;
            case 'w':
				if (!sim.config.nhosts) {
					perror("Please set nworkers first.\n");
					exit(1);
				}
				if (host_idx == sim.config.nhosts) {
					fprintf(stderr, "Can't create worker[%d]. nworkers is %d\n", host_idx, sim.config.nhosts);
					exit(1);
				}
				parse_listed_arg(optarg, 4, arrarg);
				if (arrarg[1] + arrarg[2] != 100) {
					fprintf(stderr, "Invalid option for worker[%d] read[%d] write[%d] pcents.\n", host_idx, arrarg[1], arrarg[2]);
					exit(1);
				}
				init_host_config(&(sim.hosts[host_idx].config), arrarg);
				host_idx++;
                break;
            case 's':
				if (!sim.config.nhosts) {
					perror("Please set nworkers first.\n");
					exit(1);
				}
				parse_listed_arg(optarg, sim.config.nhosts, arrarg);
				sim.config.parts_pcent = (int *)calloc(sizeof(int), sim.config.nhosts);
				memcpy(arrarg, sim.config.parts_pcent, sizeof(int) * sim.config.nhosts);
                break;
            case 'c':
				sim.config.precond = 1;
				break;
            case 'r':
				sim.config.report = 1;
				break;
			case 'o':
				sim.config.output_dir = strdup(optarg);
				break;
			case 'i':
				sim.config.nops = atoi(optarg);
				break;
            default:
				perror("Wrong usage TnT\n");
				exit(1);
        }
    }
}

void fill_host_config() {
	int i;
	unsigned int cumlba = 0;

	if (!sim.config.parts_pcent) {
		sim.config.parts_pcent = (int *)calloc(sizeof(int), sim.config.nhosts);
		for (i = 0; i < sim.config.nhosts; i++)
			sim.config.parts_pcent[i] = 100 / sim.config.nhosts;
	}

	for (i = 0; i < sim.config.nhosts; i++) {
		sim.hosts[i].config.min_lba = cumlba;
		cumlba += (unsigned int)((float)MAX_LBA * sim.config.parts_pcent[i] / 100);
		sim.hosts[i].config.max_lba = cumlba - sim.hosts[i].config.nblks;
	}
}

void show_configs() {
	printf("%d workers\n", sim.config.nhosts);
	for (int i = 0; i < sim.config.nhosts; i++) {
		printf("  Worker[%d]: %d r%d w%d nblks%d\n", i, sim.hosts[i].config.pattern, sim.hosts[i].config.op_read_pcent, sim.hosts[i].config.op_write_pcent, sim.hosts[i].config.nblks);
		printf("   - partition: %d GB ~ %d GB\n", sim.hosts[i].config.min_lba / (1000000000 / 4096), (sim.hosts[i].config.max_lba + sim.hosts[i].config.nblks) / (1000000000 / 4096));
	}
	printf("Precondition %d\n", sim.config.precond);
	printf("Report %d\n", sim.config.report);
	printf("Total OPs %d\n", sim.config.nops);
}

void flush_hist_to_file(int idx) {
	if (sim.config.report) {
		for (int i = 0; i < sim.hist_idx[idx]; i++) {
			fprintf(sim.fp[idx], "%llu,%llu,%llu,%llu,%llu,%llu\n", sim.hist[idx][i][0], sim.hist[idx][i][1], sim.hist[idx][i][2], sim.hist[idx][i][3], sim.hist[idx][i][4], sim.hist[idx][i][5]);
		}
	}
}

void sim_cleanup() {
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

void init_sim(int argc, char *argv[]) {
	init_sim_config();
	argparser(argc, argv);
	fill_host_config();

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
