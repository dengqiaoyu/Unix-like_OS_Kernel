/** @file syscalls.c
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <page.h>
#include <simics.h>
#include <x86/cr.h>
#include <malloc.h>         /* malloc, smemalign, sfree */
#include <asm.h>            /* disable_interrupts enable_interrupts */
#include <console.h>
#include <assert.h>

#include "vm.h"
#include "task.h"
#include "scheduler.h"
#include "mutex.h"
#include "asm_registers.h"
#include "asm_switch.h"
#include "allocator.h"      /* allocator */
#include "tcb_hashtab.h"    /* tcb hash table */
#include "keyboard_driver.h" /* keyboard_buffer_t */
#include "kern_sem.h"       /* semaphore */
#include "asm_page_inval.h"
#include "asm_set_exec_context.h"

extern unsigned int num_ticks;

extern thread_t *init_thread;

extern allocator_t *sche_allocator;
extern uint32_t *kern_page_dir;
extern uint32_t zfod_frame;
extern int num_free_frames;

int kern_fork(void) {
    int ret = SUCCESS;
    thread_t *old_thread = get_cur_tcb();
    int old_tid = old_thread->tid;
    task_t *old_task = old_thread->task;
    thread_t *cur_thr = NULL;

    mutex_lock(&(old_task->thread_list_mutex));
    int live_threads = get_list_size(old_task->live_thread_list);
    mutex_unlock(&(old_task->thread_list_mutex));
    if (live_threads > 1) return ERROR_FORK_TASK_MORE_THAN_ONE_THREAD;

    task_t *new_task = task_init();
    if (new_task == NULL) {
        lprintf("f1");
        return ERROR_FORK_MALLOC_TASK_FAILED;
    }
    new_task->parent_task = old_task;

    ret = page_dir_copy(new_task->page_dir, old_task->page_dir);
    if (ret != SUCCESS) {
        // TODO destroy task
        lprintf("f2");
        return ERROR_FORK_COPY_PAGE_FAILED;
    }

    ret = maps_copy(old_task->maps, new_task->maps);
    if (ret != SUCCESS) {
        lprintf("fa");
        return -1;
    }

    thread_t *new_thread = thread_init();
    if (new_thread == NULL) {
        // TODO destroy task
        // TODO unmap new page directory
        lprintf("f3");
        return -1;
    }
    new_task->task_id = new_thread->tid;
    new_thread->task = new_task;
    new_thread->status = FORKED;
    add_node_to_head(new_task->live_thread_list, TCB_TO_LIST_NODE(new_thread));

    mutex_lock(&(old_task->child_task_list_mutex));
    add_node_to_head(old_task->child_task_list, TASK_TO_LIST_NODE(new_task));
    mutex_unlock(&(old_task->child_task_list_mutex));
    asm_set_exec_context(old_thread->kern_sp,
                         new_thread->kern_sp,
                         &(new_thread->cur_sp),
                         &(new_thread->ip));
    // Now, we will have two tasks running
    // BUG that has been found!!! cannot declare var here, because we will break
    // stack
    // Do we need a mutex to protect this one?
    cur_thr = get_cur_tcb();
    // if (get_list_size(cur_thr->task->child_task_list) == 0) {
    if (cur_thr->tid != old_tid) {
        return 0;
    } else {
        sche_push_back(new_thread);
        return new_thread->tid;
    }
}

