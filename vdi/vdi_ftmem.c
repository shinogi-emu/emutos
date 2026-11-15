/*
 * vdi_ftmem.c - the memory allocator of the outline font engine
 *
 * Copyright (C) 2026 The EmuTOS development team
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/*
 * FreeType makes many small allocations: an open face takes about 65
 * blocks.  Given a GEMDOS block each, they use up EmuTOS's pool of memory
 * descriptors after four or five faces, and then no program can allocate
 * memory any more.  So small blocks are carved from arenas of ARENA_SIZE
 * bytes, each one block from the system; requests above LARGE_SIZE (font
 * files, big bitmaps) get a block of their own.
 *
 * An arena is a run of chunks, ended by a sentinel header of size 0 that
 * is always in use.  Each chunk starts with an 8-byte header holding its
 * size, with two flags in the low bits, and the size of the chunk before
 * it (0 for the first).  Free chunks are in one list, searched first fit,
 * and a freed chunk is merged with its free neighbours at once.  So an
 * arena whose blocks are all free is one chunk, with no chunk before it
 * and the sentinel after it.  One empty arena is kept for the next
 * requests; another that empties is given back to the system.
 *
 * When the system has no arena to give, a small request gets a block of
 * its own, like a large one.
 *
 * This file is compiled with 16-bit int, so every size is a ULONG.  It is
 * not reentrant, and need not be: ftmem_os_alloc() and ftmem_os_free() can
 * sleep (see vdi_sleep.h), but every caller is on the font stack of
 * vdi_outline.c, which one call uses at a time.
 */

#include "portab.h"
#include "vdi_ftmem.h"

#define ALIGN           8UL         /* of every chunk, so of every block */
#define HEADER          8UL         /* the chunk header before a block */
#define MIN_CHUNK       16UL        /* a header and the free list links */
#define ARENA_SIZE      65536UL     /* taken from the system at a time */
#define LARGE_SIZE      16384UL     /* larger requests get a block of their own */
#define MAX_REQUEST     0x7fff0000UL

/* low bits of Chunk.size */
#define F_USED          1UL
#define F_OWN           2UL         /* a block of its own from the system */
#define SIZE_MASK       (~(ALIGN - 1))

typedef struct chunk
{
    ULONG size;                 /* of the chunk, header included, and flags */
    ULONG prev_size;            /* of the chunk before it, 0 for the first */
    /* in free chunks only */
    struct chunk *next_free;
    struct chunk *prev_free;
} Chunk;

/* the header of a block of its own, as big as a chunk header */
typedef struct
{
    ULONG size;                 /* what the block holds, and flags */
    void *os_block;             /* as the system gave it */
} OwnHeader;

/* the start of an arena, before its first chunk */
typedef struct
{
    void *os_block;             /* as the system gave it */
} Arena;

#define ARENA_HEADER    16UL    /* room for an Arena, a multiple of ALIGN */

#define ROUND_UP(x)     (((x) + ALIGN - 1) & SIZE_MASK)
#define CHUNK_SIZE(c)   ((c)->size & SIZE_MASK)
#define NEXT_CHUNK(c)   ((Chunk *)((UBYTE *)(c) + CHUNK_SIZE(c)))
#define BLOCK_OF(c)     ((void *)((UBYTE *)(c) + HEADER))
#define CHUNK_OF(b)     ((Chunk *)((UBYTE *)(b) - HEADER))

static Chunk *free_list;
static LONG empty_arenas;


static UBYTE *align_up(void *p)
{
    return (UBYTE *)ROUND_UP((ULONG)p);
}

/*
 * copy between two blocks: both are aligned to ALIGN, and hold a multiple
 * of ALIGN bytes, so whole longs can be copied
 */
static void copy_block(void *to, const void *from, ULONG bytes)
{
    ULONG *t = to;
    const ULONG *f = from;
    ULONG n;

    for (n = ROUND_UP(bytes) / sizeof(ULONG); n > 0; n--)
        *t++ = *f++;
}

static void link_free(Chunk *c)
{
    c->prev_free = NULL;
    c->next_free = free_list;
    if (free_list)
        free_list->prev_free = c;
    free_list = c;
}

static void unlink_free(Chunk *c)
{
    if (c->prev_free)
        c->prev_free->next_free = c->next_free;
    else
        free_list = c->next_free;
    if (c->next_free)
        c->next_free->prev_free = c->prev_free;
}

/* is this free chunk all of its arena? */
static BOOL whole_arena(Chunk *c)
{
    return (c->prev_size == 0) && (NEXT_CHUNK(c)->size == F_USED);
}

