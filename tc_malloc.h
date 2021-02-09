#ifndef __TCMALLOC_H__
#define __TCMALLOC_H__

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#define SHIFTAMT 12
#define PAGEHEAPSIZE 256
#define PAGEMAPSIZE (1 << 23) //adjust it
#define PAGEINBYTES (1 << 12)
#define THRESHALLOC (32 * 1024)
#define SYSFETCHSIZE (1 << 28)
#define CLASSES 159
#define SPANCHUNKSIZE (1 << 23) //adjust it
#define THRESHBYTES (1 << 12)
#define BATCH 128
#define SPANPOOLSIZE 1440
#define MAXFETCHSIZE 32


#define pages_to_bytes(npage) (npage << SHIFTAMT)
#define bytes_to_pages(nbytes) (nbytes >> SHIFTAMT)

#define interp(addr) ((char *)addr)
#define addr_to_index(end, start) (bytes_to_pages((uintptr_t)end) - bytes_to_pages((uintptr_t) start))
#define index_to_addr(id, ini_addr) ((id << SHIFTAMT) + interp(ini_addr))
#define ALIGN(size, alignment) (size + alignment - 1) & ~(alignment - 1)
#define MIN(x, y) ((x) > (y) ? (y) : (x))
#define MAX(x, y) ((x) > (y) ? (x) : (y))

typedef struct linkedlist
{
    struct linkedlist *next;
} linkedlist_t;

typedef linkedlist_t *freelist;

typedef enum
{
    pageheap,
    isolated,
    central_list,
    central_idle
} spanLocation;

typedef struct Span
{
    struct Span *prev;
    struct Span *next;
    freelist objs;
    size_t obj_len;
    spanLocation loc;
    void *start;
    uint32_t objs_size;
    uint32_t obj_refcount;
    uint32_t pageSize;
    uint32_t pageId;
} _span;

#define get_span(ptr, index) (span_t) (interp(ptr) + index * sizeof(_span))

typedef _span *span_t;

typedef struct spanChunk
{
    void * chunk;
    size_t rem_bytes;
    size_t curr_ind;
} spans;

typedef struct pageToSpanMap
{
    /* stores the mapping between pageId and Span*/
    span_t page_mapping[PAGEMAPSIZE];
    size_t curr_index;

} spanMap;

typedef struct pageHeap
{
    /* main list that holds the free pages */
    span_t list[PAGEHEAPSIZE];
    void *ini_brk_addr;
} pagecache;

typedef struct centralList
{
    span_t list[CLASSES];
    span_t idle[CLASSES];
} centralCache;

typedef struct Threadlist
{
    freelist objs;
    size_t objlen;
    int max_length;
} threadlist;

typedef struct ThreadCache
{
    threadlist list[CLASSES];
} thrcache;




/*====================================================*/
/*      implemented spanlist here                     */

void 
spanlist_push(span_t *head, span_t span)
{
    assert(head && span);

    span_t ptr = *head;

    if (!ptr)
    {
        *head = span;
        span->next = span;
        span->prev = span;
        return;
    }

    span_t prev = ptr->prev;

    if(!prev)
    {
        // if here huge bug!!!
        ptr->prev = ptr;
        ptr->next = ptr;
        prev = ptr;
    }

    span->next = ptr;
    span->prev = prev;
    ptr->prev = span;
    prev->next = span;

    *head = span;
}

void
spanlist_pop(span_t *head, span_t span)
{
    assert(span);

    /* only one element in the double linked list */
    if (span == span->next)
    {
        *head = NULL;
        return;
    }
    
    span_t p = span->prev;
    span_t n = span->next;

    if(!p)
    {
        p = span;
    }

    if(!n)
    {
        n = span;
    }

    p->next = n;
    n->prev = p;

    if(*head == span)
    {
        if(span->next == span && span->prev == span)
        {
            *head = NULL;
        }
        else
        {
            *head = n;
        }
        
    }

    else
    {
        (*head)->next = n;
    }
    
    
    /* make span a selfish node */
    span->next = span;
    span->prev = span;
}

/*====================================================*/

/*====================================================*/
/*      implemented freelist here                     */

void 
freelist_push(span_t span, freelist ptr)
{
    assert(span && ptr);
    freelist head = span->objs;

    ptr->next = head;
    span->objs = ptr;
    span->obj_len++;
}


freelist
freelist_pop(size_t * len,freelist *head)
{
    assert(head);

    freelist next, curr;

    curr = (*head);
    next = (*head)->next;
    (*head) = next;
    (*len)--;

    return curr;
}
/*====================================================*/

/*====================================================*/
/* mapping between size and index and vice-versa      */

int index_to_size(int index)
{
    int class = index + 1;

    if (index <= 7)
    {
        class *= 8;
    }
    else if (index <= 38)
    {
        class -= 8;
        class *= 64;
        class += 64;
    }
    else if (index <= 158)
    {
        class -= 39;
        class *= 256;
        class += 2048;
    }

    return class;
}

int
get_index(size_t size)
{
    int ends[] = {1, 65, 2049};
    int alignments[] = {8, 64, 256};
    int prev_index[] = {8, 31};

    int index = -1;

    if (size <= 64)
    {
        index = (size - ends[0]) / alignments[0];
    }
    else if (size <= 2048)
    {
        index = (size - ends[1]) / alignments[1] + prev_index[0];
    }
    else if (size <= 32 * 1024)
    {
        index = (size - ends[2]) / alignments[2] + prev_index[0] + prev_index[1];
    }
    else
    {
        fprintf(stderr, "not a small size object\n");
    }

    return index;
}

/*====================================================*/

/*=================== PAGE CACHE API ================================== */
void fetch_from_system();
span_t fetch_span(size_t npage);
void return_span_to_pgcache(span_t span_ptr);
/*===================================================================== */


/*=================== CENTRAL CACHE API =============================== */
span_t get_span_central(size_t size);
int fetch_objs(freelist *start, freelist *end, size_t num, size_t bytes);
void return_objs_to_centralcache(freelist ptr, size_t nbytes);
/*=======================================================================*/


/*==================== THREAD CACHE API==================================*/
void insert_to_threadcache(freelist objs, size_t nbytes);
void fetch_from_centralcache(int index);
void * allocate(size_t nbytes);
void deallocate(void * alloc_ptr,size_t nbytes);
/*========================================================================*/

/*================== INITIALIZATION ======================================*/    
void init_globals();
/*=========================================================================*/



/*===================  MAIN API ===========================================*/
void *tc_central_init();
void *tc_thread_init();
void *tc_malloc(size_t size);
void tc_free(void *ptr);
/*=========================================================================*/

#endif