int kern_exec(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    char *execname = (char *)(*esi);
    char **argvec = (char **)(*(esi + 1));

    thread_t *thread = get_cur_tcb();
    task_t *task = thread->task;

    mutex_lock(&(task->thread_list_mutex));
    int live_threads = get_list_size(task->live_thread_list);
    mutex_unlock(&(task->thread_list_mutex));
    if (live_threads > 1) return -1;

    char **temp = argvec;
    int argc = 0;
    int total_len = 0;
    int ret;
    int len;

    ret = validate_user_mem((uint32_t)temp, sizeof(char *), MAP_USER);
    if (temp && ret < 0) return -1;

    while (temp && *temp) {
        char *arg = *temp;
        len = validate_user_string((uint32_t)arg, 128);
        if (len <= 0) return -1;
        total_len += len;
        if (total_len > 128) return -1;

        argc += 1;
        if (argc > 16) return -1;
        temp += 1;

        ret = validate_user_mem((uint32_t)temp, sizeof(char *), MAP_USER);
        if (ret < 0) return -1;
    }

    ret = validate_user_string((uint32_t)execname, 64);
    if (ret <= 0) return -1;
    char namebuf[64];
    sprintf(namebuf, "%s", execname);

    simple_elf_t elf_header;
    ret = elf_load_helper(&elf_header, namebuf);
    if (ret < 0) return -1;

    char argbuf[128];
    char *ptrbuf[16];
    char *arg;
    char *marker = argbuf;

    int i;
    for (i = 0; i < argc; i++) {
        arg = *(argvec + i);
        len = strlen(arg);
        strncpy(marker, arg, len);
        marker[len] = '\0';
        ptrbuf[i] = marker;
        marker += len + 1;
    }

    thread->cur_sp = USER_STACK_START;
    thread->ip = elf_header.e_entry;

    maps_clear(task->maps);
    maps_insert(task->maps, 0, PAGE_SIZE * NUM_KERN_PAGES - 1, 0);
    maps_insert(task->maps, RW_PHYS_VA, RW_PHYS_VA + PAGE_SIZE - 1, 0);

    page_dir_clear(task->page_dir);
    set_cr3((uint32_t)task->page_dir);

    ret = load_program(&elf_header, task->maps);
    if (ret < 0) return -1;

    // need macros here badly
    // 5 because ret addr and 4 args
    char **argv = (char **)(USER_STACK_START + 5 * sizeof(int));
    // 6 because argv is a null terminated char ptr array
    char *buf = (char *)(USER_STACK_START + 6 * sizeof(int) +
                         argc * sizeof(int));

    for (i = 0; i < argc; i++) {
        arg = *(ptrbuf + i);
        len = strlen(arg);
        strncpy(buf, arg, len);
        buf[len] = '\0';
        argv[i] = buf;
        buf += len + 1;
    }
    argv[argc] = NULL;

    uint32_t *ptr = (uint32_t *)USER_STACK_START;
    *(ptr + 1) = argc;
    *(ptr + 2) = (uint32_t)argv;
    *(ptr + 3) = USER_STACK_LOW + USER_STACK_SIZE;
    *(ptr + 4) = USER_STACK_LOW;

    // update fname for simics symbolic debugging
    sim_reg_process(task->page_dir, elf_header.e_fname);

    set_esp0(thread->kern_sp);
    kern_to_user(USER_STACK_START, elf_header.e_entry);

    return 0;
}

int kern_wait(void) {
    int *status_ptr = (int *)get_esi();

    thread_t *thread = get_cur_tcb();
    task_t *task = thread->task;

    if (status_ptr != NULL) {
        int perms = MAP_USER | MAP_WRITE;
        int ret = validate_user_mem((uint32_t)status_ptr, sizeof(int), perms);
        if (ret < 0) return -1;
    }

    task_t *zombie;
    mutex_lock(&(task->wait_mutex));
    if (get_list_size(task->zombie_task_list) > 0) {
        zombie = LIST_NODE_TO_TASK(pop_first_node(task->zombie_task_list));
        mutex_unlock(&(task->wait_mutex));
    } else {
        mutex_lock(&(task->child_task_list_mutex));
        int active_children = get_list_size(task->child_task_list);
        mutex_unlock(&(task->child_task_list_mutex));

        // another fork could happen in between, but that's fine
        if (active_children == 0) {
            mutex_unlock(&(task->wait_mutex));
            return -1;
        }

        // declare on stack
        wait_node_t wait_node;
        wait_node.thread = thread;

        disable_interrupts();

        // this unlock goes after disable_interrupts
        // otherwise, a child could turn into a zombie before blocking
        // then we might incorrectly block forever
        cli_mutex_unlock(&(task->wait_mutex));
        add_node_to_tail(task->waiting_thread_list, &(wait_node.node));
        sche_yield(BLOCKED_WAIT);

        if (wait_node.zombie == NULL) return -1;
        else zombie = wait_node.zombie;
    }

    int ret = zombie->task_id;
    if (status_ptr != NULL) *status_ptr = zombie->status;

    task_destroy(zombie);
    return ret;
}

