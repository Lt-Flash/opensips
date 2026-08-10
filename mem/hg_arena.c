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
#include "hg_buddy.h"
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

#define HG_CHUNK_SMALL    (256 * 1024)  /* ceiling on chunk_max, see below */
#define HG_CHUNK_MIN      (8 * 1024)    /* floor, however tiny the arena */
/*
 * Target cells per block. A block is the unit of reclaim, so this is a
 * fragmentation knob, not an amortisation one: fewer cells per block means a
 * block drains sooner, more means fewer carves. 32 is the design's figure and
 * puts class 2048 on a 64 KB block; do not raise it without re-measuring what
 * fraction of blocks actually reach empty.
 */
#define HG_CELLS_PER_BLOCK 32
#define HG_REFILL_BATCH   32             /* cells pulled from the global pool */

/*
 * Private free-cache budget.
 *
 * This used to be a flat count - 256 cells per class, whatever the class -
 * which bounds nothing, because the same number means 16 KB in class 64 and
 * 16 MB in class 65536.  One thread could therefore hoard 16 MB of a single
 * class, and nothing but an arena smaller than the ceiling stopped it.
 *
 * The bound that matters is BYTES, so express it that way and convert to a
 * per-class cell count at init.  Two numbers define the policy:
 *
 *   HG_PRIVATE_PCT       what share of the arena ALL private caches together
 *                        may hold - the whole fleet of threads, every class;
 *   HG_PRIVATE_CONSUMERS how many threads to divide that share between.
 *
 * So one thread gets PCT/CONSUMERS of the arena (25/16 ~ 1.6%), and the
 * aggregate stays at PCT by construction however many classes exist.
 * Measured on the real ladder: shm 256 MB gives a thread at most 3.0 MB
 * across all 21 classes, and pkg 24 MB gives it 0.29 MB.  The class that
 * motivated this - 65536 - drops from 256 cells (16 MB!) to 3 on shm and to
 * none at all on pkg.
 *
 * The binding case is PKG, not SHM, and it is new in 4.1: pkg is MAP_PRIVATE
 * per process, and TCP main is ONE process running an IO thread pool (one
 * thread per CPU by default) against a single 8 MB pkg arena.  Sixteen threads
 * each hoarding "a few cells of every class" is how an 8 MB arena disappears
 * into caches that no other thread can reach.  Budgeting for the pool size
 * keeps the total bounded by construction: worst case is
 * HG_PRIVATE_PCT percent of the arena, however many classes exist.
 *
 * HG_PRIVATE_CAP keeps the old ceiling for the small classes, where the byte
 * budget alone would now permit MORE hoarding than before on a large arena -
 * this change is meant to lower the bound, never raise it.
 */
#define HG_PRIVATE_PCT        25   /* of the arena, per thread, all classes */
#define HG_PRIVATE_CONSUMERS  16   /* budget for a per-CPU IO pool          */
#define HG_PRIVATE_CAP        256  /* never above the historical flat cap   */

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
 *     exceeds any sane private-cache bound and drove the donation loop off
 *     the end of a
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
/*
 * Attribute a cell to its block and move that block's global-pool count.
 *
 * This is the cache/block transition the whole reclaim design turns on. It is
 * O(1) - two shifts, a table byte and a mask (hg_buddy_block_of) - and it runs
 * only when a cell crosses between a thread's private cache and the global
 * pool, never on the private-LIFO fast path.
 *
 * Silently does nothing for a cell whose block cannot be resolved. That is not
 * laxity: cells carved before the buddy owned the arena do not exist, but a
 * cell from a multi-page run legitimately resolves to NULL, and a wild pointer
 * must not be allowed to increment some innocent block's counter towards a
 * false "wholly free".
 */
