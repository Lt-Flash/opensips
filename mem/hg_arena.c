/*
 * hugepage-backed slab allocator - arena internals
 *
 * Copyright (C) 2026 Yury Kirsanov
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HG_MALLOC

#include <string.h>
#include <stdlib.h>

#include "hg_malloc.h"
#include "hg_arena.h"
#include "hg_large.h"
#include "../dprint.h"
#include "../globals.h"

/* ~x1.5 ladder, all multiples of 32 so cells stay HG_ROUNDTO-aligned; ported
 * unchanged from cachedb_perf's pcache_arena.c cell_sizes[] - these are
 * TOTAL slot sizes (header + payload), same convention as there */
static const unsigned int cell_sizes[HG_NCLASSES] = {
	64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048,
	3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536
};

#define HG_CHUNK_SMALL    (256 * 1024)  /* cells <= 8K share 256K chunks */
#define HG_CHUNK_MIN      (8 * 1024)    /* floor, however tiny the arena */
#define HG_REFILL_BATCH   32             /* cells pulled from the global pool */
#define HG_PRIVATE_MAX    256            /* private stack size that triggers */
#define HG_DONATE         128            /*   donation of this many cells    */

/*
 * Per-THREAD private free-stack state, one slot per hg_block instance live
 * in this thread (see the hg_arena.h comment on why this can't be the
 * single static global cachedb_perf uses: HG_MALLOC can back shm, shm_dbg
 * AND pkg simultaneously). A fixed-size static array, not heap-allocated -
 * this state is exactly the kind of bootstrap-before-any-allocator-exists
 * bookkeeping that must NOT go through pkg_malloc()/shm_malloc(), since
 * HG_MALLOC may itself be backing one or both of those.
 *
 * __thread, NOT merely static, and that is a correctness requirement rather
 * than a tuning choice. The whole fast path is lock-free precisely because
 * this state is private to its owner; a plain static makes it private to the
 * PROCESS, which was true of OpenSIPS's classic one-thread-per-process model
 * but is NOT true since 4.1 - TCP main now runs a pthread IO pool
 * (tcp_pool_init(), net_tcp.c, one thread per CPU by default, started
 * unconditionally), and those threads run the read callbacks that reach
 * tcp_dispatch_msg() -> shm_malloc(). Several threads therefore hit these
 * lists at once.
 *
 * Observed on real traffic before this was made per-thread:
 *   - lost "nfree--" updates, so the counter UNDERFLOWED and wrapped
 *     (logged: "nfree claimed 4294966733", i.e. 2^32-563), which trivially
 *     exceeds HG_PRIVATE_MAX and drove the donation loop off the end of a
 *     chain that had far fewer cells than claimed;
 *   - two threads popping the SAME cell, handing one block to two callers -
 *     a tcp_ipc_payload struct came back with SIP text where its conn
 *     pointer belonged, and tcpconn_put() died on it.
 * Both faults were the same race wearing different masks. si_addr on the
 * first is 0x20 == HG_CELL_HDR, i.e. cell_next(NULL).
 *
 * Cost: sizeof(struct hg_palloc) is ~512B, so ~2KB of TLS per thread for all
 * four instances - trivial next to what it buys, and the fast path stays
 * lock-free rather than gaining a mutex.
 *
 * Note what stays process-wide on purpose: hb->gpool (the shared pool, taken
 * under hb->lock) is unchanged, so cells still circulate between threads and
 * processes normally. A thread that exits leaves its cached cells parked in
 * its own dead TLS rather than returning them - harmless here, because the
 * IO pool threads live for the lifetime of the process.
 */
#define HG_MAX_INSTANCES 4
static __thread struct hg_palloc palloc_slots[HG_MAX_INSTANCES];

/*
 * One-entry lookup cache for the slot this thread used last.
 *
 * Every alloc and every free begins by resolving hb -> palloc, so this is
 * the single hottest lookup in the allocator, and it was a linear walk of
 * HG_MAX_INSTANCES slots comparing owner pointers - on EVERY call, since
 * before this the only way in was the slow path below. In practice a worker
 * bounces between two instances at most (shm and pkg), and overwhelmingly
 * hits the same one many times in a row, so the walk almost always finds its
 * answer after re-testing slots it just rejected.
 *
 * Splitting it lets the fast path inline into hg_cell_alloc()/hg_cell_free()
 * as a TLS load and one compare, while the cold path stays out of line. That
 * matters beyond the instruction count: the slow path carries a stack-
 * protector prologue (GCC adds one once __thread and the memset() are in
 * play), and an inlined fast path skips the call and the canary entirely
 * rather than paying them per allocation.
 *
 * Staleness is safe by construction: the cache is only trusted when
 * pl->owner still equals the requested block. hg_arena_child_init() and
 * hg_arena_destroy() zero a slot's owner, so a cached pointer to a recycled
 * slot simply fails that test and falls through to the walk - but both also
 * clear hg_last outright, so the invalidation is explicit rather than
 * relying on that.
 */
