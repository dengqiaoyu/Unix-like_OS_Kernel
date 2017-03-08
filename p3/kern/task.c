/** @file task.c
 *  @author Newton Xie (ncx)
 *  @bug No known bugs.
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <simics.h>
#include <page.h>
#include <x86/cr.h>

#include "vm.h"
#include "task.h"

int task_id_counter = 0;
int thread_id_counter = 0;
task_t *cur_task;
thread_t *cur_thread;

extern uint32_t *kern_page_dir;

task_t *task_init(const char *fname)
{
    task_t *task = malloc(sizeof(task_t));
    task->task_id = task_id_counter++;
    task->main_thread = thread_init();
    task->main_thread->task = task;

    task->page_dir = smemalign(PAGE_SIZE, PAGE_SIZE);
    memset(task->page_dir, 0, PAGE_SIZE);
    int i;
    for (i = 0; i < NUM_KERN_TABLES; i++) {
        task->page_dir[i] = kern_page_dir[i];
    }

    simple_elf_t elf_header;
    elf_load_helper(&elf_header, fname);
    load_program(&elf_header, task->page_dir);
    task->main_thread->ip = elf_header.e_entry;

    return task;
}

thread_t *thread_init()
{
    thread_t *thread = malloc(sizeof(thread_t));
    thread->tid = thread_id_counter++;
    
    void *kern_stack = malloc(KERN_STACK_SIZE);
    thread->kern_sp = (uint32_t)kern_stack + KERN_STACK_SIZE;
    thread->user_sp = USER_STACK_LOW + USER_STACK_SIZE;

    return thread;
}

int load_program(simple_elf_t *header, uint32_t *page_dir)
{
    set_cr3((uint32_t)page_dir);

    load_elf_section(header->e_fname, header->e_txtstart, header->e_txtlen,
                     header->e_txtoff, PTE_USER, page_dir);
    load_elf_section(header->e_fname, header->e_datstart, header->e_datlen,
                     header->e_datoff, PTE_USER | PTE_WRITE, page_dir);
    load_elf_section(header->e_fname, header->e_rodatstart, header->e_rodatlen,
                     header->e_rodatoff, PTE_USER, page_dir);
    load_elf_section(header->e_fname, header->e_bssstart, header->e_bsslen,
                     -1, PTE_USER | PTE_WRITE, page_dir);
    load_elf_section(header->e_fname, USER_STACK_LOW, USER_STACK_SIZE,
                     -1, PTE_USER | PTE_WRITE, page_dir);

    set_cr3((uint32_t)kern_page_dir);

    return 0;
}

int load_elf_section(const char *fname, unsigned long start, unsigned long len,
                     long offset, int pte_flags, uint32_t *page_dir)
{
    uint32_t low = (uint32_t)start & PAGE_MASK;
    
    uint32_t i = 0;
    do {
        uint32_t *page = get_free_page();
        set_pte(page_dir, low + i, page, pte_flags);
        i += PAGE_SIZE;
    } while (i < len);

    memset((void *)start, 0, len);
    if (offset != -1) {
        getbytes(fname, offset, len, (void *)start);
    }

    return 0;
}
