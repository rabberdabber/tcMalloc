#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "tc_malloc.h"

/*================================================*/
/* global vars which are shared among the threads */
pagecache pgcache;
spanMap mp_obj;
spans spanchunk;
centralCache centralcache;

/* one per thread */
static __thread thrcache threadlocal;
/*================================================ */

// locks
pthread_spinlock_t centralcache_lock;
pthread_spinlock_t pageheap_lock;


static void
update_mapping(span_t new_span)
{
    int len = new_span->pageSize;

    for (int i = 0; i < len; i++)
    {
        mp_obj.page_mapping[new_span->pageId + i] = new_span;
    }
    mp_obj.curr_index = MAX(mp_obj.curr_index,new_span->pageId + len);
}

static void
remove_span(span_t *head, span_t span)
{

    /* if first element in the list */
    if (*head == span)
    {
        spanlist_pop(head, *head);
    }
    /* if not the first element in the list */
    else
    {
        spanlist_pop(&span->prev, span);
    }
}

static span_t
alloc_span(size_t npages, void *ptr)
{
    span_t new_span;

    /* try to allocate or recycle span */
    if (spanchunk.rem_bytes < sizeof(_span))
    {
    
        int index, nbytes;
        span_t meta_span;

        nbytes = pages_to_bytes(SPANPOOLSIZE);

        spanchunk.curr_ind = 0;
        spanchunk.rem_bytes = nbytes;
        spanchunk.chunk = sbrk(nbytes);
        memset(spanchunk.chunk, 0, nbytes);

        spanchunk.rem_bytes = nbytes;

        /* allocate to represent the span allocator block in page map */
        meta_span = (span_t)get_span(spanchunk.chunk, 0);
        meta_span->pageId = addr_to_index(spanchunk.chunk, pgcache.ini_brk_addr);
        meta_span->pageSize = SPANPOOLSIZE;
        update_mapping(meta_span);

        new_span = get_span(spanchunk.chunk, 1);
        spanchunk.rem_bytes -= 2 * sizeof(_span);
        spanchunk.curr_ind += 2;
        
    }

    /* fetch a span from span chunk */
    else
    {
        int index = spanchunk.curr_ind;
        new_span = get_span(spanchunk.chunk, index);
        spanchunk.curr_ind++;
        spanchunk.rem_bytes -= sizeof(_span);
    }

    new_span->pageSize = npages;

    /* skip if ptr is NULL */
    if (ptr)
    {
        new_span->pageId = addr_to_index(ptr, pgcache.ini_brk_addr);
    }

    new_span->next = new_span;
    new_span->prev = new_span;

    return new_span;
}


static span_t
map_addr_to_span(void *ptr)
{
    size_t pgid = addr_to_index(ptr, pgcache.ini_brk_addr);
    return mp_obj.page_mapping[pgid];
}


/*================================= PAGE HEAP METHODS =====================================*/
void fetch_from_system()
{
    /* allocate sysfetchsize amt from os and insert span in page cache */
    /* update the span before inserting in the page cache              */
    void *alloc_ptr = sbrk((intptr_t)SYSFETCHSIZE);
    
    if (!alloc_ptr)
    {
        perror("mmap");
        fprintf(stderr, "cannot fetch memory from the os\n");
    }

    span_t new_span = alloc_span(bytes_to_pages(SYSFETCHSIZE), alloc_ptr);
    new_span->start = alloc_ptr;
    

    /* insert the span to pagecache */
    spanlist_push(&pgcache.list[MIN(PAGEHEAPSIZE - 1,new_span->pageSize - 1)], new_span);

    /* update the mapping between pageid and span */
    update_mapping(new_span);
};



static void
split_spans(span_t tmp, span_t new_span, size_t npage)
{
    tmp->pageSize -= npage;
    
    new_span->start = interp(tmp->start) + pages_to_bytes(tmp->pageSize);

    /* make it selfish node */
    new_span->next = new_span;
    new_span->prev = new_span;

    if(tmp->pageSize)
    {
        /* insert tmp into its appropriate pagecache */
        spanlist_push(&pgcache.list[MIN(PAGEHEAPSIZE - 1, tmp->pageSize - 1)], tmp);
    }

    new_span->pageId = tmp->pageId + tmp->pageSize;
    update_mapping(new_span);
}