static __thread struct hg_palloc *hg_last;

static struct hg_palloc *hg_palloc_lookup(struct hg_block *hb)
{
	int i, free_slot = -1;

	for (i = 0; i < HG_MAX_INSTANCES; i++) {
		if (palloc_slots[i].owner == hb) {
			hg_last = &palloc_slots[i];
			return &palloc_slots[i];
		}
		if (free_slot < 0 && !palloc_slots[i].owner)
			free_slot = i;
	}

	if (free_slot < 0) {
		LM_CRIT("more than %d live HG_MALLOC instances in one process "
			"- raise HG_MAX_INSTANCES in hg_arena.c\n", HG_MAX_INSTANCES);
		return NULL;
	}

	memset(&palloc_slots[free_slot], 0, sizeof(struct hg_palloc));
	palloc_slots[free_slot].owner = hb;
	hg_last = &palloc_slots[free_slot];
	return &palloc_slots[free_slot];
}

static inline struct hg_palloc *hg_get_palloc(struct hg_block *hb)
{
	struct hg_palloc *pl = hg_last;

	if (pl && pl->owner == hb)
		return pl;
	return hg_palloc_lookup(hb);
}

/*
 * Free-list link storage: a SINGLE convention, used everywhere (private
 * free-stack and the shared global pool alike) - both helpers take
 * @cell_start (the address of the hidden header / class byte), and write
 * the link at cell_start + HG_CELL_HDR, i.e. inside the PAYLOAD area, never
 * inside the header itself. This is safe because nobody reads the payload
 * of a free cell, and it keeps the class byte at offset 0 untouched no
 * matter which free list (private or shared) currently owns the cell.
 */
static inline void *cell_next(void *cell_start)
{
	return *(void **)((char *)cell_start + HG_CELL_HDR);
}

static inline void cell_set_next(void *cell_start, void *next)
{
	*(void **)((char *)cell_start + HG_CELL_HDR) = next;
}

/*
 * Does @cell_start belong to THIS arena?
 *
 * Everything hg hands out - small cells in chunks and large frags alike -
 * is carved from the single hb->hbase reservation, whose bounds are fixed
 * at init. Deliberately NOT hb->lo/hb->hi: those widen as chunks are
 * carved, so a lock-free reader can see a stale, too-narrow range and
 * reject a perfectly good cell.
 *
 * Why this matters: a process routinely hosts THREE live hg instances (shm,
 * pkg, and cachedb_perf's own arena), all using an identical cell layout.
 * A pointer from one of the others carries a class byte that looks entirely
 * valid here (0..HG_NCLASSES-1), so without this check it would sail past
 * every test below and land on THIS arena's private free list. Its real
 * owner then reuses the cell and overwrites the payload - which is exactly
 * where cell_set_next() keeps the free-list link - truncating our chain
 * while nfree keeps counting the cells that were on it. That drift is what
 * later drove the donation loop off the end of the chain and into
 * cell_next(NULL). Refusing the free leaks one cell; accepting it corrupts
 * the pool.
 */
/* global pool ops - hb->lock must be held. Both take/return cell_start. */
static inline void gpool_push(struct hg_block *hb, int c, void *cell_start)
{
	cell_set_next(cell_start, hb->gpool[c]);
	hb->gpool[c] = cell_start;
	hb->gpool_n[c]++;
}

static inline void *gpool_pop(struct hg_block *hb, int c)
{
	void *cell_start = hb->gpool[c];

	if (cell_start) {
		hb->gpool[c] = cell_next(cell_start);
		hb->gpool_n[c]--;
	}
	return cell_start;
}

/*
 * Every byte of arena memory funnels through here: bump the atomic offset
 * within the block's own huge-page reservation. No fallback to another
 * allocator on exhaustion (unlike cachedb_perf's shm_malloc() fallback) -
 * per the HG_MALLOC design decision, exhaustion is a hard, loud failure;
 * the operator increases -m/-M instead of silently landing on 4K pages
 * mid-run through a different allocator's pool.
 */