static inline void block_gpool_delta(struct hg_block *hb, int c,
                                     void *cell_start, int delta)
{
	struct hg_chunk *ch = hg_buddy_block_of(hb, cell_start);

	if (!ch)
		return;
	/*
	 * Confirm the block really is a class chunk of THIS class before
	 * writing to it. The large tier allocates buddy blocks too, and those
	 * start with a struct hg_large_chunk - ch->in_gpool would land inside
	 * its first_frag pointer and corrupt the large heap. Only class cells
	 * reach the global pool today, so this should never trip; it is here
	 * because the cost of being wrong is silent corruption of a different
	 * allocator tier, and the check is one already-cached load.
	 */
	if (ch->cls != (unsigned int)c) {
		LM_CRIT("%s: cell %p resolves to block %p of class %u, expected "
			"class %d - not touching it\n",
			hb->name, cell_start, (void *)ch, ch->cls, c);
		return;
	}
	if (delta > 0) {
		if (ch->in_gpool >= ch->cells) {
			LM_CRIT("%s: block %p already has all %u cells in the global "
				"pool, refusing to count another\n",
				hb->name, (void *)ch, ch->cells);
			return;
		}
		ch->in_gpool++;
	} else {
		if (ch->in_gpool == 0) {
			LM_CRIT("%s: block %p has no cells in the global pool to "
				"remove\n", hb->name, (void *)ch);
			return;
		}
		ch->in_gpool--;
	}
}

static inline void gpool_push(struct hg_block *hb, int c, void *cell_start)
{
	cell_set_next(cell_start, hb->gpool[c]);
	hb->gpool[c] = cell_start;
	hb->gpool_n[c]++;
	block_gpool_delta(hb, c, cell_start, +1);
}

