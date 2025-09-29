#ifndef SIM_HOST_H_
#define SIM_HOST_H_

#include <sys/queue.h>
#include <stdbool.h>

#define MAX_QUEUE_DEPTH			(1 << P_SLOT_TAG_WIDTH)
#define CMD_NONE 				(1 << 31)

struct job {
	unsigned int hid;
	unsigned int op;
	unsigned int blkaddr;
	unsigned int nblks;
};

struct host_config {
	unsigned int min_lba;
	unsigned int max_lba;
	unsigned int nblks;
	unsigned int op_read_pcent;
	unsigned int op_write_pcent;
	unsigned int pattern; //s 0 r 1
};

struct host {
	unsigned int hid;
	struct host_config config;
	unsigned int acc;
	unsigned int next_blkaddr;
	unsigned int complete_blks[2]; //w 0 r 1
	unsigned int last_complete_blks[2];
	unsigned int complete_reqs[2]; //w 0 r 1
	unsigned int last_complete_reqs[2];
};

void init_hosts();
void init_host_config(struct host_config *config, unsigned int *opt);
struct nvme_request_entry *create_request(struct job job);
void request_send(struct nvme_request_entry *req);
struct nvme_request_entry *request_recv();
void request_destroy(struct nvme_request_entry *req);
void update_and_print_bw();
unsigned int select_op(struct host *host);
unsigned int get_next_blkaddr(struct host *host);
struct job get_next_job();
void perf_report(struct nvme_request_entry *req);
bool check_remaining_jobs();
int SchedulingHost();

#endif /* SIM_HOST_H_ */