void *hg_chunk_backing(struct hg_block *hb, unsigned long size)
{
	unsigned long asz = (size + 63) & ~63UL;   /* keep 64-aligned */
	unsigned long off = __atomic_fetch_add(&hb->hoff, asz, __ATOMIC_RELAXED);

	if (off + asz <= hb->hsize)
		return hb->hbase + off;

	/* exhausted: undo would race other bumpers, so just leave hoff past
	 * the end (further allocs also fail) - correctness holds, we only
	 * lose the tail slack */
	return NULL;
}

/*
 * Chunk granularity has to scale with the arena, not be a fixed 256K.
 *
 * A chunk is claimed whole the first time its size class is touched, so
 * with a fixed size the 21 classes cost 21 * 256K = 5.25M of granularity
 * before a single useful byte is served - which an 8M pkg arena cannot
 * afford, and it then fails to parse SIP messages at all. (F_MALLOC has no
 * equivalent floor because it splits fragments to fit.) Cap a chunk at a
 * small fraction of the arena so a small arena gets proportionally small
 * chunks, while a large one keeps the full 256K and its amortisation.
 */
static inline unsigned int chunk_size_for(struct hg_block *hb, int c)
{
	unsigned int want = cell_sizes[c] <= 8192 ?
		HG_CHUNK_SMALL : cell_sizes[c] * 32;
	unsigned int least = sizeof(struct hg_chunk) + cell_sizes[c] * 2;

	if (want > hb->chunk_max)
		want = hb->chunk_max;
	/* ...but always enough for the header plus a couple of cells, or the
	 * class could never be served at all */
	if (want < least)
		want = least;
	return want;
}

/* carve a new chunk for class @c - hb->lock must be held. The class byte of
 * every cell's hidden header is stamped HERE, before the chunk is reachable
 * by anyone - immutable from birth. */
static int carve_chunk(struct hg_block *hb, int c, struct hg_palloc *pl)
{
	struct hg_chunk *ch;
	unsigned int size = chunk_size_for(hb, c), i;
	char *cells;

	ch = hg_chunk_backing(hb, size);
	if (!ch) {
		LM_ERR("%s: no more HG_MALLOC arena memory for a %u byte chunk "
			"(class %d) - increase the arena size\n", hb->name, size, c);
		return -1;
	}

	ch->cls = c;
	ch->cell_size = cell_sizes[c];
	ch->cells = (size - sizeof(struct hg_chunk)) / cell_sizes[c];

	cells = (char *)ch + sizeof(struct hg_chunk);
	for (i = 0; i < ch->cells; i++)
		cells[(unsigned long)i * cell_sizes[c]] = (unsigned char)c;

	ch->next = hb->chunks;
	hb->chunks = ch;
	hb->nchunks++;
	hb->real_used += size;
	if (hb->real_used > hb->max_real_used)
		hb->max_real_used = hb->real_used;

	if ((unsigned long)ch < hb->lo)
		hb->lo = (unsigned long)ch;
	if ((unsigned long)ch + size > hb->hi)
		hb->hi = (unsigned long)ch + size;

	/* the whole chunk belongs to the carving process */
	pl->cls[c].bump = cells;
	pl->cls[c].left = ch->cells;

	LM_DBG("%s class %d: new %u byte chunk, %u cells of %u\n",
		hb->name, c, size, ch->cells, cell_sizes[c]);
	return 0;
}

int hg_arena_init(struct hg_block *hb, unsigned long hdr_size)
{
	unsigned int idx, c, needed;

	/* leave the block header itself untouched by the bump allocator */
	hb->hoff = (hdr_size + 63) & ~63UL;

	/* see chunk_size_for(): no single chunk may swallow a big slice of a
	 * small arena. /64 keeps all 21 classes plus the large tier inside a
	 * third of the arena even in the worst case. */
	hb->chunk_max = hb->size / 64;
	if (hb->chunk_max > HG_CHUNK_SMALL)
		hb->chunk_max = HG_CHUNK_SMALL;
	if (hb->chunk_max < HG_CHUNK_MIN)
		hb->chunk_max = HG_CHUNK_MIN;

	/* size -> class LUT: needed = requested payload + hidden header */
	for (idx = 0; idx <= HG_CELL_MAX / HG_ROUNDTO; idx++) {
		needed = idx * HG_ROUNDTO + HG_CELL_HDR;
		for (c = 0; c < HG_NCLASSES; c++)
			if (cell_sizes[c] >= needed)
				break;
		hb->size2class[idx] = (unsigned char)c;   /* HG_NCLASSES = oversize */
	}

	LM_DBG("%s arena ready: %d classes, %u B to %u B cells (header=%zu B)\n",
		hb->name, HG_NCLASSES, cell_sizes[0], cell_sizes[HG_NCLASSES-1],
		HG_CELL_HDR);
	return 0;
}

