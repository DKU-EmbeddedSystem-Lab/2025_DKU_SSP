#include <sys/queue.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "host_lld.h"

#include "sim_main.h"
#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"

/* SplitMix64 expands a seed into valid, decorrelated Tausworthe states. */
static uint64_t expand_seed(uint64_t *seed)
{
	uint64_t value = (*seed += UINT64_C(0x9e3779b97f4a7c15));
	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

/* Combined Tausworthe recurrences (Taus88 / Taus258, as used by fio). */
static uint32_t taus32_step(uint32_t state, unsigned int left,
		unsigned int right, unsigned int shift, unsigned int low_bits)
{
	return ((state & (UINT32_MAX << low_bits)) << shift) ^
		(((state << left) ^ state) >> right);
}

static uint64_t taus64_step(uint64_t state, unsigned int left,
		unsigned int right, unsigned int shift, unsigned int low_bits)
{
	return ((state & (UINT64_MAX << low_bits)) << shift) ^
		(((state << left) ^ state) >> right);
}

static uint64_t next_random(struct host *host)
{
	uint64_t *state = host->random_state;
	if (host->config.random_generator == 0) {
		state[0] = taus32_step((uint32_t)state[0], 13, 19, 12, 1);
		state[1] = taus32_step((uint32_t)state[1], 2, 25, 4, 3);
		state[2] = taus32_step((uint32_t)state[2], 3, 11, 17, 4);
		return state[0] ^ state[1] ^ state[2];
	}
	state[0] = taus64_step(state[0], 1, 53, 10, 1);
	state[1] = taus64_step(state[1], 24, 50, 5, 9);
	state[2] = taus64_step(state[2], 3, 23, 29, 12);
	state[3] = taus64_step(state[3], 5, 24, 23, 17);
	state[4] = taus64_step(state[4], 3, 33, 8, 23);
	return state[0] ^ state[1] ^ state[2] ^ state[3] ^ state[4];
}

static void reset_random_map(struct host *host)
{
	uint64_t bits = host->random_slots;
	for (unsigned int level = 0; level < host->random_map_levels; ++level) {
		size_t words = host->random_map_words[level];
		memset(host->random_map[level], 0, words * sizeof(uint64_t));
		/* Padding is occupied, so it can never become an address candidate. */
		if (bits % 64)
			host->random_map[level][words - 1] = UINT64_MAX << (bits % 64);
		bits = words;
	}
	host->random_remaining = host->random_slots;
}

void cleanup_host_pattern(struct host *host)
{
	for (unsigned int level = 0; level < HOST_RANDOM_MAP_LEVELS; ++level) {
		free(host->random_map[level]);
		host->random_map[level] = NULL;
		host->random_map_words[level] = 0;
	}
	host->random_map_levels = 0;
	host->random_slots = 0;
	host->random_remaining = 0;
}

/* A new host must be zero-initialized; this also supports reinitialization. */
int init_host_pattern(struct host *host, uint64_t seed)
{
	static const unsigned int low32[] = {1, 3, 4};
	static const unsigned int low64[] = {1, 9, 12, 17, 23};
	struct host_config *config = &host->config;
	uint64_t bits;

	cleanup_host_pattern(host);
	if (!config->nblks || config->min_lba > config->max_lba ||
	    (config->random_generator != 0 && config->random_generator != 1))
		return -1;
	host->next_blkaddr = config->min_lba;
	if (!config->pattern)
		return 0;

	host->random_slots =
		((uint64_t)config->max_lba - config->min_lba) / config->nblks + 1;
	/* Valid Tausworthe states must have at least one retained high bit. */
	for (unsigned int i = 0; i < (config->random_generator ? 5U : 3U); ++i) {
		uint64_t state = expand_seed(&seed);
		if (config->random_generator)
			host->random_state[i] = state | (UINT64_C(1) << low64[i]);
		else
			host->random_state[i] = (uint32_t)state | (UINT32_C(1) << low32[i]);
	}
	for (unsigned int i = 0; i < 6; ++i)
		(void)next_random(host);
	if (config->norandommap)
		return 0;

	bits = host->random_slots;
	for (unsigned int level = 0; level < HOST_RANDOM_MAP_LEVELS; ++level) {
		uint64_t words = (bits + 63) / 64;
		if (words > SIZE_MAX / sizeof(uint64_t))
			goto fail;
		host->random_map[level] = calloc((size_t)words, sizeof(uint64_t));
		if (!host->random_map[level])
			goto fail;
		host->random_map_words[level] = (size_t)words;
		host->random_map_levels++;
		if (words == 1) {
			reset_random_map(host);
			return 0;
		}
		bits = words;
	}
fail:
	cleanup_host_pattern(host);
	return -1;
}

/* Parent bits mark full child words, avoiding a linear scan of a busy map. */
static uint64_t find_free_slot(const struct host *host, unsigned int level,
		uint64_t start)
{
	uint64_t bits = level ? host->random_map_words[level - 1] : host->random_slots;
	uint64_t word = start / 64;
	uint64_t available;
	if (start >= bits)
		return UINT64_MAX;
	available = ~host->random_map[level][word] & (UINT64_MAX << (start % 64));
	if (!available) {
		if (level + 1 == host->random_map_levels)
			return UINT64_MAX;
		word = find_free_slot(host, level + 1, word + 1);
		if (word == UINT64_MAX)
			return UINT64_MAX;
		available = ~host->random_map[level][word];
	}
	assert(available);
	return word * 64 + (unsigned int)__builtin_ctzll(available);
}

static void mark_random_slot(struct host *host, uint64_t slot)
{
	for (unsigned int level = 0; level < host->random_map_levels; ++level) {
		uint64_t word = slot / 64;
		host->random_map[level][word] |= UINT64_C(1) << (slot % 64);
		if (host->random_map[level][word] != UINT64_MAX)
			break;
		slot = word;
	}
	host->random_remaining--;
}

static unsigned int next_random_lba(struct host *host)
{
	uint64_t random = next_random(host);
	uint64_t slot;
	assert(host->random_slots);
	/* fio-style range scaling, calculated exactly without floating rounding. */
	if (host->config.random_generator)
		slot = (uint64_t)(((__uint128_t)random * host->random_slots) >> 64);
	else
		slot = (random * host->random_slots) >> 32;
	if (!host->config.norandommap) {
		if (!host->random_remaining)
			reset_random_map(host);
		slot = find_free_slot(host, 0, slot);
		if (slot == UINT64_MAX)
			slot = find_free_slot(host, 0, 0);
		assert(slot < host->random_slots);
		mark_random_slot(host, slot);
	}
	return (unsigned int)(host->config.min_lba + slot * host->config.nblks);
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

		if (init_host_pattern(host, host->config.randseed +
			(uint64_t)i * UINT64_C(0x9e3779b97f4a7c15))) {
			fprintf(stderr, "fio job '%s': cannot initialize random address map.\n",
				host->config.name);
			sim_cleanup();
			exit(EXIT_FAILURE);
	}
	}
}

unsigned int get_next_blkaddr(struct host *host) {
	struct host_config config = host->config;
	unsigned int blkaddr = host->next_blkaddr;

	if (config.pattern)
		return next_random_lba(host);
	if (blkaddr > config.max_lba ||
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

	/* Reclaim queue depth before trying to submit more work at this time. */
	while (fe_req_cq.outstanding) {
		req = request_recv();
		perf_report(req);
		request_destroy(req);
		fe_req_cq.outstanding--;
		exe++;
	}

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

	if (exe)
		sim_note_progress();

	if (!check_remaining_jobs()) {
		update_and_print_bw();
		return -1;
	}

	return exe;
}
