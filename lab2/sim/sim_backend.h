#ifndef SIM_BACKEND_H_
#define SIM_BACKEND_H_

#if COSMOS_SIM //jy
#include <stdio.h>

/* Map firmware logging to the simulator's standard output. */
#define xil_printf printf
#else
#include "xil_printf.h"
#endif

#include <sys/queue.h>
#include "ftl_config.h"

extern void *g_mem;
extern struct timer_pqueue g_timer;

/* Never put pointer variables into "type" */
#define Addr2Mem(type, addr)		((type *)((char *)g_mem + (addr)))

#define SET_BIT(x, n)				((x) |= (1 << n))
#define CLEAR_BIT(x, n)				((x) &= ~(1 << n))

struct timer_pqueue_entry {
	unsigned long long trigger_time;
	unsigned int ch;
	unsigned int way;
	void *completion;
	unsigned int flag;
	TAILQ_ENTRY(timer_pqueue_entry) entry;
};


struct timer_pqueue {
	unsigned long long current_time;
	unsigned int ongoing;
	TAILQ_HEAD(timer_pqueue_head, timer_pqueue_entry) head;
};

void init_g_timer();
void init_nand();
void instant_task_raw(unsigned int ch, unsigned int way, void *completion, unsigned int flag);
void instant_task(unsigned int ch, unsigned int way);
void instant_task_comp(void *comletion, unsigned int flag);
struct timer_pqueue_entry *task_create_raw(unsigned int ch, unsigned int way, unsigned long long trigger_time, void *completion, unsigned int flag);
struct timer_pqueue_entry *task_create(unsigned int ch, unsigned int way, unsigned long long trigger_time);
void timer_put(struct timer_pqueue_entry *new_entry);
void timer_warp();
void set_busy(unsigned int ch, unsigned int way);
void clear_busy(unsigned int ch, unsigned int way);
/* Accept commands without advancing time, including calls from the driver. */
void SchedulingNand();
/* A scheduling pass may advance time only if no nested work made progress. */
unsigned long long sim_progress_generation(void);
void sim_note_progress(void);
void sim_advance_if_idle(unsigned long long generation);

#endif /* SIM_BACKEND_H_ */