void *hg_region_alloc(struct hg_block *hb, unsigned long size)
{
	struct hg_region *rg;
	unsigned long need = size + sizeof(struct hg_region) + 64;
	char *aligned;

	rg = hg_chunk_backing(hb, need);
	if (!rg) {
		LM_ERR("%s: no more HG_MALLOC arena memory for a %lu byte "
			"region\n", hb->name, need);
		return NULL;
	}
	rg->size = need;
	aligned = (char *)(((unsigned long)rg + sizeof(struct hg_region) + 63)
	                   & ~63UL);

	lock_get(&hb->lock);
	rg->next = hb->regions;
	hb->regions = rg;
	hb->real_used += need;
	if (hb->real_used > hb->max_real_used)
		hb->max_real_used = hb->real_used;
	if ((unsigned long)rg < hb->lo)
		hb->lo = (unsigned long)rg;
	if ((unsigned long)rg + need > hb->hi)
		hb->hi = (unsigned long)rg + need;
	lock_release(&hb->lock);

	return aligned;
}

void hg_arena_destroy(struct hg_block *hb)
{
	int i;

	/* every chunk/region is carved from ONE mmap (hb->hbase) - munmap'd
	 * as a whole by hg_malloc_destroy() in hg_malloc.c, never individually
	 * freed here. Just drop the per-process palloc slot, if any. */
	for (i = 0; i < HG_MAX_INSTANCES; i++)
		if (palloc_slots[i].owner == hb)
			memset(&palloc_slots[i], 0, sizeof(struct hg_palloc));
	hg_last = NULL;

	hg_large_destroy(hb);
}

void hg_arena_child_init(struct hg_block *hb)
{
	int i;

	/*
	 * After fork every child holds a COW copy of the parent's private
	 * allocator state for @hb - the SAME bump pointer and the SAME
	 * free-list cell addresses. A child must not keep them (two processes
	 * bumping one chunk would hand out the same cell) and must NOT donate
	 * them to the global pool either: every child inherited the identical
	 * copy, so each would push the same physical cells onto the shared
	 * free list, corrupting it (a cell landing on the list N times, later
	 * popped and written by several processes at once).
	 *
	 * Ported reasoning from cachedb_perf's pcache_arena_child_init(),
	 * which hit exactly this as the CP-16 corruption bug. The leftover
	 * cells belong to the parent; the child discards its inherited copy
	 * and starts empty, carving its own chunk on first use.
	 *
	 * Since palloc_slots became __thread this clears only the CALLING
	 * thread's slots, which is exactly right: fork() clones just the
	 * calling thread, so the child starts life single-threaded and there
	 * are no other slots in it to clear. Any IO threads it later spawns
	 * get freshly-zeroed TLS of their own.
	 */
	for (i = 0; i < HG_MAX_INSTANCES; i++)
		if (palloc_slots[i].owner == hb)
			memset(&palloc_slots[i], 0, sizeof(struct hg_palloc));
	hg_last = NULL;
}

unsigned int hg_cell_total_size(unsigned char cls)
{
	if (cls >= HG_NCLASSES)
		return 0;
	return cell_sizes[cls];
}

#ifdef DBG_MALLOC
void *hg_cell_alloc(struct hg_block *hb, unsigned long size,
                    const char *file, const char *func, unsigned int line)
