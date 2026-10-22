#include <sys/queue.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "host_lld.h"

#include "sim_main.h"
#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"

void init_host_config(struct host_config *config, unsigned int *opt) {
	memset(config, 0, sizeof(*config));
	config->min_lba = 0;
	config->max_lba = 0;
	config->pattern = opt[0];
	config->op_read_pcent = opt[1];
	config->op_write_pcent = opt[2];
	config->nblks = opt[3];
	config->iodepth = MAX_QUEUE_DEPTH;
}

void init_hosts() {
	struct host *host;
	for (int i = 0; i < sim.config.nhosts; i++) {
		host = &(sim.hosts[i]);

		host->hid = i;
		host->acc = 0;
		host->next_blkaddr = host->config.min_lba;
		host->complete_blks[0] = 0;
		host->complete_blks[1] = 0;
		host->last_complete_blks[0] = 0;
		host->last_complete_blks[1] = 0;
		host->complete_reqs[0] = 0;
		host->complete_reqs[1] = 0;
		host->last_complete_reqs[0] = 0;
		host->last_complete_reqs[1] = 0;
		host->outstanding = 0;

		if (host->config.pattern) {
			unsigned int first = (host->config.min_lba + 3) / 4 * 4;
			unsigned int last = host->config.max_lba / 4 * 4;

			if (first <= last) {
				unsigned int slots = (last - first) / 4 + 1;
				host->next_blkaddr = first + (rand() % slots) * 4;
			} else {
				host->next_blkaddr = host->config.min_lba;
			}
		}
	}
}

unsigned int get_next_blkaddr(struct host *host) {
	struct host_config config = host->config;
	unsigned int blkaddr = host->next_blkaddr;

	if (config.pattern) {
		unsigned int first = (config.min_lba + 3) / 4 * 4;
		unsigned int last = config.max_lba / 4 * 4;
		unsigned int slots;

		if (first <= last) {
			slots = (last - first) / 4 + 1;
			host->next_blkaddr = first + (rand() % slots) * 4;
		} else {
			host->next_blkaddr = config.min_lba;
		}
	} else if (blkaddr > config.max_lba ||
		   config.max_lba - blkaddr < config.nblks) {
		host->next_blkaddr = config.min_lba;
	} else {
		host->next_blkaddr = blkaddr + config.nblks;
	}

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
	unsigned long long elapsed = ctime - sim.last_report_time;
	float rb, wb, ri, wi;
	struct host *host;
	int hid;

	for (hid = 0; hid < sim.config.nhosts; hid++) {
		host = &(sim.hosts[hid]);

		if (elapsed) {
			wi = (float)(host->complete_reqs[0] - host->last_complete_reqs[0]) /
				(elapsed / 1000.0f);
			ri = (float)(host->complete_reqs[1] - host->last_complete_reqs[1]) /
				(elapsed / 1000.0f);
			wb = ((float)(host->complete_blks[0] - host->last_complete_blks[0]) * 4096) /
				elapsed;
			rb = ((float)(host->complete_blks[1] - host->last_complete_blks[1]) * 4096) /
				elapsed;
		} else {
			wi = 0;
			ri = 0;
			wb = 0;
			rb = 0;
		}
		host->last_complete_reqs[0] = host->complete_reqs[0];
		host->last_complete_reqs[1] = host->complete_reqs[1];

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
	if (sim.hosts[req->hid].outstanding)
		sim.hosts[req->hid].outstanding--;
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

bool get_next_job(struct job *job) {
	struct host *host;
	int checked;

	for (checked = 0; checked < sim.config.nhosts; checked++) {
		host = &(sim.hosts[sim.next_hid]);
		sim.next_hid = (sim.next_hid + 1) % sim.config.nhosts;
		if (!host->remaining_jobs ||
		    host->outstanding >= host->config.iodepth)
			continue;

		job->hid = host->hid;
		job->op = select_op(host);
		job->blkaddr = get_next_blkaddr(host);
		job->nblks = host->config.nblks;
		return true;
	}

	return false;
}

int SchedulingHost() {
	int exe = 0;
	struct nvme_request_entry *req;

	while (sim.remaining_jobs && fe_req_sq.outstanding < MAX_QUEUE_DEPTH) {
		struct job job;
		struct host *host;

		if (!get_next_job(&job))
			break;
		host = &(sim.hosts[job.hid]);
		req = create_request(job);
		request_send(req);
		fe_req_sq.outstanding++;
		host->outstanding++;
		host->remaining_jobs--;
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