span_t
fetch_span(size_t npage)
{
    span_t tmp;
    /* if page cache[npage] is not empty */
    if ((tmp = pgcache.list[MIN(PAGEHEAPSIZE - 1, npage - 1)]) != NULL)
    {
        remove_span(&pgcache.list[MIN(PAGEHEAPSIZE - 1, npage - 1)], tmp);
        return tmp;
    }

    /* if it is empty iterate to the next pagecache entry*/
    /* if entry found return the span */
    else
    {
        span_t new_span = alloc_span(npage, NULL);
        span_t head;

        if(npage > 8)
        {
            new_span->loc = isolated;
        }

        for (int i = npage; i < PAGEHEAPSIZE; i++)
        {
            if ((tmp = pgcache.list[i]))
            {
                /* pop it */
                remove_span(&pgcache.list[i], tmp);

                split_spans(tmp,new_span,npage);
                return new_span;
            }
        }
        

        tmp = head = pgcache.list[PAGEHEAPSIZE-1];

        /* if npage is very big */
        do
        {

            if(tmp)
            {
                if(tmp->pageSize >= npage)
                {
                    remove_span(&pgcache.list[PAGEHEAPSIZE-1],tmp);
                    split_spans(tmp,new_span,npage);
                    return new_span;
                
                }

                tmp = tmp->next;
            }
        }while(tmp != NULL && tmp != head);
        
    
    }

    if(npage > PAGEHEAPSIZE)
    {
        void *alloc_ptr = sbrk((intptr_t)pages_to_bytes(npage));

        if (!alloc_ptr)
        {
            perror("mmap");
            fprintf(stderr, "cannot fetch memory from the os\n");
        }

        span_t new_span = alloc_span(npage, alloc_ptr);
        new_span->start = alloc_ptr;

        update_mapping(new_span);
        return new_span;

    }

    /* else fetch from system and fetch_span again */
    fetch_from_system();
    return fetch_span(npage);
};



/* locking required */
void return_span_to_pgcache(span_t span_ptr)
{

    pthread_spin_lock(&pageheap_lock);

    int prevId, nextId,maxId;
    span_t prevSpan, nextSpan;


    prevId = (int)span_ptr->pageId - 1;

    
    /* find prevSpan and nextSpan */
    if(prevId == -1)
    {
        prevSpan = NULL;
    }
    else
    {
        prevSpan = mp_obj.page_mapping[prevId];
    }
    

    nextId = span_ptr->pageId + span_ptr->pageSize;

    nextSpan = mp_obj.page_mapping[nextId];
    
    /* coalescing */
    /* little buggy */
    /* if prevSpan is mergable */
    /*if (prevSpan && prevSpan->loc == pageheap)*/
    {
        /*=============================================*/
        /* update the spanPtr and remove prevSpan and  */
        /* recycle it,insert the spanPtr in the pgcache*/
       /* span_ptr->pageId = prevSpan->pageId;
        span_ptr->pageSize  += prevSpan->pageSize;
        span_ptr->start = prevSpan->start;
        span_ptr->loc = pageheap;
        remove_span(&pgcache.list[MIN(PAGEHEAPSIZE-1,prevSpan->pageSize-1)],prevSpan);*/
        /*==============================================*/        
    }
    
    /* if nextSpan is mergable */
    /*if (nextSpan && nextSpan->loc == pageheap)*/
    {
        /*===============================================*/
        /* update the spanPtr and remove nextSpan and    */
        /* recycle it,insert the spanPtr in the pgcache  */

        /*span_ptr->pageSize += nextSpan->pageSize;
        span_ptr->loc = pageheap;

        remove_span(&pgcache.list[MIN(PAGEHEAPSIZE-1,nextSpan->pageSize-1)],nextSpan);*/
        /*===============================================*/
    }

    /* rewrite the fields */
    span_ptr->objs_size = 0;
    span_ptr->obj_len = 0;
    span_ptr->objs = NULL;
    span_ptr->loc = pageheap;

    spanlist_push(&pgcache.list[MIN(PAGEHEAPSIZE-1,span_ptr->pageSize-1)],span_ptr);
    update_mapping(span_ptr);  

    pthread_spin_unlock(&pageheap_lock);

}