#else
void *hg_cell_alloc(struct hg_block *hb, unsigned long size)
#endif
{
	struct hg_palloc *pl;
	char *cell_start, *payload;
	unsigned int got;
	int c;

	if (size + HG_CELL_HDR > HG_CELL_MAX) {
#ifdef DBG_MALLOC
		return hg_large_alloc(hb, size, file, func, line);
#else
		return hg_large_alloc(hb, size);
#endif
	}
	c = hb->size2class[(size + HG_ROUNDTO - 1) / HG_ROUNDTO];
	if (c >= HG_NCLASSES)
		return NULL;

	pl = hg_get_palloc(hb);
	if (!pl)
		return NULL;

	/* fast paths: no locks, no shared lines */
	cell_start = pl->cls[c].free_head;
	if (cell_start) {
		pl->cls[c].free_head = cell_next(cell_start);
		pl->cls[c].nfree--;
		goto found;
	}
	if (pl->cls[c].left) {
		cell_start = pl->cls[c].bump;
		pl->cls[c].bump += cell_sizes[c];
		pl->cls[c].left--;
		goto found;
	}

	/* slow path: refill from the global pool, else carve a chunk */
	lock_get(&hb->lock);
	for (got = 0; got < HG_REFILL_BATCH; got++) {
		cell_start = gpool_pop(hb, c);
		if (!cell_start)
			break;
		cell_set_next(cell_start, pl->cls[c].free_head);
		pl->cls[c].free_head = cell_start;
		pl->cls[c].nfree++;
	}
	if (!got && carve_chunk(hb, c, pl) < 0) {
		lock_release(&hb->lock);
		return NULL;
	}
	lock_release(&hb->lock);

	cell_start = pl->cls[c].free_head;
	if (cell_start) {
		pl->cls[c].free_head = cell_next(cell_start);
		pl->cls[c].nfree--;
		goto found;
	}
	cell_start = pl->cls[c].bump;
	pl->cls[c].bump += cell_sizes[c];
	pl->cls[c].left--;

found:
	payload = cell_start + HG_CELL_HDR;
#ifdef DBG_MALLOC
	{
		const char **hfile = (const char **)(cell_start + HG_ROUNDTO);
		const char **hfunc = (const char **)(cell_start + HG_ROUNDTO * 2);
		unsigned long *hline = (unsigned long *)(cell_start + HG_ROUNDTO * 3);
		*hfile = file;
		*hfunc = func;
		*hline = line;
	}
#endif
	/* per-process slot: no lock, no shared cache line (see hg_pstat) */
	{
		struct hg_pstat *ps = hg_pstat_mine(hb);
		ps->used += cell_sizes[c] - HG_CELL_HDR;
		ps->fragments++;
		ps->cell_live += cell_sizes[c];
	}
	return payload;
}

#ifdef DBG_MALLOC
void hg_cell_free(struct hg_block *hb, void *p, const char *file,
                  const char *func, unsigned int line)
#else
void hg_cell_free(struct hg_block *hb, void *p)
#endif
{
	struct hg_palloc *pl;
	char *cell_start;
	unsigned int c, i;
	void *d;

	if (!p)
		return;

	cell_start = HG_HDR(p);

	/* before the class byte is even read: an out-of-arena pointer may
	 * not be mapped at all, and reading it would fault here rather
	 * than merely corrupt a pool. See hg_owns(). */
	if (!hg_owns(hb, cell_start)) {
		LM_CRIT("%s: %p does not belong to this arena [%p,%p) - "
			"refusing to free it\n", hb->name, p, hb->hbase,
			hb->hbase + hb->hsize);
		return;
	}

	c = *(unsigned char *)cell_start;

	if (c == HG_LARGE_MARKER) {
		hg_large_free(hb, (struct hg_lfrag *)(void *)(cell_start - HG_LFRAG_HDR));
		return;
	}

	if (c >= HG_NCLASSES) {
		/* the class byte was clobbered - freeing through it would
		 * corrupt the pools; leak the cell and shout instead. This is
		 * the only corruption check Phase 1 has - true double-free
		 * detection (has THIS pointer already been freed) needs a
		 * per-cell allocated/free bit that isn't implemented yet, see
		 * the file header note on Phase 1 scope. */
		LM_CRIT("%s: cell %p carries invalid class %u - leaking it\n",
			hb->name, p, c);
		return;
	}

	pl = hg_get_palloc(hb);
	if (!pl) {
		hg_cell_free_global(hb, p);
		return;
	}

	{
		struct hg_pstat *ps = hg_pstat_mine(hb);
		ps->used -= cell_sizes[c] - HG_CELL_HDR;
		ps->fragments--;
		ps->cell_live -= cell_sizes[c];
	}

	cell_set_next(cell_start, pl->cls[c].free_head);
	pl->cls[c].free_head = cell_start;
	pl->cls[c].nfree++;

	/* keep hoarding bounded: donate half once over the threshold */
	if (pl->cls[c].nfree > HG_PRIVATE_MAX) {
		unsigned int claimed = pl->cls[c].nfree;

		lock_get(&hb->lock);
		for (i = 0; i < HG_DONATE; i++) {
			d = pl->cls[c].free_head;
			/* HG_DONATE (128) < HG_PRIVATE_MAX (256), so a chain
			 * whose length matches nfree can always satisfy this
			 * loop. Reaching a NULL here therefore means nfree has
			 * drifted ABOVE the number of cells actually on the
			 * chain, and walking on would evaluate cell_next(NULL)
			 * - the production SIGSEGV this guard replaces (a TCP
			 * worker died with cls[5] = {free_head = NULL,
			 * nfree = 257} and si_addr = HG_CELL_HDR). */
			if (!d)
				break;
			pl->cls[c].free_head = cell_next(d);
			pl->cls[c].nfree--;
			gpool_push(hb, c, d);
		}
		/* The chain ran dry, so the truth is zero. Resyncing is not
		 * cosmetic: leaving nfree high would re-enter this block on
		 * the very next free of this class and trip the guard again
		 * on every single call, burying the log. */
		if (i < HG_DONATE)
			pl->cls[c].nfree = 0;
		lock_release(&hb->lock);

		if (i < HG_DONATE)
			LM_CRIT("%s: class %u free list ran dry after %u cells "
				"but nfree claimed %u - counter resynced to 0, "
				"pool accounting drifted\n",
				hb->name, c, i, claimed);
	}
}