void kern_vanish(void) {
    thread_t *thread = get_cur_tcb();
    task_t *task = thread->task;

    mutex_lock(&(task->vanish_mutex));
    task_t *parent = task->parent_task;

    mutex_lock(&(task->thread_list_mutex));
    remove_node(task->live_thread_list, TCB_TO_LIST_NODE(thread));
    int live_threads = get_list_size(task->live_thread_list);
    add_node_to_tail(task->zombie_thread_list, TCB_TO_LIST_NODE(thread));
    // TODO try to free previous zombie stack
    if (live_threads > 0) {
        /* 
         * need to disable interrupts here, before unlocking the thread list
         * mutex. otherwise, another thread might be switched to and vanish,
         * finding that it is the last thread to vanish. then the task could
         * be given to a waiting thread and destroyed, and with it our stack...
         */
        disable_interrupts();
        cli_mutex_unlock(&(task->thread_list_mutex));
        cli_mutex_unlock(&(task->vanish_mutex));
        sche_yield(ZOMBIE);
    }
    else {
        mutex_unlock(&(task->thread_list_mutex));

        orphan_children(task);
        orphan_zombies(task);

        if (parent == NULL) lprintf("init or idle task vanished?");
        mutex_lock(&(parent->wait_mutex));

        mutex_lock(&(parent->child_task_list_mutex));
        // assumes we are in the parent's child task list
        remove_node(parent->child_task_list, TASK_TO_LIST_NODE(task));
        mutex_unlock(&(parent->child_task_list_mutex));

        if (get_list_size(parent->waiting_thread_list) > 0) {
            node_t *node = pop_first_node(parent->waiting_thread_list);
            mutex_unlock(&(parent->wait_mutex));

            wait_node_t *waiter = (wait_node_t *)node;
            waiter->zombie = task;

            // must disable interrupts here
            // otherwise, the waiting thread might free us before we yield
            disable_interrupts();
            waiter->thread->status = RUNNABLE;
            sche_push_back(waiter->thread);

            cli_mutex_unlock(&(task->vanish_mutex));
            sche_yield(ZOMBIE);
        } else {
            node_t *last_node = get_last_node(parent->zombie_task_list);
            if (last_node != NULL) {
                // we can free the previous zombie's vm and thread resources
                task_t *prev_zombie = LIST_NODE_TO_TASK(last_node);
                task_clear(prev_zombie);
            }

            // must disable interrupts here
            // otherwise, a waiting thread might free us before we yield
            disable_interrupts();
            add_node_to_tail(parent->zombie_task_list, TASK_TO_LIST_NODE(task));
            cli_mutex_unlock(&(parent->wait_mutex));

            cli_mutex_unlock(&(task->vanish_mutex));
            sche_yield(ZOMBIE);
        }
    }

    // TODO error handling
    lprintf("returned from end of vanish");
}

int kern_yield(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    int tid = (int)esi;
    if (tid < -1) return -1;
    if (tid == -1) {
        sche_yield(RUNNABLE);
        return 0;
    }
    thread_t *thr = tcb_hashtab_get(tid);
    if (thr == NULL) return -1;
    disable_interrupts();
    if (thr->status != RUNNABLE) {
        enable_interrupts();
        return -1;
    }
    sche_push_front(thr);
    sche_yield(RUNNABLE);
    enable_interrupts();
    return 0;
}

int kern_deschedule(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    int *reject = (int *)esi;
    disable_interrupts();
    if (*reject != 0) {
        enable_interrupts();
        return 0;
    }
    sche_yield(SUSPENDED);
    enable_interrupts();
    return 0;
}

