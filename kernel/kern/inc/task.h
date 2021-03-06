/** @file task.h
 *  @author Newton Xie (ncx)
 *  @bug No known bugs.
 */

#ifndef _TASK_H_
#define _TASK_H_

#include <stdint.h>
#include <elf_410.h>
#include <syscall.h>

#include "utils/list.h"
#include "utils/kern_mutex.h"
#include "utils/maps.h"

#define KERN_STACK_SIZE 0x1000

#define USER_STACK_LOW 0xFFB00000
#define USER_STACK_SIZE 0x4000
#define USER_STACK_START 0xFFB03E00

#define LIST_NODE_TO_TCB(thread_node) ((thread_t *)((char *)thread_node + 8))
#define TCB_TO_LIST_NODE(thread) ((thread_node_t *)((char *)thread - 8))

#define LIST_NODE_TO_TASK(task_node) ((task_t *)((char *)task_node + 8))
#define TASK_TO_LIST_NODE(task) ((task_node_t *)((char *)task - 8))

typedef node_t task_node_t;
typedef node_t thread_node_t;

/** @brief  Task control block structure.
 *  
 *  Contains task id, status, virtual memory housekeeping, thread and task
 *  lists, and mutexes.
 */
typedef struct task {
    int task_id;
    int status;
    uint32_t *page_dir;
    map_list_t *maps;

    kern_mutex_t thread_list_mutex;
    list_t *live_thread_list;
    list_t *zombie_thread_list;

    kern_mutex_t child_task_list_mutex;
    list_t *child_task_list;

    list_t *zombie_task_list;
    list_t *waiting_thread_list;

    /* 
     * This wait_mutex protects both zombie_task_list and waiting_thread_list.
     */
    kern_mutex_t wait_mutex;

    /* 
     * This vanish_mutex is acquired by a task when it vanishes, or by a parent
     * task when it orphans its children. It ensures that the task's parent
     * pointer is not changed to the init task in the middle of vanishing.
     */
    kern_mutex_t vanish_mutex;
    struct task *parent_task;
} task_t;

/** @brief  Thread control block structure.
 *
 *  Contains tid, status, a pointer to the parent task, stack and instruction
 *  pointer information, and exception handler information.
 */
typedef struct thread {
    int tid;
    int status;
    task_t *task;

    uint32_t kern_sp;
    uint32_t cur_sp;
    uint32_t ip;

    void *swexn_sp;
    swexn_handler_t swexn_handler;
    void *swexn_arg;
} thread_t;

/** @brief  Structure used for blocking on wait().
 *
 *  An instance of this structure is declared on the stack before a waiting
 *  thread blocks. This is done instead of using the thread block itself in
 *  the waiting threads list, because the list node must contain a place to
 *  store the zombie task address, and we don't want to put this in the thread
 *  block (too messy).
 */
typedef struct wait_node {
    node_t node;
    thread_t *thread;
    task_t *zombie;
} wait_node_t;

/** @brief  Structure used to block on sleep().
 *
 *  This is here for similar reasons as wait_node, since we must be able to
 *  access the number of ticks needed for wakeup.
 */
typedef struct sleep_node {
    node_t node;
    thread_t *thread;
    unsigned int wakeup_ticks;
} sleep_node_t;

/* status define */
#define RUNNABLE 0
#define INITIALIZED 1
#define SUSPENDED 2
#define FORKED 3
#define BLOCKED_MUTEX 4
#define BLOCKED_WAIT 5
#define ZOMBIE 6
#define SLEEPING 7

int id_counter_init();

int gen_thread_id();

task_t *task_init();

void undo_task_init(task_t *task);

void task_clear(task_t *task);

void task_destroy(task_t *task);

void reap_threads(task_t *task);

int task_lists_init(task_t *task);

void task_lists_destroy(task_t *task);

int task_mutexes_init(task_t *task);

void task_mutexes_destroy(task_t *task);

thread_t *thread_init();

void thread_destroy(thread_t *thread);

int validate_user_mem(uint32_t addr, uint32_t len, int perms);

int validate_user_string(uint32_t addr, int max_len);

int load_program(simple_elf_t *header, map_list_t *maps);

int load_elf_section(const char *fname, unsigned long start, unsigned long len,
                     long offset, int pte_flags);

void orphan_children(task_t *task);

void orphan_zombies(task_t *task);

#endif