void hg_cell_free_global(struct hg_block *hb, void *p)
{
	char *cell_start;
	unsigned int c;

	if (!p)
		return;

	cell_start = HG_HDR(p);
	if (!hg_owns(hb, cell_start)) {
		LM_CRIT("%s: %p does not belong to this arena [%p,%p) - "
			"refusing to free it\n", hb->name, p, hb->hbase,
			hb->hbase + hb->hsize);
		return;
	}
	c = *(unsigned char *)cell_start;
	if (c == HG_LARGE_MARKER) {
		hg_large_free(hb, (struct hg_lfrag *)(void *)(cell_start - HG_LFRAG_HDR));
		return;
	}
	if (c >= HG_NCLASSES) {
		LM_CRIT("%s: cell %p carries invalid class %u - leaking it\n",
			hb->name, p, c);
		return;
	}
	/* hg_cell_free() delegates here WITHOUT having touched the counters
	 * (it returns early when this process has no palloc), and this is that
	 * function's only caller - so the decrement belongs here, not there.
	 * Before this, a free taken down this path left used/fragments
	 * permanently over-reported. */
	{
		struct hg_pstat *ps = hg_pstat_mine(hb);
		ps->used -= cell_sizes[c] - HG_CELL_HDR;
		ps->fragments--;
		ps->cell_live -= cell_sizes[c];
	}

	lock_get(&hb->lock);
	gpool_push(hb, c, cell_start);
	lock_release(&hb->lock);
}

/*
 * Cell-slot bytes that are carved into chunks but NOT currently handed out:
 * cells on a private free stack, cells in the global pool, and cells never
 * bumped at all. This capacity is fully reusable.
 *
 * It exists because HG_MALLOC never un-carves a chunk, so hb->real_used (the
 * arena's own footprint) can only ever rise. q_malloc/f_malloc report a freed
 * fragment sitting on a free list as FREE; without subtracting this figure,
 * HG_MALLOC would report the identical state as permanently USED, and
 * real_used would read as an unbounded leak on any monitoring dashboard.
 *
 * Walks the chunk registry WITHOUT hb->lock, deliberately: the list is
 * append-only, ch->cells/ch->cell_size are immutable once carved, and the
 * head pointer is published after the chunk is fully built, so a reader
 * either sees a complete chunk or does not see it at all. Taking the lock
 * here would nest it under whatever the stats caller already holds, for a
 * figure that is a sample either way.
 */
unsigned long hg_slab_recycled(struct hg_block *hb)
{
	struct hg_chunk *ch;
	unsigned long capacity = 0, live;

	for (ch = hb->chunks; ch; ch = ch->next)
		capacity += (unsigned long)ch->cells * ch->cell_size;

	live = hg_cell_live(hb);
	return capacity > live ? capacity - live : 0;
}

void hg_arena_stats(struct hg_block *hb, unsigned int *nchunks,
		unsigned long *bytes)
{
	lock_get(&hb->lock);
	*nchunks = hb->nchunks;
	*bytes = hb->real_used;
	lock_release(&hb->lock);
}

/*
 * Live-cell enumeration (mem-group stats_core_init, hg_status_dbg's dump).
 *
 * Small cells (unlike large frags' f->prev) carry no per-cell free/live
 * flag - a cell's free-ness is only implicit via free-list membership. So
 * "walk every live cell" first collects every currently-free address into
 * a plain glibc-malloc'd hash set (deliberately NOT pkg/shm-backed - this
 * is diagnostic machinery examining the allocator itself, matching
 * mem_dbg_hash.c's own independence), then walks every chunk's cells
 * checking membership.
 *
 * Accuracy: exact when called pre-fork (hg_stats_core_init(), single
 * process, no siblings yet to hide cells in their own private free
 * stacks). Best-effort post-fork (hg_status_dbg()'s memdump path): this
 * process's own private free stack IS included, but a cell idling in some
 * OTHER worker's private free_head chain at the moment of the walk will be
 * misreported as live - an inherent consequence of the lock-free private-
 * cache design, not a bug. Documented, not silently pretended away.
 */
