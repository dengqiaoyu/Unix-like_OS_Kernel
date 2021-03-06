/**
 *  @file   vm.c
 *  @brief  this file contains functions that are used to operate between virtual
 *          memory and physical frames, and provides helpers to create, copy,
 *          destroy, read and write memory.
 *  @author Newton Xie (ncx)
 *  @author Qiaoyu Deng (qdeng)
 *  @bug    No known bugs.
 */

/* libc includes */
#include <stdlib.h>
#include <malloc.h>             /* malloc */
#include <string.h>             /* memset */
#include <page.h>               /* PAGE_SIZE */
#include <assert.h>

/* x86 specific includes */
#include <x86/cr.h>             /* set_cr3, set_cr4, set_esp0 */
#include <common_kern.h>        /* machine_phys_frames */

/* DEBUG */
#include <simics.h>             /* lprintf */

#include "vm.h"
#include "vm_internal.h"
#include "asm_page_inval.h"     /* asm_page_inval */
#include "utils/kern_mutex.h"

/* DEBUG */
#define print_line lprintf("line %d", __LINE__)

static uint32_t *kern_page_dir;
/**
 * when new_page is called, all the newly allocated vm is point to this frame
 * until a wrtie operation happen
 */
static uint32_t zfod_frame;

/* physical frames allocator */
static int num_free_frames;
static kern_mutex_t num_free_frames_mutex;

static uint32_t first_free_frame;
static kern_mutex_t first_free_frame_mutex;
/* physical frames allocator */


/**
 * Set up kernel virtual memory, set paging and create free physical frames list
 * which is used to allocate new physical frames.
 */
int vm_init() {
    kern_page_dir = smemalign(PAGE_SIZE, PAGE_SIZE);
    if (kern_page_dir == NULL) return -1;
    memset(kern_page_dir, 0, PAGE_SIZE);

    uint32_t frame = 0;
    int flags = PTE_WRITE | PTE_PRESENT;

    /* 16MB kernel memory only needs 4(NUM_KERN_TABLES) page table  */
    uint32_t *page_tab_addr;
    int i, j;
    for (i = 0; i < NUM_KERN_TABLES; i++) {
        page_tab_addr = smemalign(PAGE_SIZE, PAGE_SIZE);
        if (page_tab_addr == NULL) return -1;

        memset(page_tab_addr, 0, PAGE_SIZE);
        kern_page_dir[i] = (uint32_t)page_tab_addr | flags;

        for (j = 0; j < NUM_PT_ENTRIES; j++) {
            page_tab_addr[j] = frame | flags;
            frame += PAGE_SIZE;
        }
    }

    set_cr3((uint32_t)kern_page_dir);
    set_cr0(get_cr0() | CR0_PG);
    set_cr4(get_cr4() | CR4_PGE);

    int machine_frames = machine_phys_frames();
    kern_mutex_init(&first_free_frame_mutex);
    kern_mutex_init(&num_free_frames_mutex);
    first_free_frame = frame;
    num_free_frames = machine_frames - NUM_KERN_PAGES - 1;

    zfod_frame = PAGE_SIZE * (machine_frames - 1);
    access_physical(zfod_frame);
    memset((void *)RW_PHYS_VA, 0, PAGE_SIZE);

    uint32_t last_frame = zfod_frame - PAGE_SIZE;
    access_physical(last_frame);
    *((uint32_t *)RW_PHYS_VA) = 0;

    while (frame < last_frame) {
        access_physical(frame);
        *((uint32_t *)RW_PHYS_VA) = frame + PAGE_SIZE;
        frame += PAGE_SIZE;
    }

    return 0;
}

// assumes it's in cr3
uint32_t get_pte(uint32_t addr) {
    uint32_t *page_dir = (uint32_t *)get_cr3();
    int pd_index = PD_INDEX(addr);
    int pt_index = PT_INDEX(addr);

    if (!(page_dir[pd_index] & PTE_PRESENT)) return 0;
    else {
        uint32_t *page_tab = ENTRY_TO_ADDR(page_dir[pd_index]);
        return page_tab[pt_index];
    }
}

int set_pte(uint32_t addr, uint32_t frame_addr, int flags) {
    uint32_t *page_dir = (uint32_t *)get_cr3();
    int pd_index = PD_INDEX(addr);
    int pt_index = PT_INDEX(addr);

    if (!(page_dir[pd_index] & PTE_PRESENT)) {
        void *ret = smemalign(PAGE_SIZE, PAGE_SIZE);
        if (ret == NULL) return -1;
        memset(ret, 0, PAGE_SIZE);

        page_dir[pd_index] = (uint32_t)ret;
        page_dir[pd_index] |= PTE_USER | PTE_WRITE | PTE_PRESENT;
    }

    uint32_t *page_tab = ENTRY_TO_ADDR(page_dir[pd_index]);
    page_tab[pt_index] = frame_addr | flags;

    return 0;
}

uint32_t get_frame() {
    kern_mutex_lock(&first_free_frame_mutex);
    uint32_t frame = first_free_frame;
    assert(frame != 0);

    access_physical(frame);
    first_free_frame = *((uint32_t *)RW_PHYS_VA);

    memset((void *)RW_PHYS_VA, 0, PAGE_SIZE);
    kern_mutex_unlock(&first_free_frame_mutex);
    return frame;
}

void free_frame(uint32_t frame) {
    kern_mutex_lock(&first_free_frame_mutex);
    access_physical(frame);

    *((uint32_t *)RW_PHYS_VA) = first_free_frame;
    first_free_frame = frame;
    kern_mutex_unlock(&first_free_frame_mutex);
}

