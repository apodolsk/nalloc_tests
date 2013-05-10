/**
 * @file   nalloc.c
 * @author Alex Podolsky <apodolsk@andrew.cmu.edu>
 * @date   Sat Oct 27 19:16:19 2012
 * 
 * @brief A lockfree memory allocator.
 *
 * nalloc's main features are thread-local segregated list subheaps, a
 * lockfree global slab allocator to manage subheaps, and a lockfree
 * inter-thread memory transfer mechanism to return freed blocks to their
 * original subheap. Nah lock.
 *
 * Why are the stack reads safe:
 * - wayward_blocks. Only 1 thread pops. No one else can pop the top and then
 * force it to segfault.
 * - free_arenas. Safe simply because I never return to the system. Was going
 * to say it's because unmaps happen before insertion to stack, but it's still
 * possible to read top, then someone pops top, uses it, and unmaps it before
 * reinserting. Could honestly just do a simple array. 
 *
 */

#define MODULE NALLOC

#include <nalloc.h>
#include <list.h>
#include <stack.h>
#include <sys/mman.h>
#include <global.h>

#define MAX_ARENA_FAILS 10

static lfstack_t free_arenas = INITIALIZED_STACK;
__thread list_t private_arenas = INITIALIZED_LIST;
/* Warning: It happens to be the case that INITIALIZED_BLIST is a big fat
   NULL. */
__thread blist_t blists[NBLISTS];
__thread block_t dummy = { .free = 0, .size = MAX_BLOCK, .l_size = MAX_BLOCK };

void *nmalloc(size_t size){
    trace2(size, lu);
    used_block_t *found = alloc(umax(align_up_pow2(size + sizeof(used_block_t),
                                                   MIN_ALIGNMENT),
                                     MIN_BLOCK));
    return found ? found + 1 : NULL;
}
        
void nfree(void *buf){
    trace2(buf, p);
    dealloc((used_block_t *) buf - 1);
}

void block_init(block_t *b, size_t size, size_t l_size){
    b->size = size;
    b->free = 1;
    b->l_size = l_size;
    b->lanc = (lanchor_t) INITIALIZED_LANCHOR;
    assert(write_block_magics(b));
}

void free_arena_init(arena_t *a){
    trace4(a, p);
    *a = (arena_t) INITIALIZED_FREE_ARENA;
    block_init(a->heap, MAX_BLOCK, MAX_BLOCK);
    assert(aligned(((used_block_t*)&a->heap[0])->data, MIN_ALIGNMENT));
}