struct hg_free_set_entry {
	void *addr;
	struct hg_free_set_entry *next;
};

struct hg_free_set {
	struct hg_free_set_entry **buckets;
	unsigned int nbuckets;
};

static int hg_free_set_init(struct hg_free_set *set, unsigned int expected)
{
	unsigned int n = 64;

	while (n < expected * 2 && n < (1U << 24))
		n <<= 1;
	set->buckets = malloc(n * sizeof(*set->buckets));
	if (!set->buckets)
		return -1;
	memset(set->buckets, 0, n * sizeof(*set->buckets));
	set->nbuckets = n;
	return 0;
}

static inline unsigned int hg_ptr_hash(const void *p, unsigned int nbuckets)
{
	unsigned long v = (unsigned long)p;

	v ^= v >> 16;
	v *= 2654435761UL;   /* Knuth multiplicative hash */
	v ^= v >> 13;
	return (unsigned int)(v & (nbuckets - 1));
}

static void hg_free_set_add(struct hg_free_set *set, void *addr)
{
	unsigned int h = hg_ptr_hash(addr, set->nbuckets);
	struct hg_free_set_entry *e = malloc(sizeof *e);

	if (!e)
		return; /* best-effort: a missed insert only risks a free cell
		         * being misreported as live in a diagnostic dump, never
		         * a correctness issue for the allocator itself */
	e->addr = addr;
	e->next = set->buckets[h];
	set->buckets[h] = e;
}

static int hg_free_set_has(struct hg_free_set *set, void *addr)
{
	struct hg_free_set_entry *e;

	for (e = set->buckets[hg_ptr_hash(addr, set->nbuckets)]; e; e = e->next)
		if (e->addr == addr)
			return 1;
	return 0;
}

static void hg_free_set_destroy(struct hg_free_set *set)
{
	unsigned int i;
	struct hg_free_set_entry *e, *n;

	for (i = 0; i < set->nbuckets; i++)
		for (e = set->buckets[i]; e; e = n) {
			n = e->next;
			free(e);
		}
	free(set->buckets);
}

static void hg_free_set_populate(struct hg_block *hb, struct hg_free_set *set)
{
	int c;
	void *cur;
	struct hg_palloc *pl = hg_get_palloc(hb);

	for (c = 0; c < HG_NCLASSES; c++) {
		for (cur = hb->gpool[c]; cur; cur = cell_next(cur))
			hg_free_set_add(set, cur);
		if (pl)
			for (cur = pl->cls[c].free_head; cur; cur = cell_next(cur))
				hg_free_set_add(set, cur);
	}
}

void hg_arena_walk_live(struct hg_block *hb,
		void (*cb)(void *payload, void *ctx), void *ctx)
{
	struct hg_free_set set;
	struct hg_chunk *ch;
	unsigned int i, total_free = 0;
	char *cell_start;

	lock_get(&hb->lock);

	for (i = 0; i < HG_NCLASSES; i++)
		total_free += hb->gpool_n[i];

	if (hg_free_set_init(&set, total_free < 1024 ? 1024 : total_free) < 0) {
		lock_release(&hb->lock);
		LM_ERR("%s: out of memory building the live-cell diagnostic "
			"set - skipping the walk\n", hb->name);
		return;
	}
	hg_free_set_populate(hb, &set);

	for (ch = hb->chunks; ch; ch = ch->next) {
		for (i = 0; i < ch->cells; i++) {
			cell_start = (char *)ch + sizeof(struct hg_chunk) +
				(unsigned long)i * ch->cell_size;
			if (!hg_free_set_has(&set, cell_start))
				cb(cell_start + HG_CELL_HDR, ctx);
		}
	}

	hg_free_set_destroy(&set);
	lock_release(&hb->lock);
}

#ifdef SHM_EXTRA_STATS
static void hg_stats_core_init_cb(void *payload, void *ctx)
{
	HG_STATS_IDX(payload) = (unsigned long)*(int *)ctx;
}

void hg_arena_stats_core_init(struct hg_block *hb, int core_index)
{
	hg_arena_walk_live(hb, hg_stats_core_init_cb, &core_index);
	hg_large_stats_core_init(hb, core_index);
}
#endif

#endif /* HG_MALLOC */


