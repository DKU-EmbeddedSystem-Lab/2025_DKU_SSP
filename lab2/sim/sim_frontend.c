#include <stddef.h>
#include <sys/queue.h>
#include <stdio.h>

#include "io_access.h"
#include "xparameters.h"
#include "host_lld.h"

#include "sim_host.h"
#include "sim_frontend.h"
#include "sim_backend.h"

struct nvme_request_queue fe_req_sq;
struct nvme_request_queue fe_req_cq;
struct fe_status fe_stat;

void init_fe() {
	init_fe_req_queues();
	init_fe_stat();
}

void init_fe_req_queues() {
//	NVME_CMD_FIFO_REG nvme_cmd;

	TAILQ_INIT(&(fe_req_sq.head));
	fe_req_sq.outstanding = 0;
	TAILQ_INIT(&(fe_req_cq.head));
	fe_req_cq.outstanding = 0;
}

void init_fe_stat() {
	fe_stat.cmd_id = 0;
	fe_stat.ongoing = 0;
	fe_stat.last_rx_tail = 0;
	fe_stat.last_tx_tail = 0;
}

bool check_nvme_stat() {
	NVME_CMD_FIFO_REG nvme_cmd;
	nvme_cmd.dword = IO_READ32(NVME_CMD_FIFO_REG_ADDR);
	return nvme_cmd.cmdValid;
}

void send_nvme_cmd(struct nvme_request_entry *req) {
	NVME_CMD_FIFO_REG nvme_cmd;
	NVME_IO_COMMAND nvme_io_cmd;
	IO_WRITE_COMMAND_DW12 dw12;
	unsigned int addr, idx;
	union addr blkaddr;

	blkaddr.addr = (void *)req->blkaddr;
	nvme_io_cmd.OPC = req->op;
	nvme_io_cmd.dword10 = blkaddr.low; //addr low
	nvme_io_cmd.dword11 = blkaddr.high; //addr high
	nvme_io_cmd.PRP1[0] = 0; //prp1 low
	nvme_io_cmd.PRP1[1] = 0; //prp1 high
	nvme_io_cmd.PRP2[0] = 0; //prp2 low
	nvme_io_cmd.PRP2[1] = 0; //prp2 high
	dw12.NLB = req->nblks - 1;
	nvme_io_cmd.dword12 = dw12.dword;

	nvme_cmd.cmdSlotTag = req->cmd_id;
	nvme_cmd.qID = 1;
	nvme_cmd.cmdSeqNum = 0;
	nvme_cmd.cmdValid = 1;

	addr = NVME_CMD_SRAM_ADDR + (req->cmd_id * 64);
	for (idx = 0; idx < 16; idx++)
		IO_WRITE32(addr + (idx * 4), nvme_io_cmd.dword[idx]);

	IO_WRITE32(NVME_CMD_FIFO_REG_ADDR, nvme_cmd.dword);

	TASK_STATE_NEXT(req);
}

unsigned int recv_dma_cmd(unsigned int direction, unsigned int tail) {
	HOST_DMA_CMD_FIFO_REG dma_cmd;

	dma_cmd = g_hostDmaCmdQueue.cmd[tail][direction];
	return dma_cmd.cmdSlotTag;
}

void send_nvme_complete(struct nvme_request_entry *req) {
	TAILQ_INSERT_TAIL(&(fe_req_cq.head), req, entry);
	fe_req_cq.outstanding++;
}

void set_task_complete(struct nvme_request_entry *task) {
	TAILQ_REMOVE(&(fe_req_sq.head), task, entry);
	fe_req_sq.outstanding--;
	fe_stat.ongoing--;
	send_nvme_complete(task);
}

void handle_dma_req(unsigned int cmd_id) {
	struct nvme_request_entry *req;

	TAILQ_FOREACH(req, &(fe_req_sq.head), entry) {
		if (!ONGOING_TASK(req))
			continue;
		if (req->cmd_id == cmd_id) {
			if (!--req->remaining_dma)
				set_task_complete(req);
			break;
		}
	}
	
	if (!req)
		printf("No such req which cmd_id is %d\n", cmd_id);
}

unsigned int get_cmd_id() {
	unsigned int cmd_id = fe_stat.cmd_id;
	fe_stat.cmd_id = ((cmd_id + 1) % (1 << P_SLOT_TAG_WIDTH));
	return cmd_id;
}

void request_to_task(struct nvme_request_entry *req) {
	req->cmd_id = get_cmd_id();
	req->state = req->op;
	req->remaining_dma = req->nblks;
}

void set_task_start_single() {
	struct nvme_request_entry *req;

	TAILQ_FOREACH(req, &(fe_req_sq.head), entry) {
		if (!ONGOING_TASK(req)) {
			request_to_task(req);
			send_nvme_cmd(req);
			fe_stat.ongoing++;
			return;
		}
	}
}

void update_dma_status() {
	HOST_DMA_FIFO_CNT_REG dma_fifo_head;

	dma_fifo_head.dword = IO_READ32(HOST_DMA_FIFO_CNT_REG_ADDR);
	dma_fifo_head.autoDmaRx = fe_stat.last_rx_tail;
	dma_fifo_head.autoDmaTx = fe_stat.last_tx_tail;
	IO_WRITE32(HOST_DMA_FIFO_CNT_REG_ADDR, dma_fifo_head.dword);
}

int SchedulingFE() {
	int exe = 0;
	unsigned int cmd_id;
	HOST_DMA_FIFO_CNT_REG dma_fifo_tail;

	//send nvme
	if (fe_req_sq.outstanding > fe_stat.ongoing && !check_nvme_stat()) {
		set_task_start_single();
		exe++;
	}

	// recv dma
	dma_fifo_tail.dword = IO_READ32(HOST_DMA_FIFO_CNT_REG_ADDR + 4);
	while (fe_stat.last_rx_tail != dma_fifo_tail.autoDmaRx) {
		cmd_id = recv_dma_cmd(HOST_DMA_RX_DIRECTION, fe_stat.last_rx_tail);
		handle_dma_req(cmd_id);
		fe_stat.last_rx_tail++;
		exe++;
	}
	while (fe_stat.last_tx_tail != dma_fifo_tail.autoDmaTx) {
		cmd_id = recv_dma_cmd(HOST_DMA_TX_DIRECTION, fe_stat.last_tx_tail);
		handle_dma_req(cmd_id);
		fe_stat.last_tx_tail++;
		exe++;
	}
	update_dma_status();

	return exe;
}
