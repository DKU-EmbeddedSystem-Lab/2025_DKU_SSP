#ifndef SIM_H_
#define SIM_H_

#include "sim_host.h"
#include "ftl_config.h"

#define HIST_BUF_MAX				10000000
#define MAX_LBA						storageCapacity_L 
#define LOGICAL_SLICE_MAX 			(storageCapacity_L / NVME_BLOCKS_PER_SLICE)
#define LOGICAL_BLOCK_MAX			(LOGICAL_SLICE_MAX / USER_PAGES_PER_BLOCK)
#define LOGICAL_BLOCKS_PER_DIE_MAX	(LOGICAL_BLOCK_MAX / USER_DIES)
#define LOGICAL_BLOCKS_PER_LUN_MAX	(LOGICAL_BLOCKS_PER_DIE_MAX / LUNS_PER_DIE)

struct sim_config {
	int partition;
	int nhosts;
	int *parts_pcent;
	int precond;
	int report;
	int nops;
	char *output_dir;
};

struct sim {
	struct sim_config config;
	unsigned long long initial_report_time;
	unsigned long long last_report_time;
	int next_hid;
	unsigned int remaining_jobs;
	struct host *hosts;
	FILE *fp[2];
	volatile unsigned int hist_idx[2];
	volatile unsigned long long hist[2][HIST_BUF_MAX][6]; // LATENCY(time hid op lat lba nblks) PERF(time hid rb wb crb cwb)
};

void init_sim_config();
void parse_listed_arg(char *optarg, int len, unsigned int *listed_arg);
void argparser(int argc, char *argv[]);
void fill_host_config();
void show_configs();
void flush_hist_to_file(int idx);
void sim_cleanup();
void precond_mappings();
void init_sim(int argc, char *argv[]);

extern struct sim sim;

#endif /* SIM_H_ */