int dec_num_free_frames(int n) {
    int ret = 0;
    kern_mutex_lock(&num_free_frames_mutex);
    if (num_free_frames < n) ret = -1;
    else num_free_frames -= n;
    kern_mutex_unlock(&num_free_frames_mutex);
    return ret;
}

void inc_num_free_frames(int n) {
    kern_mutex_lock(&num_free_frames_mutex);
    num_free_frames += n;
    kern_mutex_unlock(&num_free_frames_mutex);
}

uint32_t *page_dir_init() {
    uint32_t *page_dir = smemalign(PAGE_SIZE, PAGE_SIZE);
    if (page_dir == NULL) return NULL;
    memset(page_dir, 0, PAGE_SIZE);

    int i;
    for (i = 0; i < NUM_KERN_TABLES; i++) {
        page_dir[i] = kern_page_dir[i];
    }
    return page_dir;
}

int page_dir_clear(uint32_t *page_dir) {
    int i, j;
    for (i = NUM_KERN_TABLES; i < NUM_PD_ENTRIES; i++) {
        uint32_t pde = page_dir[i];
        if ((pde & PDE_PRESENT) == 0) continue;
        uint32_t *page_tab = ENTRY_TO_ADDR(pde);

        for (j = 0; j < NUM_PT_ENTRIES; j++) {
            uint32_t pte = page_tab[j];
            if ((pte & PTE_PRESENT) == 0) continue;

            // don't mess with the RW_PHYS reserved page
            if (i == RW_PHYS_PD_INDEX && j == RW_PHYS_PT_INDEX) continue;
            page_tab[j] = 0;
            uint32_t frame = pte & PAGE_ALIGN_MASK;
            if (frame != zfod_frame) free_frame(frame);
            inc_num_free_frames(1);
        }

        page_dir[i] = 0;
        sfree(page_tab, PAGE_SIZE);
    }

    return 0;
}

int page_dir_copy(uint32_t *new_page_dir, uint32_t *old_page_dir) {
    int i, j;
    int fail = 0;
    for (i = NUM_KERN_TABLES; i < NUM_PD_ENTRIES; i++) {
        uint32_t old_pde = old_page_dir[i];
        if ((old_pde & PDE_PRESENT) == 0) continue;
        int new_pde_flag = old_pde & PAGE_FLAG_MASK;
        uint32_t *old_page_tab = ENTRY_TO_ADDR(old_pde);

        uint32_t *new_page_tab = smemalign(PAGE_SIZE, PAGE_SIZE);
        if (new_page_tab == NULL) {
            fail = 1;
            break;
        }
        memset(new_page_tab, 0, PAGE_SIZE);
        new_page_dir[i] = (uint32_t)new_page_tab | new_pde_flag;

        for (j = 0; j < NUM_PT_ENTRIES; j++) {
            uint32_t old_pte = old_page_tab[j];
            if ((old_pte & PTE_PRESENT) == 0) continue;

            if (dec_num_free_frames(1) < 0) {
                fail = 1;
                break;
            }

            uint32_t frame = old_pte & PAGE_ALIGN_MASK;
            if (frame == zfod_frame) {
                new_page_tab[j] = old_pte;
                continue;
            }

            // don't mess with the RW_PHYS reserved page
            if (i == RW_PHYS_PD_INDEX && j == RW_PHYS_PT_INDEX) continue;
            uint32_t new_physical_frame = get_frame();
            int new_pte_flag = old_pte & PAGE_FLAG_MASK;
            uint32_t new_pte = new_physical_frame | new_pte_flag;

            uint32_t virtual_addr = ((uint32_t)i << 22) | ((uint32_t)j << 12);
            write_physical(new_physical_frame, (void *)virtual_addr, PAGE_SIZE);
            new_page_tab[j] = new_pte;
        }
        if (fail) break;
    }
    if (fail) {
        page_dir_clear(new_page_dir);
        return -1;
    }
    return 0;
}

// maps RW_PHYS_VA to physical address addr
void access_physical(uint32_t addr) {
    asm_page_inval((void *)RW_PHYS_VA);
    uint32_t entry = addr & PAGE_ALIGN_MASK;
    set_pte(RW_PHYS_VA, entry, PTE_WRITE | PTE_PRESENT);
}

void read_physical(void *virtual_dest, uint32_t phys_src, uint32_t n) {
    kern_mutex_lock(&first_free_frame_mutex);

    access_physical(phys_src);
    uint32_t page_offset = phys_src & ~PAGE_ALIGN_MASK;

    int len;
    if (page_offset + n < PAGE_SIZE) len = n;
    else len = PAGE_SIZE - page_offset;

    uint32_t virtual_src = RW_PHYS_VA + page_offset;
    memcpy(virtual_dest, (void *)virtual_src, len);

    kern_mutex_unlock(&first_free_frame_mutex);
}

void write_physical(uint32_t phys_dest, void *virtual_src, uint32_t n) {
    kern_mutex_lock(&first_free_frame_mutex);

    access_physical(phys_dest);
    uint32_t page_offset = (uint32_t)virtual_src & ~PAGE_ALIGN_MASK;

    int len;
    if (page_offset + n < PAGE_SIZE) len = n;
    else len = PAGE_SIZE - page_offset;

    uint32_t virtual_dest = RW_PHYS_VA + page_offset;
    memcpy((void *)virtual_dest, virtual_src, len);

    kern_mutex_unlock(&first_free_frame_mutex);
}

uint32_t *get_kern_page_dir(void) {
    return kern_page_dir;
}

uint32_t get_zfod_frame(void) {
    return zfod_frame;
}