int kern_make_runnable(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    int tid = (int)esi;
    thread_t *thr = tcb_hashtab_get(tid);
    if (thr == NULL) return -1;
    if (thr->status != SUSPENDED) return -1;
    disable_interrupts();
    thr->status = RUNNABLE;
    sche_push_back(thr);
    enable_interrupts();
    return 0;
}

int kern_gettid(void) {
    thread_t *thread = get_cur_tcb();
    return thread->tid;
}

int kern_new_pages(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    uint32_t base = (uint32_t)(*esi);
    uint32_t len = (uint32_t)(*(esi + 1));

    if (base & (~PAGE_ALIGN_MASK)) {
        return -1;
    }

    if (len == 0 || len % PAGE_SIZE != 0) {
        return -1;
    }

    uint32_t high = base + (len - 1);
    // check for overflow
    if (high < base) {
        return -1;
    }

    thread_t *thread = get_cur_tcb();
    task_t *task = thread->task;
    if (maps_find(task->maps, base, high)) {
        // already mapped or reserved
        return -1;
    }

    if (dec_num_free_frames(len / PAGE_SIZE) < 0) {
        // not enough memory

        // maps_print(task->maps);
        return -1;
    }

    uint32_t offset;
    for (offset = 0; offset < len; offset += PAGE_SIZE) {
        set_pte(base + offset, zfod_frame, PTE_USER | PTE_PRESENT);
    }

    maps_insert(task->maps, base, high, MAP_USER | MAP_WRITE | MAP_REMOVE);

    return 0;
}

int kern_remove_pages(void) {
    uint32_t base = (uint32_t)get_esi();

    thread_t *thread = get_cur_tcb();
    task_t *task = thread->task;
    map_t *map = maps_find(task->maps, base, base);

    if (!map) {
        // already mapped or reserved
        return -1;
    }

    if (map->low != base) {
        return -1;
    }

    if (!(map->perms & MAP_REMOVE)) {
        return -1;
    }

    uint32_t len = map->high - map->low + 1;
    inc_num_free_frames(len / PAGE_SIZE);

    uint32_t addr, frame;
    for (addr = map->low; addr < map->high; addr += PAGE_SIZE) {
        frame = get_pte(addr) & PAGE_ALIGN_MASK;
        assert(frame != 0);
        if (frame != zfod_frame) free_frame(frame);
        page_inval((void *)addr);
        set_pte(addr, 0, 0);
    }

    maps_delete(task->maps, base);

    return 0;
}

int kern_sleep(void) {
    int ticks = (int)get_esi();

    if (ticks == 0) return 0;
    unsigned int cur_ticks = num_ticks;
    unsigned int wakeup_ticks = cur_ticks + (unsigned int)ticks;
    if (wakeup_ticks < cur_ticks) return -1;

    sleep_node_t sleep_node;
    sleep_node.thread = get_cur_tcb();
    sleep_node.wakeup_ticks = wakeup_ticks;

    disable_interrupts();
    tranquilize(&sleep_node);
    sche_yield(SLEEPING);

    return 0;
}