/* take an arena from the system: it is one free chunk */
static BOOL new_arena(void)
{
    UBYTE *os, *start, *end;
    Chunk *c, *sentinel;

    os = ftmem_os_alloc(ARENA_SIZE);
    if (!os)
        return FALSE;

    start = align_up(os);
    ((Arena *)start)->os_block = os;
    c = (Chunk *)(start + ARENA_HEADER);
    end = (UBYTE *)((((ULONG)os) + ARENA_SIZE) & SIZE_MASK);
    sentinel = (Chunk *)(end - HEADER);

    c->size = (ULONG)sentinel - (ULONG)c;
    c->prev_size = 0;
    sentinel->size = F_USED;
    sentinel->prev_size = c->size;
    link_free(c);
    empty_arenas++;

    return TRUE;
}

/* a block of its own from the system */
static void *own_block(ULONG size)
{
    UBYTE *os;
    OwnHeader *h;
    ULONG room = ROUND_UP(size);

    os = ftmem_os_alloc(room + HEADER + ALIGN - 1);
    if (!os)
        return NULL;

    h = (OwnHeader *)align_up(os);
    h->size = room | F_OWN | F_USED;
    h->os_block = os;

    return (UBYTE *)h + HEADER;
}

static Chunk *find_free(ULONG need)
{
    Chunk *c;

    for (c = free_list; c; c = c->next_free)
        if (c->size >= need)
            return c;

    return NULL;
}

/* use a free chunk for need bytes; what is left over becomes a free chunk */
static void use_chunk(Chunk *c, ULONG need)
{
    ULONG size = c->size;

    unlink_free(c);
    if (whole_arena(c))
        empty_arenas--;

    if (size - need >= MIN_CHUNK)
    {
        Chunk *rest = (Chunk *)((UBYTE *)c + need);

        rest->size = size - need;
        rest->prev_size = need;
        NEXT_CHUNK(rest)->prev_size = rest->size;
        link_free(rest);
        size = need;
    }

    c->size = size | F_USED;
}


void *ftmem_alloc(ULONG size)
{
    ULONG need;
    Chunk *c;

    if (size > MAX_REQUEST)
        return NULL;
    if (size > LARGE_SIZE)
        return own_block(size);

    need = ROUND_UP(size + HEADER);
    if (need < MIN_CHUNK)
        need = MIN_CHUNK;

    c = find_free(need);
    if (!c && new_arena())
        c = find_free(need);
    if (!c)
        return own_block(size);     /* no arena to be had */

    use_chunk(c, need);

    return BLOCK_OF(c);
}


void ftmem_free(void *block)
{
    Chunk *c, *next, *prev;
    ULONG size;

    if (!block)
        return;

    c = CHUNK_OF(block);
    if (!(c->size & F_USED))
        return;                     /* freed already */
    if (c->size & F_OWN)
    {
        ftmem_os_free(((OwnHeader *)c)->os_block);
        return;
    }

    size = CHUNK_SIZE(c);
    c->size = size;

    /* merge with the chunk after it, then with the one before it */
    next = NEXT_CHUNK(c);
    if (!(next->size & F_USED))
    {
        unlink_free(next);
        size += next->size;
        c->size = size;
    }
    if (c->prev_size)
    {
        prev = (Chunk *)((UBYTE *)c - c->prev_size);
        if (!(prev->size & F_USED))
        {
            unlink_free(prev);
            size += prev->size;
            prev->size = size;
            c = prev;
        }
    }
    NEXT_CHUNK(c)->prev_size = size;

    if (whole_arena(c))
    {
        /* one empty arena is kept for what comes next */
        if (empty_arenas > 0)
        {
            ftmem_os_free(((Arena *)((UBYTE *)c - ARENA_HEADER))->os_block);
            return;
        }
        empty_arenas++;
    }

    link_free(c);
}


void *ftmem_realloc(void *block, ULONG size)
{
    Chunk *c;
    ULONG room;
    void *newblock;

    if (!block)
        return ftmem_alloc(size);
    if (size == 0)
    {
        ftmem_free(block);
        return NULL;
    }

    c = CHUNK_OF(block);
    room = (c->size & F_OWN) ? CHUNK_SIZE(c) : CHUNK_SIZE(c) - HEADER;

    /* a block that holds the new size without wasting half stays */
    if (size <= room && (size >= room / 2 || room <= MIN_CHUNK))
        return block;

    newblock = ftmem_alloc(size);
    if (!newblock)
        return NULL;                /* the old block is left alone */

    copy_block(newblock, block, (size < room) ? size : room);
    ftmem_free(block);

    return newblock;
}