/*=================================================================================================*/

/*=============================== CENTRAL CACHE METHODS *==========================================*/

/* needs locking to avoid contention */
span_t
get_span_central(size_t size)
{
    span_t span_start, span_ptr;
    centralCache *cache = &centralcache;
    int npage;

    int index = get_index(size);
    span_start = span_ptr = cache->list[index];

    /*========================================*/
    /* try to find a span in the central list */
    do
    {
        if(span_ptr){
            if (span_ptr->objs)
                return span_ptr;

            span_ptr = span_ptr->next;
        }
    }while(span_ptr != NULL && span_ptr != span_start);
    /*========================================*/

    /*========================================*/
    /* fetch a span from page cache      */
    /* update the objlist objlen              */
    npage = BATCH;

    //lock
    pthread_spin_lock(&pageheap_lock);
    span_t new_span = fetch_span(npage);
    pthread_spin_unlock(&pageheap_lock);
   
    void * start = new_span->start;
    freelist obj;
    char * end = (interp(start) + pages_to_bytes(new_span->pageSize));


    new_span->objs_size = size;

    for (char * ptr = (char *) start; ptr + size <= end; ptr += size)
    {
        // init freelist
        obj = (freelist) ptr;
        obj->next = NULL;
        freelist_push(new_span,(freelist) ptr);
    }

    spanlist_push(&cache->list[index], new_span);
    new_span->loc = central_list;

    /*=========================================*/
    return new_span;
}


int
fetch_objs(freelist *start, freelist *end, size_t num, size_t nbytes)
{
    int n = 0,index,max_iter;
    span_t span = get_span_central(nbytes);
    freelist ptr, prev = NULL;

    index = get_index(nbytes);
    max_iter = MIN(num,span->obj_len-span->obj_refcount);

    /*======================================================*/
    /* find  min(num,len(spans->objlen-refcount)) element in list  */
    for (ptr = span->objs; ptr && (n < max_iter); ptr = ptr->next)
    {
        prev = ptr;
        n++;
    }
    /*======================================================*/

    /* make the iterator a freelist */
    *start = span->objs;
    *end = prev;

    (*end)->next = NULL;

    /* update the span fields */
    span->objs = ptr;
    span->obj_refcount += n;

    if(span->obj_refcount == span->obj_len)
    {
        remove_span(&centralcache.list[index],span);
        spanlist_push(&centralcache.idle[index],span);
        span->loc = central_idle;
    }

    return n;
}

void return_objs_to_centralcache(freelist start, size_t nbytes)
{
    pthread_spin_lock(&centralcache_lock);
    span_t span_ptr;
    size_t index = get_index(nbytes);
    freelist next;

    /* for each element in the freelist map the ptr*/
    /* to its span and push it into the span       */
    
    
    for (; start;)
    {
        next = start->next;
        span_ptr = map_addr_to_span((void *)start);
        freelist_push(span_ptr, start);
        span_ptr->obj_refcount--;

        start = next;
    }

    /* after returning object check if it was in idle section */
    /* if it was return it to the list                        */
    if(span_ptr->loc == central_idle && span_ptr->obj_refcount != span_ptr->obj_len)
    {
        remove_span(&centralcache.idle[index],span_ptr);
        spanlist_push(&centralcache.list[index],span_ptr);
    }

    /* if all objects have been returned*/
    if (span_ptr->obj_refcount == 0)
    {
        remove_span(&centralcache.list[index], span_ptr);

        span_ptr->next = span_ptr;
        span_ptr->prev = span_ptr;
        span_ptr->objs_size = span_ptr->obj_len = 0;
        span_ptr->objs = NULL;

        /* insert span_ptr to pagecache */
        return_span_to_pgcache(span_ptr);
    }
    pthread_spin_unlock(&centralcache_lock);
}

/*================================================================================================*/

/*==================== THREAD CACHE METHODS ======================================================*/
/* make sure to call this function so that freelist len is not greater then threshold             */
void insert_to_threadcache(freelist objs, size_t nbytes)
{

    /* find the appropriate class corresponding to nbytes */
    size_t size_class = get_index(nbytes);
    threadlist *head = &threadlocal.list[size_class];
    freelist ptr,next;


    for (;objs;)
    {
        /* insert at the beginning */
        next = objs->next;

        objs->next = head->objs;
        head->objs = objs;

        objs = next;

        head->objlen++;
    }
}

