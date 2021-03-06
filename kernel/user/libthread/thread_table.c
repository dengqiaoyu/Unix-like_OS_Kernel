/** @file thread_table.c
 *  @brief Implements the thread table used by the thread library.
 *
 *  The thread table is a "hash table" which stores thread control blocks.
 *  It is an array of linked lists which indexes thread blocks by tid.
 *  Distinct linked lists can safely be searched and updated in parallel.
 *  The initializer also create an array of mutexes of the same size as
 *  the table, and clients must lock these mutexes to ensure thread safety
 *  within the linked lists.
 *
 *  The table handles allocation of memory for thread blocks using block
 *  allocators (defined in allocator.c). These allocators allow several
 *  thread blocks to be created in parallel. The table assumes that all
 *  insertions use thread blocks allocated by the table interface.
 *
 *  @author Newton Xie (ncx)
 *  @author Qiaoyu Deng (qdeng)
 *  @bug none known
 */

#include <stdlib.h>
#include <assert.h>
#include <mutex.h>
#include <simics.h>
#include "thr_internals.h"
#include "thread_table.h"
#include "allocator.h"

/* struct for table entry which allows casting from thr_info pointer */
typedef struct table_node table_node_t;
struct table_node {
    thr_info tinfo;
    table_node_t *prev;
    table_node_t *next;
};

/* counter to distribute allocation load evenly across allocators */
unsigned int counter;
mutex_t counter_mutex;

allocator_t **thread_allocators;
table_node_t **thread_table;
mutex_t *table_mutexes;

int thread_table_init() {
    counter = 0;
    if (mutex_init(&counter_mutex) != 0) return -1;

    thread_table = calloc(THREAD_TABLE_SIZE, sizeof(void *));
    if (thread_table == NULL) return -1;

    thread_allocators = calloc(NUM_THREAD_ALLOCATORS, sizeof(void *));
    if (thread_allocators == NULL) return -1;
    int i;
    for (i = 0; i < NUM_THREAD_ALLOCATORS; i++) {
        allocator_t **ptr = &(thread_allocators[i]);
        if (allocator_init(ptr, sizeof(table_node_t), THREAD_ALLOCATOR_SIZE))
            return -1;
    }

    table_mutexes = calloc(THREAD_TABLE_SIZE, sizeof(mutex_t));
    if (table_mutexes == NULL) return -1;
    for (i = 0; i < THREAD_TABLE_SIZE; i++) {
        if (mutex_init(&(table_mutexes[i])) != 0) return -1;
    }
    return 0;
}

thr_info *thread_table_alloc() {
    mutex_lock(&counter_mutex);
    int count = counter++;
    mutex_unlock(&counter_mutex);

    assert(thread_allocators != NULL);
    allocator_t *allocator = thread_allocators[THREAD_ALLOCATOR_INDEX(count)];
    assert(allocator != NULL);

    thr_info *new_node = allocator_alloc(allocator);
    if (new_node == NULL) return NULL;
    return new_node;
}

void thread_table_insert(int tid, thr_info *tinfo) {
    assert(tinfo != NULL);
    assert(thread_table != NULL);
    int tid_list = THREAD_TABLE_INDEX(tid);
    table_node_t *new_node = (table_node_t *)tinfo;

    new_node->prev = NULL;
    if (thread_table[tid_list] == NULL) {
        thread_table[tid_list] = new_node;
        new_node->next = NULL;
    } else {
        thread_table[tid_list]->prev = new_node;
        new_node->next = thread_table[tid_list];
        thread_table[tid_list] = new_node;
    }
}

thr_info *thread_table_find(int tid) {
    assert(thread_table != NULL);
    int tid_list = THREAD_TABLE_INDEX(tid);
    table_node_t *temp;

    temp = thread_table[tid_list];
    while (temp != NULL) {
        if (temp->tinfo.tid == tid) {
            return &(temp->tinfo);
        } else temp = temp->next;
    }
    return NULL;
}

void thread_table_delete(thr_info *tinfo) {
    assert(tinfo != NULL);
    assert(thread_table != NULL);
    int tid_list = THREAD_TABLE_INDEX(tinfo->tid);
    table_node_t *temp = (table_node_t *)tinfo;

    if (temp->prev != NULL) temp->prev->next = temp->next;
    else thread_table[tid_list] = temp->next;
    if (temp->next != NULL) temp->next->prev = temp->prev;
    allocator_free(temp);
}

mutex_t *thread_table_get_mutex(int tid) {
    assert(table_mutexes != NULL);
    int tid_list = THREAD_TABLE_INDEX(tid);
    return &(table_mutexes[tid_list]);
}