static inline void *gpool_pop(struct hg_block *hb, int c)
{
	void *cell_start = hb->gpool[c];

	if (cell_start) {
		hb->gpool[c] = cell_next(cell_start);
		hb->gpool_n[c]--;
		block_gpool_delta(hb, c, cell_start, -1);
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
	unsigned long off;

	/*
	 * Init only, once the buddy owns the arena.
	 *
	 * hg_buddy_init() reserves every leaf below hoff and publishes the rest
	 * as free. A bump AFTER that returns memory the buddy already considers
	 * free, so the same bytes get handed to two owners - silently, and with
	 * a delay before the corruption shows. There is exactly one legitimate
	 * caller left (the buddy carving its own metadata, before it is ready),
	 * so anything else is a bug and says so rather than corrupting.
	 */
	if (hb->buddy_ready) {
		LM_CRIT("%s: bump carve of %lu bytes after the buddy owns the "
			"arena - refusing, this would double-allocate\n",
			hb->name, size);
		return NULL;
	}

	off = __atomic_fetch_add(&hb->hoff, asz, __ATOMIC_RELAXED);

	if (off + asz <= hb->hsize)
		return hb->hbase + off;

	/* exhausted: undo would race other bumpers, so just leave hoff past
	 * the end (further allocs also fail) - correctness holds, we only
	 * lose the tail slack */
	return NULL;
}

/*
 * As hg_chunk_backing(), but the returned address is @align-aligned.
 *
 * The buddy layer needs whole huge pages at their natural alignment, and
 * over-allocating by align-1 to trim afterwards would throw away up to a
 * whole 2 MB page per page claimed. So this bumps by the EXACT padded amount
 * under a compare-exchange instead: the pad is computed from the candidate
 * offset, and if another bumper wins the race the pad is recomputed against
 * the new offset rather than reused.
 *
 * Unlike hg_chunk_backing() a failed reservation does not consume the tail -
 * the CAS simply never commits - so an oversized request cannot poison the
 * arena for the smaller ones behind it.
 *
 * @align must be a power of two. Alignment is applied to the ADDRESS, not to
 * the offset, because hbase itself is not guaranteed aligned on the non-Linux
 * reserve path.
 */
void *hg_backing_aligned(struct hg_block *hb, unsigned long size,
                         unsigned long align)
{
	unsigned long asz = (size + 63) & ~63UL;
	unsigned long cur, aligned_off, newoff;

	if (align < 64)
		align = 64;
	if (align & (align - 1)) {
		LM_ERR("%s: alignment %lu is not a power of two\n", hb->name, align);
		return NULL;
	}

	cur = __atomic_load_n(&hb->hoff, __ATOMIC_RELAXED);
	do {
		unsigned long addr = (unsigned long)hb->hbase + cur;

		aligned_off = cur + ((~addr + 1) & (align - 1));
		newoff = aligned_off + asz;
		if (newoff > hb->hsize)
			return NULL;
	} while (!__atomic_compare_exchange_n(&hb->hoff, &cur, newoff, 1,
	                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED));

	return hb->hbase + aligned_off;
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
	unsigned int want, least = sizeof(struct hg_chunk) + cell_sizes[c] * 2;

	/*
	 * Aim for HG_CELLS_PER_BLOCK cells, floored at one buddy leaf.
	 *
	 * This deliberately REPLACES the old "256 KB for every class up to
	 * 8 KB cells". A block is the unit of reclaim now, and a block only
	 * comes back when every one of its cells is free, so a block holding
	 * 2730 cells of class 96 is a block that will essentially never drain -
	 * one survivor pins 256 KB. That is exactly the "too coarse" failure
	 * the design rejects whole-chunk reclaim for.
	 *
	 * The sizes fall where the design says: class 96 lands on the 8 KB
	 * floor (85 cells), class 2048 on 64 KB (32 cells) rather than an 8 KB
	 * block holding only four.
	 */
	want = cell_sizes[c] * HG_CELLS_PER_BLOCK;
	if (want < HG_LEAF_SIZE)
		want = HG_LEAF_SIZE;

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
	int ord;

	/*
	 * Chunks are buddy blocks now, so the size rounds UP to an order. That
	 * is not waste: a chunk is a bag of cells, so a bigger block simply
	 * holds more of them, and in exchange the block is naturally aligned
	 * and - once the GC lands - returnable. The bump allocator it replaces
	 * could never give any of that back.
	 */
	ord = hg_buddy_order_for(hb, size);
	if (ord < 0) {
		LM_ERR("%s: class %d wants a %u byte chunk, larger than the %lu byte "
			"page the buddy tops out at\n", hb->name, c, size,
			(unsigned long)hb->hps);
		return -1;
	}
	size = (unsigned int)(HG_LEAF_SIZE << ord);
	ch = hg_buddy_alloc(hb, (unsigned int)ord);
	if (!ch) {
		LM_ERR("%s: no more HG_MALLOC arena memory for a %u byte chunk "
			"(class %d, order %d) - increase the arena size\n",
			hb->name, size, c, ord);
		return -1;
	}

	/* the block comes from the buddy carrying whatever the free-list
	 * linkage left in its first bytes, so every field is set here, not
	 * assumed zero */
	ch->in_gpool = 0;
	ch->fnext = NULL;
	ch->fprev = NULL;
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

/*
 * Turn the byte budget into a per-class cell count, once, at arena init.
 *
 * A class whose single cell already exceeds one thread's per-class share gets
 * priv_max 0: it is never cached privately and every free goes straight to the
 * shared pool.  That is the correct answer rather than a degenerate one - those
 * allocations are rare, so the lock they now take is rare too, and the memory
 * they would have pinned is large.
 *
 * priv_donate is half the cap, so the donation loop can always be satisfied by
 * a chain whose length matches nfree - the invariant the old fixed HG_DONATE
 * relied on, now maintained per class instead of by two constants that had to
 * be kept in the right order by hand.
 */
static void private_caps_init(struct hg_block *hb)
{
	unsigned long per_thread = (unsigned long)hb->size / 100 * HG_PRIVATE_PCT
	                           / HG_PRIVATE_CONSUMERS;
	unsigned long per_class  = per_thread / HG_NCLASSES;
	unsigned int c;

	for (c = 0; c < HG_NCLASSES; c++) {
		unsigned long n = per_class / cell_sizes[c];

		if (n > HG_PRIVATE_CAP)
			n = HG_PRIVATE_CAP;
		hb->priv_max[c]    = (unsigned int)n;
		hb->priv_donate[c] = (unsigned int)(n / 2);
		if (hb->priv_donate[c] == 0)
			hb->priv_donate[c] = 1;
	}

	LM_DBG("%s: private cache budget %lu B/thread (%lu B/class): "
		"class %u caches %u cells, class %u caches %u\n",
		hb->name, per_thread, per_class,
		cell_sizes[0], hb->priv_max[0],
		cell_sizes[HG_NCLASSES - 1], hb->priv_max[HG_NCLASSES - 1]);
}

/*
 * Lay out the page grid the v2 buddy addresses through, and PROVE the
 * arithmetic on real addresses before anything is built on top of it.
 *
 * Everything above the cell level resolves an address to its block with
 * two shifts and a mask (README.hg_arena_v2, "Address to block"). That is
 * only sound if the origin is huge-page aligned and the derived page and
 * leaf indices round-trip. Getting it wrong would not fail loudly at the
 * point of the mistake - it would hand out a block descriptor belonging to
 * a different block, which surfaces much later as corruption, so it is
 * checked here rather than assumed.
 *
 * Returns 0 when the grid is usable, -1 when the arithmetic does not hold.
 */
static int pages_init(struct hg_block *hb)
{
	unsigned long hps = hb->hps, off;
	unsigned int shift = 0;
	const char *end;
	int i;

	if (!hps || (hps & (hps - 1))) {
		LM_ERR("%s: huge page size %lu is not a power of two - cannot "
			"build the page grid\n", hb->name, hps);
		return -1;
	}
	while ((1UL << shift) < hps)
		shift++;
	hb->hps_shift = shift;

	/*
	 * Anchor the shift to the probed size. Everything below derives page
	 * indices FROM hps_shift and checks them against each other, so a shift
	 * that disagrees with hps is self-consistent and would sail through the
	 * probe loop - the corrupted-grid harness caught exactly that. This is
	 * the one comparison that ties the grid to physical reality.
	 */
	if ((1UL << shift) != hps) {
		LM_ERR("%s: page shift %u describes %lu bytes, but the probed huge "
			"page is %lu\n", hb->name, shift, 1UL << shift, hps);
		return -1;
	}

	if (shift <= HG_LEAF_SHIFT) {
		LM_ERR("%s: huge page size %lu is not larger than the %lu byte "
			"buddy leaf\n", hb->name, hps, HG_LEAF_SIZE);
		return -1;
	}

	/* page 0 starts at the first aligned address at or after hbase. On
	 * Linux every reserve path already aligns, so this is hbase and the
	 * subtraction below is zero; the non-Linux fallback is a plain mmap()
	 * and loses the unaligned head. */
	hb->pbase = (char *)(((unsigned long)hb->hbase + hps - 1) & ~(hps - 1));
	end = hb->hbase + hb->hsize;
	hb->npages = (unsigned long)(end - hb->pbase) >> shift;

	if (hb->pbase != hb->hbase)
		LM_INFO("%s: reservation base %p is not %lu-aligned, losing %lu "
			"bytes of head to align the page grid\n", hb->name,
			hb->hbase, hps, (unsigned long)(hb->pbase - hb->hbase));

	if (hb->npages == 0) {
		/* An arena smaller than one huge page is legitimate (a tiny -M on
		 * a 512M-page arm64 box), it just cannot carry a buddy page. The
		 * chunk allocator below is unaffected, so this is not fatal. */
		LM_INFO("%s: arena of %lu bytes holds no whole %lu byte page - "
			"buddy reclaim will be inactive\n",
			hb->name, hb->hsize, hps);
		return 0;
	}

	/* The grid must not describe memory the reservation does not own. Also
	 * caught by the harness: an npages one too large keeps every internal
	 * relation intact and only shows up against the reservation end. */
	if (hb->pbase + (hb->npages << shift) > end) {
		LM_ERR("%s: page grid of %lu pages ends at %p, past the %lu byte "
			"reservation ending at %p\n", hb->name, hb->npages,
			hb->pbase + (hb->npages << shift), hb->hsize, end);
		return -1;
	}

	/* --- the proof. Real addresses, spanning the whole grid. --- */
	for (i = 0; i < 5; i++) {
		unsigned long pg, leaf, want_pg;
		const char *probe, *pbase_of;

		switch (i) {
		case 0: want_pg = 0;               off = 0;                 break;
		case 1: want_pg = 0;               off = HG_LEAF_SIZE;      break;
		case 2: want_pg = hb->npages / 2;  off = hps / 2;           break;
		case 3: want_pg = hb->npages - 1;  off = 0;                 break;
		default:want_pg = hb->npages - 1;  off = hps - 1;           break;
		}
		probe = hb->pbase + (want_pg << shift) + off;

		if (!hg_in_pages(hb, probe)) {
			LM_ERR("%s: addressing self-test %d: %p should be inside the "
				"%lu-page grid at %p but is not\n",
				hb->name, i, probe, hb->npages, hb->pbase);
			return -1;
		}
		pg = hg_page_of(hb, probe);
		if (pg != want_pg) {
			LM_ERR("%s: addressing self-test %d: %p resolved to page %lu, "
				"expected %lu\n", hb->name, i, probe, pg, want_pg);
			return -1;
		}
		pbase_of = hg_page_base(hb, probe);
		if ((unsigned long)pbase_of & (hps - 1)) {
			LM_ERR("%s: addressing self-test %d: page base %p is not "
				"%lu-aligned\n", hb->name, i, pbase_of, hps);
			return -1;
		}
		if (probe - pbase_of != (long)off) {
			LM_ERR("%s: addressing self-test %d: %p is %ld bytes into its "
				"page, expected %lu\n", hb->name, i, probe,
				(long)(probe - pbase_of), off);
			return -1;
		}
		leaf = hg_leaf_of(hb, probe);
		if (leaf != off >> HG_LEAF_SHIFT ||
		    leaf >= hg_leaves_per_page(hb)) {
			LM_ERR("%s: addressing self-test %d: %p is leaf %lu, expected "
				"%lu of %lu\n", hb->name, i, probe, leaf,
				off >> HG_LEAF_SHIFT, hg_leaves_per_page(hb));
			return -1;
		}
	}

	/* and the negative side - the header region and the byte past the end
	 * must NOT classify as page memory, or a stray pointer would be
	 * "resolved" to a block that does not exist */
	if (hb->pbase != hb->hbase && hg_in_pages(hb, hb->hbase)) {
		LM_ERR("%s: addressing self-test: unaligned head %p classifies as "
			"page memory\n", hb->name, hb->hbase);
		return -1;
	}
	if (hg_in_pages(hb, hb->pbase + (hb->npages << shift))) {
		LM_ERR("%s: addressing self-test: the byte past the last page "
			"classifies as page memory\n", hb->name);
		return -1;
	}

	LM_DBG("%s page grid: %lu pages of %lu B at %p (shift %u), %lu leaves "
		"of %lu B per page; addressing verified\n",
		hb->name, hb->npages, hps, hb->pbase, shift,
		hg_leaves_per_page(hb), HG_LEAF_SIZE);
	return 0;
}

int hg_arena_init(struct hg_block *hb, unsigned long hdr_size)
{
	unsigned int idx, c, needed;

	if (pages_init(hb) < 0)
		return -1;

	/*
	 * Leave the block header itself untouched by the bump allocator, and
	 * start on a leaf boundary: a buddy block must be naturally aligned to
	 * its own size, and every size is a multiple of the leaf, so aligning
	 * the very first carve is what makes all of them aligned. Costs one
	 * rounding here - under one leaf, once per arena - and is the whole
	 * reason the address mask above can be a mask at all.
	 */
	hb->hoff = (hdr_size + HG_LEAF_SIZE - 1) & ~(HG_LEAF_SIZE - 1);
	if (hb->pbase != hb->hbase) {
		/* the grid does not start at hbase, so the first carve must clear
		 * the discarded head too */
		unsigned long skip = (unsigned long)(hb->pbase - hb->hbase);

		if (hb->hoff < skip)
			hb->hoff = skip;
	}

	/* see chunk_size_for(): no single chunk may swallow a big slice of a
	 * small arena. /64 keeps all 21 classes plus the large tier inside a
	 * third of the arena even in the worst case. */
	hb->chunk_max = hb->size / 64;
	if (hb->chunk_max > HG_CHUNK_SMALL)
		hb->chunk_max = HG_CHUNK_SMALL;
	if (hb->chunk_max < HG_CHUNK_MIN)
		hb->chunk_max = HG_CHUNK_MIN;

	private_caps_init(hb);

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

	/*
	 * Last, because it carves its metadata with the bump allocator and then
	 * reserves everything below the resulting hoff. Anything that bump-carves
	 * after this point would be handing out memory the buddy believes is
	 * free, which is why hg_chunk_backing() refuses once buddy_ready is set.
	 */
	if (hg_buddy_init(hb) < 0)
		return -1;
	return 0;
}

/*
 * NOTE: this has no callers anywhere in the tree - checked across all of
 * modules/ and the core - and is kept only as a published arena API. It is
 * routed through the buddy rather than the bump allocator regardless: left on
 * the bump it would be a loaded gun, silently double-allocating the moment
 * anyone did start calling it (see the guard in hg_chunk_backing()).
 */
void *hg_region_alloc(struct hg_block *hb, unsigned long size)
{
	struct hg_region *rg;
	unsigned long need = size + sizeof(struct hg_region) + 64;
	char *aligned;
	int ord;

	lock_get(&hb->lock);
	ord = hg_buddy_order_for(hb, need);
	rg = ord < 0 ? NULL : hg_buddy_alloc(hb, (unsigned int)ord);
	if (!rg && ord < 0)
		rg = hg_buddy_alloc_run(hb,
			(need + hb->hps - 1) >> hb->hps_shift);
	lock_release(&hb->lock);

	if (!rg) {
		LM_ERR("%s: no more HG_MALLOC arena memory for a %lu byte "
			"region\n", hb->name, need);
		return NULL;
	}
	if (ord >= 0)
		need = HG_LEAF_SIZE << ord;
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
		struct hg_block *owner = hg_owner(cell_start);

		if (!owner) {
			LM_CRIT("%s: %p belongs to no live arena - refusing to "
				"free it\n", hb->name, p);
			return;
		}

		/*
		 * A different arena of ours issued this cell, which is normal
		 * after fork: the child runs on its own pkg arena while
		 * modules still release things the parent allocated pre-fork.
		 * Hand it back to whoever owns it. Straight to that arena's
		 * global pool, deliberately - the per-thread cache exists to
		 * speed up reuse, and this process will never allocate from
		 * the foreign arena again, so caching there would strand the
		 * cell instead of freeing it.
		 */
		hg_xarena_frees++;
		hg_cell_free_global(owner, p);
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

	/* keep hoarding bounded: donate half once over this class's threshold.
	 * The threshold is a cell count derived from a byte budget, so a big
	 * class trips it after very few cells - and a class whose priv_max is 0
	 * trips it on the first free, which is exactly the intent: never cache
	 * that class privately. */
	if (pl->cls[c].nfree > hb->priv_max[c]) {
		unsigned int claimed = pl->cls[c].nfree;
		unsigned int donate  = hb->priv_donate[c];

		lock_get(&hb->lock);
		for (i = 0; i < donate; i++) {
			d = pl->cls[c].free_head;
			/* priv_donate is half priv_max, so a chain whose
			 * length matches nfree can always satisfy this loop.
			 * Reaching a NULL here therefore means nfree has
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
		if (i < donate)
			pl->cls[c].nfree = 0;
		lock_release(&hb->lock);

		if (i < donate)
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
		struct hg_block *owner = hg_owner(cell_start);

		/* see the matching comment in hg_cell_free() - reached both
		 * directly and by that function's redirect, so the redirect
		 * must not loop: it only ever passes the resolved owner. */
		if (!owner) {
			LM_CRIT("%s: %p belongs to no live arena - refusing to "
				"free it\n", hb->name, p);
			return;
		}
		if (owner != hb) {
			hg_xarena_frees++;
			hb = owner;
		}
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