arena_t *arena_new(void){
    arena_t *fa = stack_pop_lookup(arena_t, sanc, &free_arenas);
    if(!fa){
        fa = mmap(NULL, ARENA_SIZE * ARENA_ALLOC_BATCH, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if(!fa)
            return NULL;
        /* Note that we start from 1. */
        for(int i = 1; i < ARENA_ALLOC_BATCH; i++){
            arena_t *extra = (void*) ((uptr_t) fa + i * ARENA_SIZE);
            free_arena_init(extra);
            stack_push(&extra->sanc, &free_arenas);
        }
        /* TODO This will result in wasteful magics writes, but that's
           dbg-only. */
        free_arena_init(fa);
    }
    
    return fa;
}

void arena_free(arena_t *freed){
    freed->sanc = (sanchor_t) INITIALIZED_SANCHOR;
    stack_push(&freed->sanc, &free_arenas);
}

used_block_t *alloc(size_t size){
    trace2(size, lu);
    assert(size <= MAX_BLOCK);
    assert(size >= MIN_BLOCK);

    used_block_t *found;

    for(blist_t *b = blist_larger_than(size); b < &blists[NBLISTS]; b++)
        if( (found = alloc_from_blist(size, b)) )
            goto done;

    absorb_all_wayward_blocks(&private_arenas);
    arena_t *a = arena_new();
    if(!a)
        return NULL;
    found = alloc_from_block(a->heap, size);
    
done:
    assert(found);
    rassert(found->size, >=, size);
    assert(aligned(found->data, MIN_ALIGNMENT));
    return found;
}

used_block_t *alloc_from_blist(size_t size, blist_t *bl){
    block_t *b = list_pop_lookup(block_t, lanc, &bl->blocks);
    if(!b)
        return NULL;
    return alloc_from_block(b, size);
}

used_block_t *alloc_from_block(block_t *b, size_t size){
    used_block_t *shaved = shave(b, size);
    shaved->free = 0;
    return shaved;
}

used_block_t *shave(block_t *b, size_t size){
    trace3(b,p,size,lu);

    used_block_t *newb;
    size_t shavesize = b->size - size;
    /* Induction: if b->data and b->size are MIN_ALIGNMENT aligned after arena
       init, and all future allocation sizes are always MIN_ALIGNMENT aligned,
       then newb->data and shavesize must be aligned too. */
    newb = (used_block_t *) ((uptr_t) b + shavesize);

    assert(aligned(shavesize, MIN_ALIGNMENT));

    if(shavesize){
        newb->size = b->size - shavesize;

        r_neighbor((block_t *) newb)->l_size = newb->size;
        newb->l_size = b->size = shavesize;

        log("Split - %p:%u | %p:%u", b, b->size, newb, newb->size);

        if(shavesize >= MIN_BLOCK)
            list_add_front(&b->lanc,
                           &blist_smaller_than(b->size)->blocks);
        else
            assert(is_junk_block(b));
    }
    
    return newb;
}

void dealloc(used_block_t *_b){
    trace2(_b, p);
    assert(used_block_valid(_b));
    block_t *b = (block_t *) _b;
    arena_t *arena = arena_of(b);
    if(arena->owner == pthread_self()){
        b = merge_adjacent(b);
        b->free = 1;
        assert(write_block_magics(b));
            
        if(b->size == MAX_BLOCK && private_arenas.size > IDEAL_FULL_ARENAS)
            arena_free(arena);
        else
            list_add_front(&b->lanc, &blist_smaller_than(b->size)->blocks);
    }
    else
        return_wayward_block((wayward_block_t*) b, arena);
}

void return_wayward_block(wayward_block_t *b, arena_t *arena){
    stack_push(&b->sanc, &arena->wayward_blocks);
}

void absorb_all_wayward_blocks(list_t *arenas){
    arena_t *cur_arena;
    FOR_EACH_LLOOKUP(cur_arena, arena_t, lanc, arenas)
        absorb_arena_blocks(cur_arena);
}

void absorb_arena_blocks(arena_t *arena){
    wayward_block_t *block;    
    FOR_EACH_SPOPALL_LOOKUP(block, wayward_block_t, sanc,
                            &arena->wayward_blocks)
        dealloc((used_block_t *) block);
}


void *merge_adjacent(block_t *b){
    trace3(b, p);
    int loops = 0;
    block_t *near;    
    while( ((near = r_neighbor(b)) && near->free) ||
           ((near = l_neighbor(b)) && near->free) ) 
    {
        if(near->size >= MIN_BLOCK)
            list_remove(&near->lanc, &blist_smaller_than(near->size)->blocks);

        if(near < b)
            log("Merge - %p:%u | %p:%u", near, near->size, b, b->size);
        else
            log("Merge - %p:%u | %p:%u", b, b->size, near, near->size);

        block_t *oldb = b;
        b = (block_t *) umin(b, near);
        b->size = oldb->size + near->size;

        /* At most 1 merge each for l and r with the current eager coalescing
           policy. */
        assert(loops++ < 2);
        assert(near < oldb || loops == 1);
    }

    r_neighbor(b)->l_size = b->size;
    return b;
}

blist_t *blist_larger_than(size_t size){
    trace4(size, lu);
    assert(size <= MAX_BLOCK);
    for(int i = 0; i < NBLISTS; i++)
        if(blist_size_lookup[i] >= size)
            return &blists[i];
    return &blists[NBLISTS];
}

blist_t *blist_smaller_than(size_t size){
    trace4(size, lu);
    assert(size >= MIN_BLOCK);
    for(int i = NBLISTS - 1; i >= 0; i--)
        if(blist_size_lookup[i] <= size)
            return &blists[i];
    LOGIC_ERROR();
    return NULL;
}

block_t *l_neighbor(block_t *b){
    if((uptr_t) b == (uptr_t) arena_of(b)->heap)
        return NULL;
    return (block_t *) ((uptr_t) b - b->l_size);
}

block_t *r_neighbor(block_t *b){
    arena_t *a = arena_of(b);
    if((uptr_t) b + b->size == (uptr_t) a + MAX_BLOCK)
        return NULL;
    return (block_t *) ((uptr_t) b + b->size);
}

arena_t *arena_of(block_t *b){
    return (arena_t *) align_down_pow2(b, PAGE_SIZE);
}

int is_junk_block(block_t *b){
    return b->size < MIN_BLOCK;
}

int free_block_valid(block_t *b){
    assert(block_magics_valid(b));
    assert(b->size >= MIN_BLOCK);
    assert(b->size <= MAX_BLOCK);
    assert(!l_neighbor(b) || l_neighbor(b)->size == b->l_size);
    assert(!r_neighbor(b) || r_neighbor(b)->l_size == b->size);
    assert(lanchor_valid(&b->lanc,
                         &blist_smaller_than(b->size)->blocks));
    assert(dummy.size == MAX_BLOCK);
    
    return 1;
}

int used_block_valid(used_block_t *_b){
    block_t *b = (block_t *) _b;
    assert(b->size >= MIN_BLOCK);
    assert(b->size <= MAX_BLOCK);
    assert(aligned(_b->data, MIN_ALIGNMENT));
    assert(!l_neighbor(b) || l_neighbor(b)->size == b->l_size);
    assert(!r_neighbor(b) || r_neighbor(b)->l_size == b->size);
    assert(dummy.size == MAX_BLOCK);

    return 1;
}

int write_block_magics(block_t *b){
    if(!HEAP_DBG)
        return 1;
    for(int i = 0; i < (b->size - sizeof(*b)) / sizeof(*b->magics); i++)
        b->magics[i] = NALLOC_MAGIC_INT;
    return 1;
}

int block_magics_valid(block_t *b){
    if(!HEAP_DBG)
        return 1;
    for(int i = 0; i < (b->size - sizeof(*b)) / sizeof(*b->magics); i++)
        rassert(b->magics[i], ==, NALLOC_MAGIC_INT);
    return 1;
}

int free_arena_valid(arena_t *a){
    /* assert(sanchor_valid(&a->sanc)); */
    return 1;
}

int used_arena_valid(arena_t *a){
    return 1;
}