extern keyboard_buffer_t kb_buf;
int kern_readline(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    int len = (int)(*esi);
    char *buf = (char *)(*(esi + 1));
    if (len > 4096) return -1;
    /* check buf */
    int ret = validate_user_mem((uint32_t)buf, len, MAP_USER | MAP_WRITE);
    if (ret < 0) return -1;

    kern_sem_wait(&kb_buf.readline_sem);
    mutex_lock(&kb_buf.mutex);
    disable_interrupts();
    if (kb_buf.newline_cnt == 0) {
        int kb_buf_ending = kb_buf.buf_ending;
        enable_interrupts();
        kb_buf.is_waiting = 1;
        for (int i = kb_buf.buf_start;
                i < kb_buf_ending;
                i = (i + 1) % KB_BUF_LEN) {
            putbyte(kb_buf.buf[i]);
        }
        kern_cond_wait(&kb_buf.cond, &kb_buf.mutex);
    } else enable_interrupts();

    int if_print = 1;
    if (kb_buf.is_waiting) {
        if_print = 0;
        kb_buf.is_waiting = 0;
    }
    int kb_buf_ending = kb_buf.buf_ending;
    mutex_unlock(&kb_buf.mutex);
    int actual_len = 0;
    while (kb_buf.buf_start < kb_buf_ending) {
        int new_kb_buf_start = (kb_buf.buf_start + 1) % KB_BUF_LEN;
        char ch = kb_buf.buf[kb_buf.buf_start];
        kb_buf.buf_start = new_kb_buf_start;
        buf[actual_len++] = ch;
        if (if_print) putbyte(ch);
        if (ch == '\n') {
            if (actual_len < len) buf[actual_len] = '\0';
            if (actual_len == len) buf[actual_len - 1] = '\0';
            mutex_lock(&kb_buf.mutex);
            kb_buf.newline_cnt--;
            mutex_unlock(&kb_buf.mutex);
            break;
        }
        if (actual_len == len - 1) {
            buf[actual_len] = '\0';
            break;
        }
    }
    kern_sem_signal(&kb_buf.readline_sem);

    return 0;
}

int kern_print(void) {
    // TODO DEBUG
    return 0;

    uint32_t *esi = (uint32_t *)get_esi();
    int len = (int)(*esi);
    char *buf = (char *)(*(esi + 1));

    // TODO macro megabyte
    if (len > 1024 * 1024) return -1;
    int ret = validate_user_mem((uint32_t)buf, len, MAP_USER);
    if (ret < 0) return -1;

    putbytes(buf, len);
    return 0;
}

// TODO put this elsewhere
#define COLOR_MASK 0x7F

int kern_set_term_color(void) {
    int color = (int)get_esi();

    if (color & ~COLOR_MASK) return -1;
    return set_term_color(color);
}

int kern_set_cursor_pos(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    int row = (int)(*esi);
    int col = (int)(*(esi + 1));

    return set_cursor(row, col);
}

int kern_get_cursor_pos(void) {
    uint32_t *esi = (uint32_t *)get_esi();
    int *row = (int *)(*esi);
    int *col = (int *)(*(esi + 1));

    int ret;
    ret = validate_user_mem((uint32_t)row, sizeof(int), MAP_USER | MAP_WRITE);
    if (ret < 0) return -1;
    ret = validate_user_mem((uint32_t)col, sizeof(int), MAP_USER | MAP_WRITE);
    if (ret < 0) return -1;

    get_cursor(row, col);
    return 0;
}

int kern_thread_fork(void) {
    thread_t *old_thread = get_cur_tcb();
    task_t *cur_task = old_thread->task;
    thread_t *new_thread = thread_init();
    int old_tid = old_thread->tid;
    thread_t *cur_thr = NULL;
    if (new_thread == NULL) {
        // TODO destroy task
        // TODO unmap new page directory
        lprintf("kern_thread_fork thread_init() failed");
        return -1;
    }
    mutex_lock(&cur_task->thread_list_mutex);
    add_node_to_head(cur_task->live_thread_list, TCB_TO_LIST_NODE(new_thread));
    mutex_unlock(&cur_task->thread_list_mutex);
    new_thread->task = cur_task;
    new_thread->status = FORKED;
    asm_set_exec_context(old_thread->kern_sp,
                         new_thread->kern_sp,
                         &(new_thread->cur_sp),
                         &(new_thread->ip));
    cur_thr = get_cur_tcb();
    if (cur_thr->tid != old_tid) {
        return 0;
    } else {
        sche_push_back(new_thread);
        return new_thread->tid;
    }
}

unsigned int kern_get_ticks(void) {
    return num_ticks;
}

void kern_halt(void) {
    disable_interrupts();
    printf("Shutdown, Goodbye World!\n");
    sim_halt();
    while (1)
        continue;
}

void kern_set_status(void) {
    int status = (int)get_esi();

    thread_t *thread = get_cur_tcb();
    task_t *task = thread->task;
    task->status = status;
}
