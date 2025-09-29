#include <sys/queue.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "host_lld.h"

#include "sim_main.h"
#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"

void init_host_config(struct host_config *config, unsigned int *opt) {
	config->min_lba = 0;
	config->max_lba = 0;
	config->pattern = opt[0];
	config->op_read_pcent = opt[1];
	config->op_write_pcent = opt[2];
	config->nblks = opt[3];
}

void init_hosts() {
	struct host *host;
	for (unsigned int i = 0; i < sim.config.nhosts; i++) {
		host = &(sim.hosts[i]);

		host->hid = i;
		host->acc = 0;
		host->next_blkaddr = host->config.min_lba;
		host->complete_blks[0] = 0;
		host->complete_blks[1] = 0;
		host->last_complete_blks[0] = 0;
		host->last_complete_blks[1] = 0;

		host->next_blkaddr = get_next_blkaddr(host);
	}
}

unsigned int get_next_blkaddr(struct host *host) {
	struct host_config config = host->config;
	unsigned int blkaddr = host->next_blkaddr;

	if (config.pattern)
		host->next_blkaddr = config.min_lba + (rand() % (config.max_lba - config.min_lba));
	else
		host->next_blkaddr = ((blkaddr + config.nblks - config.min_lba) % config.max_lba) + config.min_lba;

	return blkaddr;
}

struct nvme_request_entry *create_request(struct job job) {
	struct nvme_request_entry *req = (struct nvme_request_entry *)malloc(sizeof(struct nvme_request_entry));

	req->hid = job.hid;
	req->op = job.op;
	req->cmd_id = CMD_NONE;
	req->request_time = g_timer.current_time;
	req->blkaddr = job.blkaddr;
	req->nblks = job.nblks;
	req->state = 0;
	req->remaining_dma = 0;

	return req;
}

void request_send(struct nvme_request_entry *req) {
	TAILQ_INSERT_TAIL(&(fe_req_sq.head), req, entry);
}

struct nvme_request_entry *request_recv() {
	struct nvme_request_entry *req;
	req = TAILQ_FIRST(&(fe_req_cq.head));
	TAILQ_REMOVE(&(fe_req_cq.head), req, entry);
	return req;
}

void request_destroy(struct nvme_request_entry *req) {
	free(req);
}

void update_and_print_bw() {
	unsigned long long ctime = g_timer.current_time;
	float rb, wb, ri, wi;
	struct host *host;
	int hid;

	for (hid = 0; hid < sim.config.nhosts; hid++) {
		host = &(sim.hosts[hid]);

		wi = (float)(host->complete_reqs[0] - host->last_complete_reqs[0]) / ((ctime - sim.last_report_time) / 1000);
		ri = (float)(host->complete_reqs[1] - host->last_complete_reqs[1]) / ((ctime - sim.last_report_time) / 1000);
		host->last_complete_reqs[0] = host->complete_reqs[0];
		host->last_complete_reqs[1] = host->complete_reqs[1];

		wb = ((float)(host->complete_blks[0] - host->last_complete_blks[0]) * 4096) / (ctime - sim.last_report_time);
		rb = ((float)(host->complete_blks[1] - host->last_complete_blks[1]) * 4096) / (ctime - sim.last_report_time);
		host->last_complete_blks[0] = host->complete_blks[0];
		host->last_complete_blks[1] = host->complete_blks[1];

		sim.hist[1][sim.hist_idx[1]][0] = ctime - sim.initial_report_time;
		sim.hist[1][sim.hist_idx[1]][1] = hid;
		sim.hist[1][sim.hist_idx[1]][2] = (unsigned int)rb;
		sim.hist[1][sim.hist_idx[1]][3] = (unsigned int)wb;
		sim.hist[1][sim.hist_idx[1]][4] = (unsigned int)ri;
		sim.hist[1][sim.hist_idx[1]++][5] = (unsigned int)wi;
		if (sim.hist_idx[1] == HIST_BUF_MAX) {
			flush_hist_to_file(1);
			sim.hist_idx[1] = 0;
		}

		printf("[%llus]Host[%d]: Current BW[R %.2fMB/s, W %.2fMB/s] IOPS[R %.2f KIOPS, W %.2f KIOPS]\n", (ctime - sim.initial_report_time)/1000000, hid, rb, wb, ri, wi);
	}
	sim.last_report_time = ctime;
}

void perf_report(struct nvme_request_entry *req) {
	unsigned long long ctime = g_timer.current_time;
	int lat = (int)(ctime - req->request_time + 1);

	if (!sim.initial_report_time) {
		sim.initial_report_time = ctime;
		sim.last_report_time = ctime;
	}

	sim.hosts[req->hid].complete_blks[req->op - 1] += req->nblks;
	sim.hosts[req->hid].complete_reqs[req->op - 1]++;
	sim.hist[0][sim.hist_idx[0]][0] = ctime - sim.initial_report_time;
	sim.hist[0][sim.hist_idx[0]][1] = req->hid;
	sim.hist[0][sim.hist_idx[0]][2] = req->op;
	sim.hist[0][sim.hist_idx[0]][3] = lat;
	sim.hist[0][sim.hist_idx[0]][4] = req->blkaddr;
	sim.hist[0][sim.hist_idx[0]++][5] = req->nblks;
	if (sim.hist_idx[0] == HIST_BUF_MAX) {
		flush_hist_to_file(0);
		sim.hist_idx[0] = 0;
	}

	if (ctime > sim.last_report_time + 1000000) 
		update_and_print_bw();
}

bool check_remaining_jobs() {
	return (sim.remaining_jobs || fe_req_sq.outstanding || fe_req_cq.outstanding);
}

unsigned int select_op(struct host *host) {
	host->acc += host->config.op_read_pcent;
	if (host->acc >= 100) {
		host->acc -= 100;
		return IO_NVM_READ;
	} else
		return IO_NVM_WRITE;
}

struct job get_next_job() {
	struct host *host = &(sim.hosts[sim.next_hid]);
	struct job job;

	job.hid = host->hid;
	job.op = select_op(host);
	job.blkaddr = get_next_blkaddr(host);
	job.nblks = host->config.nblks;

	sim.next_hid = (sim.next_hid + 1) % (sim.config.nhosts);

	return job;
}

int SchedulingHost() {
	int exe = 0;
	struct nvme_request_entry *req;

	while (sim.remaining_jobs && fe_req_sq.outstanding < MAX_QUEUE_DEPTH) {
		struct job job = get_next_job();
		req = create_request(job);
		request_send(req);
		fe_req_sq.outstanding++;
		sim.remaining_jobs--;
		exe++;
	}

	while (fe_req_cq.outstanding) {
		req = request_recv();
		perf_report(req);
		request_destroy(req);
		fe_req_cq.outstanding--;
		exe++;
	}

	if (!check_remaining_jobs()) {
		update_and_print_bw();
		return -1;
	}

	return exe;
}


