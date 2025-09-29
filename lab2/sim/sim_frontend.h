#ifndef SIM_FRONTEND_H_
#define SIM_FRONTEND_H_

#include <sys/queue.h>
#include "nvme.h"
#include "stdbool.h"

extern struct nvme_request_queue fe_req_sq;
extern struct nvme_request_queue fe_req_cq;

#define STATE_NEXT				10
#define TASK_STATE_NEXT(x)		((x)->state += STATE_NEXT)
#define ONGOING_TASK(x)			((x->state) > STATE_NEXT)
#define TASK_STATE_WR			IO_NVM_WRITE
#define TASK_STATE_RD			IO_NVM_READ
#define TASK_STATE_RX			WAIT_TASK_STATE_WR + STATE_NEXT
#define TASK_STATE_TX			WAIT_TASK_STATE_RD + STATE_NEXT 

struct nvme_request_entry {
	unsigned int hid;
	unsigned int op;
	unsigned int cmd_id;
	unsigned int request_time;
	unsigned int state;
	unsigned int blkaddr;
	unsigned int nblks;
	unsigned int remaining_dma;
	TAILQ_ENTRY(nvme_request_entry) entry;
};

struct nvme_request_queue {
	unsigned int outstanding;
	TAILQ_HEAD(nvme_request_queue_head, nvme_request_entry) head;
};

struct fe_status {
	unsigned int cmd_id;
	unsigned int ongoing;
	unsigned char last_rx_tail;
	unsigned char last_tx_tail;
};

void init_fe();
void init_fe_req_queues();
void init_fe_stat();
bool check_nvme_stat();
unsigned int get_cmd_id();
unsigned int recv_dma_cmd();
void request_to_task(struct nvme_request_entry *req);
void send_nvme_cmd(struct nvme_request_entry *req);
void send_nvme_complete(struct nvme_request_entry *req);
void set_task_start_single();
void set_task_complete(struct nvme_request_entry *task);
void handle_dma_req(unsigned int cmd_id);
void update_dma_status();
int SchedulingFE();

#endif /* SIM_FRONTEND_H_ */