/* =========================================================================
 * Statistics (shm arena only - see hg_register_stats() in hg_malloc.h)
 *
 * The same figures hg_stats reports over MI, as core statistics so a dashboard
 * can graph them.  Without this, hugepage consumption shows up only as the
 * host pool draining (node_exporter's HugePages_Free) with nothing attributing
 * it to the allocator.
 *
 * NAMING IS DELIBERATE: hg_shm_* under the "hgmem" module, kept clearly apart
 * from cachedb_perf's hugepage_arena_* statistics.  Those describe that
 * module's OWN optional dedicated reservation and read 0 when it has none -
 * different memory entirely.  The two have already been confused once: a
 * cachedb_perf arena reported as "plain 4K pages" was read as the cache
 * sitting on small pages when its data was really in HG_MALLOC's 2M-backed
 * shm.  Anything ambiguous here invites that mistake again, or double-counting.
 * ========================================================================= */

#include "../statistics.h"
#include "shm_mem.h"   /* mem_allocator_shm */

enum hg_stat_field {
	HGS_TIER = 0, HGS_TOTAL, HGS_PINNED_BYTES, HGS_CARVED, HGS_CARVED_PEAK,
	HGS_CHUNKS, HGS_FREE_TO_CARVE, HGS_LIVE, HGS_LIVE_PEAK, HGS_PAYLOAD,
	HGS_CELLS, HGS_SLAB_LIVE, HGS_SLAB_RECYCLED,
};

static unsigned long hg_shm_stat(void *ctx)
{
	struct hg_block *hb = (struct hg_block *)shm_block;

	if (!hb)
		return 0;
	switch ((enum hg_stat_field)(long)ctx) {
	case HGS_TIER:          return hb->tier;
	case HGS_TOTAL:         return hb->size;
	/* bytes rather than the MB the MI reports, so it composes with the
	 * other byte-valued statistics and with node_exporter's page counts */
	case HGS_PINNED_BYTES:  return (unsigned long)hb->locked_mb << 20;
	/* carved: taken from the arena and cut into size-class chunks.  Never
	 * returned, so this only ever grows - and it is what free_to_carve
	 * counts down from. */
	case HGS_CARVED:        return hb->real_used;
	case HGS_CARVED_PEAK:   return hb->max_real_used;
	case HGS_CHUNKS:        return hb->nchunks;
	case HGS_FREE_TO_CARVE: return hb->size - hb->real_used;
	/* live: handed out right now.  NOT the same as carved - the gap is
	 * memory sitting on per-process free stacks, reusable only within its
	 * own size class, which is expected slab behaviour and not a leak. */
	case HGS_LIVE:          return hg_get_real_used(hb);
	case HGS_LIVE_PEAK:     return hb->max_live_used;
	case HGS_PAYLOAD:       return hg_used(hb);
	case HGS_CELLS:         return hg_fragments(hb);
	/* SLAB ONLY: large allocations go to the boundary-tag tier and never
	 * appear here, so this is deliberately NOT comparable with payload */
	case HGS_SLAB_LIVE:     return hg_cell_live(hb);
	case HGS_SLAB_RECYCLED: return hg_slab_recycled(hb);
	}
	return 0;
}

static const struct {
	const char *name;
	enum hg_stat_field field;
} hg_stat_defs[] = {
	{"hg_shm_tier",          HGS_TIER},
	{"hg_shm_total_size",    HGS_TOTAL},
	{"hg_shm_pinned_bytes",  HGS_PINNED_BYTES},
	{"hg_shm_carved",        HGS_CARVED},
	{"hg_shm_carved_peak",   HGS_CARVED_PEAK},
	{"hg_shm_chunks",        HGS_CHUNKS},
	{"hg_shm_free_to_carve", HGS_FREE_TO_CARVE},
	{"hg_shm_live",          HGS_LIVE},
	{"hg_shm_live_peak",     HGS_LIVE_PEAK},
	{"hg_shm_live_payload",  HGS_PAYLOAD},
	{"hg_shm_live_cells",    HGS_CELLS},
	{"hg_shm_slab_live",     HGS_SLAB_LIVE},
	{"hg_shm_slab_recycled", HGS_SLAB_RECYCLED},
	{NULL, 0}
};

int hg_register_stats(void)
{
	int i;

	if (mem_allocator_shm != MM_HG_MALLOC &&
	        mem_allocator_shm != MM_HG_MALLOC_DBG)
		return 0;       /* a different allocator is in use - nothing to say */

	for (i = 0; hg_stat_defs[i].name; i++) {
		if (register_stat2("hgmem", (char *)hg_stat_defs[i].name,
		        (stat_var **)hg_shm_stat, STAT_NO_RESET|STAT_IS_FUNC,
		        (void *)(long)hg_stat_defs[i].field, 0) != 0) {
			LM_ERR("failed to add the %s statistic\n", hg_stat_defs[i].name);
			return -1;
		}
	}
	return 0;
}
