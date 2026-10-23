#include <stdlib.h>
#include <assert.h>
#include <sys/queue.h>
#include <stdint.h>
#include "sim_backend.h"
#include "nsc_driver.h"

void *g_mem;
struct timer_pqueue g_timer;
static unsigned long long progress_generation;

unsigned long long sim_progress_generation(void) {
	return progress_generation;
}

void sim_note_progress(void) {
	progress_generation++;
}

void sim_advance_if_idle(unsigned long long generation) {
	if (generation == progress_generation && !TAILQ_EMPTY(&g_timer.head))
		timer_warp();
}

void init_g_timer() {
	TAILQ_INIT(&(g_timer.head));
	g_timer.current_time = 0;
	g_timer.ongoing = 0;
	progress_generation = 0;
}

void init_nand() {
	g_mem = calloc(1, 0x100000000);
}

void instant_task_raw(unsigned int ch, unsigned int way, void *completion, unsigned int flag) {
	clear_busy(ch, way);
	*((unsigned int *)completion) = flag;
}

void instant_task(unsigned int ch, unsigned int way) {
	clear_busy(ch, way);
}

void instant_task_comp(void *completion, unsigned int flag) {
	*((unsigned int *)completion) = flag;
}

struct timer_pqueue_entry *task_create_raw(unsigned int ch, unsigned int way, unsigned long long trigger_time, void *completion, unsigned int flag) {
	struct timer_pqueue_entry *task = (struct timer_pqueue_entry *)malloc(sizeof(struct timer_pqueue_entry));

	if (!task)
		assert(!"[WARNING]Failed to allocate timer_pqueue_entry[WARNING]");

	task->trigger_time = g_timer.current_time + trigger_time;
	task->ch = ch;
	task->way = way;
	if (flag) {
		task->completion = completion;
		task->flag = flag;
	} else {
		task->completion = NULL;
		task->flag = flag;
	}

	return task;
}

struct timer_pqueue_entry *task_create(unsigned int ch, unsigned int way, unsigned long long trigger_time) {
	return task_create_raw(ch, way, trigger_time, NULL, 0);
}

void timer_put(struct timer_pqueue_entry *task) {
	struct timer_pqueue_entry *entry;

	set_busy(task->ch, task->way);
	g_timer.ongoing++;

	if (TAILQ_EMPTY(&(g_timer.head))) {
		TAILQ_INSERT_HEAD(&(g_timer.head), task, entry);
		return;
	}

	TAILQ_FOREACH(entry, &(g_timer.head), entry) {
		if (entry->trigger_time > task->trigger_time) {
			TAILQ_INSERT_BEFORE(entry, task, entry);
			return;
		}
	}

	TAILQ_INSERT_TAIL(&(g_timer.head), task, entry);
	return;
}

void timer_warp() {
	struct timer_pqueue_entry *task = TAILQ_FIRST(&(g_timer.head));
	struct timer_pqueue_entry *next_task;
  
   	if (!task)	
		return;

	do {
		next_task = TAILQ_NEXT(task, entry);

		g_timer.ongoing--;
		g_timer.current_time = task->trigger_time;
		clear_busy(task->ch, task->way);
		if (task->flag)
			*((unsigned int *)(task->completion)) = task->flag;

		TAILQ_REMOVE(&(g_timer.head), task, entry);
		free(task);
		task = next_task;
	} while (task && task->trigger_time == g_timer.current_time);

	/* A nested wait must not let its caller advance time a second time. */
	sim_note_progress();
	return;
}

void set_busy(unsigned int ch, unsigned int way) {
	CLEAR_BIT(chCtlReg[ch]->readyBusy, way);
}

void clear_busy(unsigned int ch, unsigned int way) {
	SET_BIT(chCtlReg[ch]->readyBusy, way);
}

void SchedulingNand() {
	int execmd = 0;
	unsigned int ch, way;
	union addr completion;
	struct timer_pqueue_entry *task;

	for (ch = 0; ch < USER_CHANNELS; ch++) {
		switch (chCtlReg[ch]->cmdSelect) {
			case V2FCommand_NOP:
				break;
			case V2FCommand_Reset:
				way = chCtlReg[ch]->waySelection;
				instant_task(ch, way);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_SetFeatures:
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_GetFeatures:
				completion.low = chCtlReg[ch]->completionAddress;
				completion.high = chCtlReg[ch]->errorCountAddress;
				instant_task_comp(completion.addr, 1);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_ReadPageTrigger:
				way = chCtlReg[ch]->waySelection;
				// row = chCtlReg[ch]->rowAddress;
				task = task_create(ch, way, 30);
				timer_put(task);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_ReadPageTransfer:
				way = chCtlReg[ch]->waySelection;
				// row = chCtlReg[ch]->rowAddress;
				// pageDataBuffer = chCtlReg[ch]->dataAddress;
				// spareDataBuffer = chCtlReg[ch]->spareAddress;
				// errorInformation = chCtlReg[ch]->errorCountAddress;
				completion.low = chCtlReg[ch]->completionAddress;
				completion.high = chCtlReg[ch]->errorCountAddress;
				task = task_create_raw(ch, way, 300, completion.addr, 1);
				timer_put(task);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_ProgramPage:
				way = chCtlReg[ch]->waySelection;
				// row = chCtlReg[ch]->rowAddress;
				// pageDataBuffer = chCtlReg[ch]->dataAddress;
				// spareDataBuffer = chCtlReg[ch]->spareAddress;
				task = task_create(ch, way, 610);
				timer_put(task);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_BlockErase:
				way = chCtlReg[ch]->waySelection;
				// row = chCtlReg[ch]->rowAddress;
				task = task_create(ch, way, 5250);
				timer_put(task);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_StatusCheck:
				completion.low = chCtlReg[ch]->completionAddress;
				completion.high = chCtlReg[ch]->errorCountAddress;
				instant_task_comp(completion.addr, ((0x60) << 1 | 1));
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
			case V2FCommand_ReadPageTransferRaw:
				way = chCtlReg[ch]->waySelection;
				// pageDataBuffer = chCtlReg[ch]->dataAddress;
				completion.low = chCtlReg[ch]->completionAddress;
				completion.high = chCtlReg[ch]->errorCountAddress;
				task = task_create_raw(ch, way, 5, completion.addr, 1);
				timer_put(task);
				chCtlReg[ch]->cmdSelect = V2FCommand_NOP;
				execmd++;
				break;
		}
	}

	if (execmd)
		sim_note_progress();
}