/* needs locking */
void 
fetch_from_centralcache(int index)
{
    size_t opt_size,max,nbytes;
    freelist start = NULL,end = NULL;
    threadlist *local_list = &threadlocal.list[index];
    int fetch_amt = 0;

    nbytes = index_to_size(index);

    opt_size = MAXFETCHSIZE;
    max = local_list->max_length;


    /* find optimal size for threadcache to receive from central cache */
    opt_size = MIN(opt_size,max);

    /* max is the bottleneck */
    if(opt_size < MAXFETCHSIZE && index <= 22)
    {
        local_list->max_length *= 2;
    }
    else if(opt_size < MAXFETCHSIZE)
    {
        local_list->max_length++;
    }

    //lock
    pthread_spin_lock(&centralcache_lock);
    fetch_amt = fetch_objs(&start,&end,opt_size,nbytes);
    pthread_spin_unlock(&centralcache_lock);

    insert_to_threadcache(start,nbytes);

}

void * 
allocate(size_t nbytes)
{
    int index = get_index(nbytes);
    freelist pop;

    threadlist *local_list = &threadlocal.list[index];
    freelist objs = local_list->objs;

    if(objs)
    {
        pop = freelist_pop(&local_list->objlen,&local_list->objs);
    }
    else
    {
        fetch_from_centralcache(index);
        pop = freelist_pop(&local_list->objlen, &local_list->objs);
    }
    
    return (void *)pop;
}

void 
deallocate(void * alloc_ptr,size_t nbytes)
{
    int index = get_index(nbytes);
    size_t curr_size,max;
    threadlist * local_list;
    freelist head,ptr;

    local_list = &threadlocal.list[index];
    curr_size = local_list->objlen;
    max = local_list->max_length;
    
    /* store the head and clear local_list  */
    head = local_list->objs;

    ptr = (freelist) alloc_ptr;
    ptr->next = head;
    local_list->objlen++;


    if(curr_size == max)
    {
        local_list->objs = NULL;
        local_list->objlen = 0;

        return_objs_to_centralcache(ptr,nbytes);
       
    }
    else
    {
        local_list->objs = ptr;
    }

}
/*===============================================================================================*/

void
init_globals()
{
    /* init page cache */
    memset((void *)&pgcache, 0, sizeof(pagecache));
    memset(&mp_obj, 0, sizeof(spanMap));
    /* init span Chunk */
    memset(&spanchunk, 0, sizeof(spans));
    /* init central cache */
    memset(&centralcache, 0, sizeof(centralCache));

    return;
}

static void 
init_threadlist(threadlist *list)
{
    list->objlen = 0;
    list->objs = NULL;
    list->max_length = 1;//slow start
}

static void 
init_lock()
{
    pthread_spin_init(&centralcache_lock,1);
    pthread_spin_init(&pageheap_lock,1);
}

void *
tc_thread_init()
{

    for(int i = 0;i < CLASSES;i++)
    {
        init_threadlist(&threadlocal.list[i]);
        fetch_from_centralcache(i);
    }

    return &threadlocal.list;
}

void *
tc_central_init()
{
    /* set the initial heap address */
    /* used for pageMapping         */
    init_globals();
    init_lock();
    pgcache.ini_brk_addr = sbrk(0);

    for(int i =0;i < CLASSES;i++)
    {
        pthread_spin_lock(&centralcache_lock);
        get_span_central(index_to_size(i));
        pthread_spin_unlock(&centralcache_lock);
    }

    return &pgcache.list;
}

void *
tc_malloc(size_t size)
{
    if(size <= THRESHALLOC)
    {
        return allocate(size);
    }

    pthread_spin_lock(&pageheap_lock);
    span_t span = fetch_span(bytes_to_pages(size));
    pthread_spin_unlock(&pageheap_lock);

    return span->start;


}

void 
tc_free(void *ptr)
{
    span_t span;
    freelist node;

    span = map_addr_to_span(ptr);

    if(span->objs_size > 0 && span->objs_size <= THRESHALLOC)
    {
        deallocate(ptr,span->objs_size);
    }

    else
    {
        return_span_to_pgcache(span);
    }
    
}


