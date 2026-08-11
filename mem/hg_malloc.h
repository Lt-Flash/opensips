/*
 * hugepage-backed slab allocator
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

#ifndef hg_malloc_h
#define hg_malloc_h

#include <stdio.h>
#include "meminfo.h"
#include "common.h"
/* for process_no, used by hg_pstat_mine() below to pick this process's
 * stats slot. Included explicitly rather than relied on to arrive via
 * some other header - it does on a native build, and does not under a
 * cross-compiler. */
#include "../globals.h"

#undef HG_ROUNDTO

#if defined(__CPU_sparc64) || defined(__CPU_sparc)
	#define HG_ROUNDTO		sizeof(long long)
#else
	#define HG_ROUNDTO		sizeof(void *)
#endif

#define HG_NCLASSES  21
#define HG_CELL_MAX  65536  /* largest cell; bigger allocs fail (Phase 1) */

/* the four-tier huge-page ladder, best first (ported from cachedb_perf) */
enum hg_mem_tier {
	HG_MEM_HUGETLB = 1,   /* mmap MAP_HUGETLB */
	HG_MEM_THP_ADVISE,    /* shmem THP via MADV_HUGEPAGE, huge at fault */
	HG_MEM_THP_COLLAPSE,  /* shmem THP via MADV_COLLAPSE, post-fill */
	HG_MEM_4K,            /* plain pages - always works */
};

const char *hg_mem_tier_str(enum hg_mem_tier tier);

/*
 * Cell header, hidden before every returned pointer (like FM_FRAG(p) in
 * f_malloc) - unlike cachedb_perf, HG_MALLOC hands pointers to arbitrary
 * caller code via shm_malloc()/pkg_malloc(), so the class tag can NOT live
 * in-band at the front of the payload the way cachedb_perf's records do.
 *
 * Composable layout - DBG_MALLOC and SHM_EXTRA_STATS are INDEPENDENT build
 * flags (a plain non-DBG build can still have SHM_EXTRA_STATS on for
 * mem-group accounting), so each contributes its own slice rather than one
 * replacing the other, mirroring how f_malloc's fm_frag has both the
 * DBG_MALLOC file/func/line fields AND the SHM_EXTRA_STATS
 * statistic_index field as separate #ifdef'd struct members:
 *
 *   offset 0                          : class id (immutable, stamped at
 *                                        chunk-carve time)
 *   offset HG_ROUNDTO           (DBG)    : file
 *   offset HG_ROUNDTO*2         (DBG)    : func
 *   offset HG_ROUNDTO*3         (DBG)    : line
 *   offset HG_ROUNDTO+HG_CELL_HDR_DBG
 *          (SHM_EXTRA_STATS)          : statistic_index (mem-group index)
 *   offset HG_CELL_HDR                : payload starts here; while a cell
 *                                        is FREE, the first HG_ROUNDTO*2 bytes
 *                                        of payload double as the free-list
 *                                        link (cell_next()/cell_set_next()
 *                                        in hg_arena.c) - safe, since
 *                                        nobody reads payload of a free cell.
 */
#ifdef DBG_MALLOC
#define HG_CELL_HDR_DBG (HG_ROUNDTO * 3)  /* file ptr + func ptr + line */
#else
#define HG_CELL_HDR_DBG 0
#endif

#ifdef SHM_EXTRA_STATS
#define HG_CELL_HDR_STATS (HG_ROUNDTO)    /* statistic_index */
#else
#define HG_CELL_HDR_STATS 0
#endif

/*
 * Payloads must be aligned for the widest scalar a caller may store in
 * them; 8 covers uint64_t/double everywhere we build. This is NOT implied
 * by HG_ROUNDTO: on 32-bit ARM HG_ROUNDTO is 4, so the raw header below would be
 * 4 bytes in a plain build, and since cells always start 32-byte aligned
 * EVERY payload would land at 4 mod 8 - misaligned for any 64-bit field,
 * and an outright fault for the LDREXD/STREXD that gen_lock_t and the
 * 64-bit atomics in shared structs compile down to. (f_malloc does not hit
 * this only because its header is a struct that happens to be 8-aligned.)
 *
 * The padding goes at the END of the header, so every field offset below
 * stays exactly where it was and only the header's total size grows.
 */
#define HG_PAYLOAD_ALIGN 8
#define HG_CELL_HDR_RAW  (HG_ROUNDTO + HG_CELL_HDR_DBG + HG_CELL_HDR_STATS)
#define HG_CELL_HDR \
	(((HG_CELL_HDR_RAW + HG_PAYLOAD_ALIGN - 1) / HG_PAYLOAD_ALIGN) \
	 * HG_PAYLOAD_ALIGN)

/* offset of the statistic_index field, valid only when SHM_EXTRA_STATS */
#define HG_CELL_STATS_OFF (HG_ROUNDTO + HG_CELL_HDR_DBG)

#define HG_HDR(p)   ((char *)(p) - HG_CELL_HDR)
#define HG_CLASS(p) (*(unsigned char *)HG_HDR(p))

/* valid only when SHM_EXTRA_STATS; uniform for small cells AND large frags,
 * since the tag region (and thus this offset) always sits immediately
 * before payload regardless of what precedes it (fixed chunk cell vs.
 * hg_lfrag boundary-tag header) */
#ifdef SHM_EXTRA_STATS
#define HG_STATS_IDX(p) (*(unsigned long *)(HG_HDR(p) + HG_CELL_STATS_OFF))
#endif

/* tag-byte value marking a cell as belonging to the large-object tier
 * (hg_large.c) rather than a fixed size class - valid classes are
 * [0, HG_NCLASSES), so this is the first value past them, still
 * distinguishable from genuine corruption (any other out-of-range byte) */
#define HG_LARGE_MARKER HG_NCLASSES

struct hg_large_chunk;   /* opaque here, defined in hg_large.c */
struct hg_lfrag;         /* opaque here, defined in hg_large.h */

/* manifest sizeof(struct hg_lfrag): needed here (opaque type, can't call
 * sizeof() on it) to locate a large frag's header from a payload pointer.
 * hg_large.c static_asserts this matches the real struct, so any future
 * field change there fails the build here instead of drifting silently. */
#define HG_LFRAG_HDR_SIZE (4 * HG_ROUNDTO)

/*
 * A chunk is one buddy block dedicated to one cell class, and its header sits
 * in the FIRST bytes OF THAT BLOCK.
 *
 * In-block rather than in a side array, and the arithmetic is the whole
 * argument: at ~32 bytes per block, a separate array over a 5 GB arena would
 * be 20 MB - twenty times every other piece of metadata combined - where
 * in-block costs nothing external, shares a cache line with the block being
 * touched anyway, and gives up exactly one cell.
 *
 * The enabler was already in the tree before any of v2: hg_cell_free() reads
 * the class from the CELL's own header byte rather than from the chunk, so a
 * page can host blocks of different classes with no fast-path change.
 */
/*
 * Fullness grades. Eight is the design's figure: enough that "fullest
 * partial" is meaningfully sorted, few enough that a block changes grade
 * rarely rather than on every cell, and that the scan for a source block is
 * eight pointer tests.
 */
#define HG_GRADES         8
#define HG_GRADE_NONE     0xffffffffu   /* no free cells, on no list */
#define HG_GRADE_DRAINED  0xfffffffeu   /* every cell free, on drained[] */

/* a block whose cells are ALL in the global pool - the GC's work queue */
#define HG_CHUNK_DRAINED    (1u << 0)
/* selected by this GC pass; its cells are being unlinked right now */
#define HG_CHUNK_RECLAIMING (1u << 1)

struct hg_chunk {
	/*
	 * Global registry. Doubly linked, which it did not need to be while
	 * chunks were immortal: hg_slab_recycled() and the DBG dump walker both
	 * traverse it, so a reclaimed block MUST come out of it - otherwise the
	 * first keeps counting capacity that no longer exists and the second
	 * reads a block the buddy has already handed to another class.
	 */
	struct hg_chunk *next;
	struct hg_chunk *prev;
	unsigned int cls;         /* immutable */
	unsigned int cell_size;   /* total slot size, header included */
	unsigned int cells;

	/*
	 * Cells of this block currently parked in the GLOBAL pool, i.e.
	 * definitively free and reachable by anyone. Maintained only at
	 * cache/block transitions (gpool_push/gpool_pop), which are 1-3% of
	 * operations and already under hb->lock - a free that lands in a
	 * thread's private LIFO does not touch this, by design.
	 *
	 * A cell sitting in some thread's private cache therefore still counts
	 * as live. That is deliberately conservative: it can only delay a
	 * reclaim, never cause a premature one, and flushing those caches is
	 * exactly what the idle sweep (task #59) is for.
	 *
	 * in_gpool == cells means every cell of this block is free and globally
	 * visible, which is the condition the GC in task #57 acts on.
	 */
	unsigned int in_gpool;
	unsigned int flags;       /* HG_CHUNK_DRAINED / _RECLAIMING */
	unsigned int order;       /* buddy order, so the GC can hand it back */

	/*
	 * This block's OWN free cells, and which fullness list it currently sits
	 * on. The shared pool used to be one mixed list per class, so a 32-cell
	 * refill skimmed cells from whichever blocks happened to be at its head
	 * and topped every block up a little - measured, that left blocks
	 * stalled at 88% drained and the GC with nothing to collect.
	 *
	 * Per block, a refill can instead take all 32 from ONE block, and the
	 * fullness grading makes it the FULLEST partial - so that block is used
	 * up while emptier ones are left alone to reach zero.
	 *
	 * This costs the fast path nothing: hg_cell_free() still pushes to the
	 * thread-private TLS LIFO with no lock, and only the donation/refill
	 * path - already under hb->lock, 1-3% of operations - touches this.
	 */
	void        *free_head;
	unsigned int grade;       /* HG_GRADE_NONE / _DRAINED, or 0..HG_GRADES-1 */

	/*
	 * The list this block is currently on. Today that is only the per-class
	 * drained queue; task #58 generalises it into fullness buckets, of
	 * which "drained" is simply the empty one - which is why the design
	 * calls the empty bucket the GC's work queue.
	 */
	struct hg_chunk *fnext;
	struct hg_chunk *fprev;
} __attribute__ ((aligned (64)));

/*
 * v2 buddy geometry - mem/README.hg_arena_v2, "Address to block".
 *
 * The leaf is the minimum buddy order. 8 KB is a deliberate floor: the leaf
 * array is one byte per leaf per page, so dropping to 1 KB would multiply that
 * array by eight for no measured gain. Do not lower it without evidence.
 *
 * Leaves per page is NOT a constant, because the huge page size is probed -
 * 256 leaves on a 2M page, 65536 on a 512M arm64 page - so it is derived from
 * hb->hps_shift rather than baked in.
 */
#define HG_LEAF_SHIFT  13
#define HG_LEAF_SIZE   (1UL << HG_LEAF_SHIFT)

/*
 * Ceiling on the number of buddy orders, for the fixed free-list array in
 * struct hg_block. The REAL count is derived per arena as
 * hps_shift - HG_LEAF_SHIFT: 8 on a 2 MB page with 8 KB leaves, 16 on a
 * 512 MB arm64 page. 24 leaves room for a 128 GB page that does not exist yet
 * and costs 25 pointers in one struct.
 */
#define HG_MAX_ORDERS  24

struct hg_page;      /* hg_buddy.h */
struct hg_free_blk;  /* hg_buddy.h */

/* the accessors that turn an address into a page and a leaf live just after
 * struct hg_block below - they dereference it, so they cannot precede it */

struct hg_region {
	struct hg_region *next;
	unsigned long size;
};

/* per-process private allocation state for ONE hg_block instance. Several
 * hg_block instances can be live in the same process at once (shm, shm_dbg,
 * pkg all selecting HG_MALLOC simultaneously) so this is looked up per-block,
 * not a single global - see hg_get_palloc() in hg_arena.c */
struct hg_palloc {
	struct hg_block *owner;   /* which block this state belongs to */
	struct {
		char *bump;            /* next unused cell in own chunk */
		unsigned int left;
		void *free_head;       /* private LIFO free stack */
		unsigned int nfree;
	} cls[HG_NCLASSES];
};

/*
 * Per-process counters for the two stats the lock-free fast path has to
 * touch on every single allocation and free.
 *
 * Keeping them as plain fields in hg_block would make every worker do an
 * unsynchronized read-modify-write on the same shared words - a data race
 * on every architecture (x86 TSO does not make "x += y" atomic either;
 * a weakly-ordered machine just loses more updates), whose symptom is
 * silently under-reported memory in /info. Making them atomic instead
 * would be correct but would put a contended shared cache line back on
 * the fast path - exactly what the per-process free stacks exist to
 * avoid. So each process gets its own cache-line-isolated slot and
 * readers sum the slots.
 *
 * Both fields are SIGNED on purpose: a cell allocated by one process can
 * be freed by another (that is what the shared pool is for), so an
 * individual slot legitimately goes negative. Only the sum is meaningful.
 */
#define HG_STAT_SLOTS     256
#define HG_STAT_LINE      64

struct hg_pstat {
	long used;       /* payload bytes handed out by this process */
	long fragments;  /* live cells handed out by this process */
	/* Cell-slot bytes (header + payload + size-class round-up) currently
	 * handed out by this process. "used" alone cannot tell how much ARENA
	 * a process is holding, because a 100-byte request occupies a whole
	 * 128-byte slot; hg_slab_recycled() needs the slot figure to work out
	 * how much carved capacity is sitting idle. Lives on the same
	 * already-private cache line as the two counters above, so maintaining
	 * it costs no extra cache traffic on the fast path. */
	long cell_live;
	char _pad[HG_STAT_LINE - 3 * sizeof(long)];
} __attribute__ ((aligned (HG_STAT_LINE)));

struct hg_block {
	char *name; /* purpose of this memory block */

	gen_lock_t lock;          /* slow paths only: gpool + chunk carve */

	struct hg_chunk *chunks;
	unsigned int nchunks;
	/* upper bound on one chunk, derived from the arena size at init -
	 * see chunk_size_for() in hg_arena.c */
	unsigned int chunk_max;
	/* Per-THREAD private free-cache bound, in CELLS, one entry per class,
	 * derived from the arena size at init - see private_caps_init() in
	 * hg_arena.c.  A flat cell count cannot bound anything, because the
	 * same count means 16 KB in class 64 and 16 MB in class 65536; these
	 * are the byte budget expressed per class.  priv_max may legitimately
	 * be 0, which means "never cache this class privately" - the class is
	 * large enough that one cell already exceeds a thread's whole share,
	 * and such allocations are rare enough that the shared pool is the
	 * right home for them. */
	unsigned int priv_max[HG_NCLASSES];
	unsigned int priv_donate[HG_NCLASSES];
	struct hg_region *regions;
	/*
	 * Shared free cells, per class, held as BLOCKS graded by fullness
	 * rather than as one mixed cell list. bucket[c][0] holds the blocks
	 * with the fewest free cells, so scanning from 0 up finds the fullest
	 * partial in O(HG_GRADES) - the "concentration" the design requires.
	 * A block with no free cells is on no list; one with every cell free is
	 * on drained[] instead, which is the GC's queue.
	 */
	struct hg_chunk *bucket[HG_NCLASSES][HG_GRADES];
	unsigned int gpool_n[HG_NCLASSES];   /* free cells of the class, total */
	unsigned long lo, hi;           /* extent watermarks */

	/* large-object tier (hg_large.c): list of independently-carved
	 * chunks, each an f_malloc-style boundary-tag heap, sharing ONE free
	 * list across all of them; hb->lock guards all of it (slow path
	 * already, no separate lock needed) */
	struct hg_large_chunk *large_chunks;
	struct hg_lfrag *large_free;
	/*
	 * Backing the large tier holds from the buddy grid, and how much of it
	 * is handed out right now.  Both are needed because the two answer
	 * different questions and only the first one is what the grid lost:
	 * a chunk is taken whole from the buddy, then sub-allocated, so
	 * charging only the live fragments (which is what this tier used to do)
	 * left real_used under-reporting the arena's true footprint by the
	 * chunks' unused slack - and free_to_carve over-reporting what was
	 * left, monotonically, which is exactly the v1 pathology v2 exists to
	 * remove.  large_backing is charged to real_used at chunk acquisition;
	 * large_recycled() = large_backing - large_live is the large tier's
	 * analogue of hg_slab_recycled() and is subtracted back out to get the
	 * live figure.
	 */
	unsigned long large_backing;
	unsigned long large_live;

	/* Bytes carved from the reservation into chunks/regions, i.e. the
	 * arena's own footprint. Only ever changed while holding hb->lock
	 * (carve_chunk, hg_region_alloc, the large tier), never on the
	 * lock-free fast path, so a plain field is safe here. */
	unsigned long real_used;
	unsigned long max_real_used;
	/* High-water mark of the LIVE figure that hg_get_real_used() reports, as
	 * opposed to max_real_used above, which is the peak CARVE. They are
	 * different quantities: carve only ever grows (chunks are never
	 * un-carved), so reporting it as max_used made max_used drift away from
	 * real_used forever instead of meaning "the peak real_used reached" -
	 * unlike every other allocator. Sampled, not exact: refreshed whenever
	 * the stats are read, so a spike between two reads can be missed. The
	 * unsynchronized max update is a benign race - a lost update can only
	 * under-report, never over-report. */
	unsigned long max_live_used;

	/* the fast-path counters - see struct hg_pstat above. Summed by
	 * hg_used()/hg_fragments(); never read directly. */
	struct hg_pstat pstat[HG_STAT_SLOTS];
	unsigned long size;        /* total arena size */

	/* the huge-page reservation this block owns: a pre-fork (or per-process,
	 * for PKG), never-unmapped, 2M-aligned MAP_SHARED (or private, for PKG)
	 * region. Chunks bump from it lock-free (atomic hoff) */
	char                 *hbase;
	unsigned long         hsize;
	volatile unsigned long hoff;
	enum hg_mem_tier      tier;
	unsigned long         locked_mb;

	/*
	 * v2 buddy substrate - see mem/README.hg_arena_v2.
	 *
	 * The whole design turns "which block owns this address" into two shifts
	 * and a mask, which needs a known-aligned origin. hbase is NOT reliably
	 * that origin: all three Linux reserve paths align it (MAP_HUGETLB is
	 * aligned by the kernel, the THP tiers align explicitly at
	 * hg_malloc.c:301), but the non-Linux fallback takes a plain
	 * mmap(NULL, ...) and gets only page alignment. So page 0 starts at
	 * pbase, hbase rounded up to a huge page - equal to hbase everywhere it
	 * matters, and correct where it is not.
	 *
	 * hps is PROBED, not assumed: it is 2M on x86_64 and on arm64 with 4K
	 * base pages, but 32M with 16K pages and 512M with 64K. A hardcoded
	 * ">> 21" would mis-address every block on those machines, which is the
	 * same trap hg_hps() already exists to avoid for the mapping itself.
	 */
	unsigned long hps;         /* huge page size, probed at reserve time */
	unsigned int  hps_shift;   /* log2(hps), so page-of is a shift */
	char         *pbase;       /* page 0 - hbase rounded up to hps */
	unsigned long npages;      /* whole pages from pbase to the reservation end */

	/*
	 * Buddy state (hg_buddy.c). Every field here is written only while
	 * holding hb->lock - the buddy is entirely slow path, reached from
	 * carve_chunk() and the large tier, never from the cell fast path.
	 */
	struct hg_page     *pages;                    /* npages descriptors */
	struct hg_free_blk *bfree[HG_MAX_ORDERS + 1]; /* free list per order */
	unsigned long       nfree[HG_MAX_ORDERS + 1]; /* its length, per order */
	unsigned long       buddy_free_leaves;        /* free leaves, whole arena */
	unsigned int        buddy_top;                /* whole-page order, cached */
	unsigned int        buddy_ready;              /* 0 until hg_buddy_init() */

	/*
	 * GC work queue: blocks whose every cell is in the global pool, per
	 * class. Reclaiming one means unlinking its cells from that class's
	 * free list, which is a walk - so drained blocks accumulate here and a
	 * single walk serves all of them, rather than one walk per block.
	 */
	struct hg_chunk    *drained[HG_NCLASSES];
	unsigned int        ndrained[HG_NCLASSES];
	unsigned long       gc_blocks_returned;       /* lifetime, for stats */
	unsigned long       gc_passes;
	unsigned long       cache_flushes;            /* sweeps run */
	unsigned long       cells_flushed;            /* cells recovered from TLS */
	unsigned long       buddy_splits;             /* blocks split down an order */
	/*
	 * RUNTIME merges only.  hg_buddy_init() publishes the one page that the
	 * block header and buddy metadata straddle leaf by leaf, through the
	 * ordinary free path, so the tree is built by the normal rules - and
	 * every one of those coalesces used to land in this counter.  That gave
	 * it an arbitrary startup offset with no relation to fragmentation:
	 * an idle 8 MB pkg arena reads 7 splits against 244 merges purely from
	 * init.  The init total is snapshotted into buddy_merges_init and this
	 * counter is rebased to 0, so splits and merges finally share a zero
	 * point and their difference means something.
	 */
	unsigned long       buddy_merges;             /* blocks merged with a buddy */
	unsigned long       buddy_merges_init;        /* coalesces done building the tree */
	unsigned long       blocks_carved;            /* class blocks cut, lifetime */
	/* set while a flush walks a cache chain: pushing a cell can reclaim its
	 * block, and the next cell on the chain may live in that same block */
	unsigned int        gc_deferred;
	/*
	 * Reserve floor: free leaves below which the arena is treated as under
	 * pressure. Set once at init as a fraction of the grid. Not consumable
	 * by an expanding class in the sense that crossing it triggers a sweep
	 * BEFORE the failure, which is the only warning an operator can act on.
	 */
	unsigned long       reserve_floor;
	unsigned int        below_floor;      /* hysteresis: already reported */
	unsigned long       floor_crossings;

	unsigned char size2class[(HG_CELL_MAX / HG_ROUNDTO) + 1];
} __attribute__ ((aligned (HG_ROUNDTO)));

/*
 * Address -> page/leaf, the arithmetic the whole v2 buddy layer rests on.
 * See the HG_LEAF_SHIFT block above and mem/README.hg_arena_v2.
 * pages_init() in hg_arena.c verifies these against real addresses at every
 * arena init, and refuses to start the arena if they do not round-trip.
 */

/* how many leaves tile one huge page */
static inline unsigned long hg_leaves_per_page(const struct hg_block *hb)
{
	return 1UL << (hb->hps_shift - HG_LEAF_SHIFT);
}

/* Is @p inside the page-addressable region? Everything before pbase (the
 * block header, and on a non-Linux fallback the unaligned head) is arena
 * memory but not buddy memory, so it must answer NO. */
static inline int hg_in_pages(const struct hg_block *hb, const void *p)
{
	return (const char *)p >= hb->pbase &&
	       (const char *)p <  hb->pbase + (hb->npages << hb->hps_shift);
}

/* page index of @p; only meaningful when hg_in_pages() */
static inline unsigned long hg_page_of(const struct hg_block *hb, const void *p)
{
	return (unsigned long)((const char *)p - hb->pbase) >> hb->hps_shift;
}

/* first byte of the page holding @p */
static inline char *hg_page_base(const struct hg_block *hb, const void *p)
{
	return hb->pbase + (hg_page_of(hb, p) << hb->hps_shift);
}

/* leaf index of @p WITHIN its own page */
static inline unsigned long hg_leaf_of(const struct hg_block *hb, const void *p)
{
	unsigned long off = (unsigned long)((const char *)p - hb->pbase);

	return (off & ((1UL << hb->hps_shift) - 1)) >> HG_LEAF_SHIFT;
}

/*
 * Reserves its own huge-page-backed (or gracefully degraded) region of
 * @size bytes and lays out the block control structure at its start.
 *
 * Unlike fm_malloc_init(), this does NOT take a pre-reserved address: every
 * other allocator receives memory that shm_getmem() already mmap'd (a plain,
 * non-huge anonymous mapping), but HG_MALLOC always needs to run its own
 * hugepage tier ladder to get the mapping in the first place, so it owns the
 * whole reservation step itself (see hg_malloc.c). Returns NULL on total
 * failure (caller logs and aborts startup, per the "no silent fallback to a
 * different allocator" design decision).
 */
/* @shared: 1 for shm/shm_dbg (MAP_SHARED - one arena for every worker),
 * 0 for pkg (MAP_PRIVATE - each forked worker gets its own copy-on-write
 * arena, lock and free pools). See hg_mem_reserve() in hg_malloc.c for why
 * getting this wrong for pkg is a correctness AND a performance bug. */
struct hg_block *hg_malloc_init(unsigned long size, char *name, int shared,
		const char *proc_desc);
void hg_malloc_destroy(struct hg_block *hb);

/* re-sync per-process state after fork(): see hg_arena.c for why the
 * inherited private free-stack/bump state must be discarded, not kept or
 * donated (ported from cachedb_perf's pcache_arena_child_init reasoning) */
void hg_malloc_child_init(struct hg_block *hb);

/* sizes the DBG_MALLOC allocation-history pool (shm_hist / struct_hist),
 * same purpose and shape as fm_get_dbg_pool_size() - a HG_CELL_HDR-based
 * estimate substituted for f_malloc's FRAG_OVERHEAD. Best-effort: under- or
 * over-estimating just wastes a bit of the -m reservation or fails a chunk
 * carve loudly (logged, not corrupting) - no correctness risk either way,
 * since chunks are carved dynamically rather than packed into one fixed
 * region the way f_malloc's frag allocator is. */
unsigned long hg_get_dbg_pool_size(unsigned int hist_size);

#ifdef DBG_MALLOC
void *hg_malloc(struct hg_block *hb, unsigned long size,
                const char *file, const char *func, unsigned int line);
void hg_free(struct hg_block *hb, void *p, const char *file,
             const char *func, unsigned int line);
void *hg_realloc(struct hg_block *hb, void *p, unsigned long size,
                 const char *file, const char *func, unsigned int line);
#ifndef INLINE_ALLOC
void *hg_malloc_dbg(struct hg_block *hb, unsigned long size,
                    const char *file, const char *func, unsigned int line);
void hg_free_dbg(struct hg_block *hb, void *p, const char *file,
                 const char *func, unsigned int line);
void *hg_realloc_dbg(struct hg_block *hb, void *p, unsigned long size,
                     const char *file, const char *func, unsigned int line);
#endif
#else
void *hg_malloc(struct hg_block *hb, unsigned long size);
void hg_free(struct hg_block *hb, void *p);
void *hg_realloc(struct hg_block *hb, void *p, unsigned long size);
#endif

void hg_status(struct hg_block *hb);
#if !defined INLINE_ALLOC && defined DBG_MALLOC
void hg_status_dbg(struct hg_block *hb);
#endif
void hg_info(struct hg_block *hb, struct mem_info *info);

/* defined in hg_arena.c; table lookup, no lock needed (immutable) */
unsigned int hg_cell_total_size(unsigned char cls);

/* defined in hg_large.c; @frag is HG_HDR(p) - HG_LFRAG_HDR (the struct
 * hg_lfrag* at the very start of the block). Declared here taking an
 * opaque pointer rather than struct hg_lfrag* to avoid hg_malloc.h needing
 * to depend on hg_large.h, which itself includes hg_malloc.h. */
unsigned long hg_large_frag_size_at(const void *frag);

/*
 * Arena ownership tests.
 *
 * Everything hg hands out lives inside some block's hb->hbase reservation,
 * whose bounds are fixed at init. A pointer from outside it - a foreign
 * arena, a stale pointer, or a corrupted one - has a "class byte" at
 * HG_HDR(p) that is either garbage or, worse, unmapped, so simply reading it
 * faults. That is not hypothetical: it took a process down (staging RGS,
 * 2026-08-09, si_addr == p-32 inside hg_frag_size()).
 *
 * Two flavours, because the callers differ:
 *
 *   hg_owns()     - for code that already knows which block it is working on.
 *                   Exact, one range, no loop.
 *
 *   hg_owns_any() - for code that does NOT. hg_frag_size() is the reason this
 *                   exists: it is installed into the shared
 *                   "unsigned long (*shm_frag_size)(void *)" function pointer
 *                   next to fm_/qm_/hp_/parallel_frag_size(), so its signature
 *                   belongs to an interface we do not own and cannot grow an
 *                   hb parameter. It walks a registry of live arenas instead.
 *
 *                   This IS on the free fast path, contrary to what this
 *                   comment used to claim: _shm_free() calls shm_frag_size()
 *                   unconditionally on every free (mem/shm_mem.h), so every
 *                   shm_free walks the registry. Measured anyway, three
 *                   alternating pairs at 800 cps on the bench harness:
 *                   2.747% allocator self-time without the checks, 2.807%
 *                   with, against a within-arm spread of ~0.2pp. The cost is
 *                   below the noise floor, so the registry stays a plain
 *                   linear walk - a cache here would be unmeasurable
 *                   complexity. Revisit only if HG_ARENA_REG_MAX grows.
 *
 * Both are advisory: they turn "dereference and die" into "decline and carry
 * on". They do not make a bad pointer good.
 */
static inline int hg_owns(struct hg_block *hb, void *cell_start)
{
	return (char *)cell_start >= hb->hbase &&
	       (char *)cell_start < hb->hbase + hb->hsize;
}

#define HG_ARENA_REG_MAX 8
struct hg_arena_range {
	char             *base;
	unsigned long     size;
	struct hg_block  *hb;
};
/* defined in hg_malloc.c; maintained by hg_malloc_init/destroy. Process-local
 * on purpose - a forked child inherits the parent's entries (its shm mapping
 * really is the same memory) and adds its own private pkg arena on top. */
extern struct hg_arena_range hg_arena_reg[HG_ARENA_REG_MAX];

/* count of frees redirected to their true owner; reported by hg_stats */
extern unsigned long hg_xarena_frees;

/*
 * Which arena does this pointer belong to, if any?
 *
 * A child does NOT only ever free pointers from its current arena, and it is
 * not a bug when it does not. Every other allocator lets a child inherit the
 * parent's pkg arena COW, so freeing something the parent allocated pre-fork
 * is ordinary, supported behaviour - cachedb_redis and six sibling modules do
 * exactly that in child_init(), releasing the URL list mod_init() built.
 * HG_MALLOC hands each child a fresh arena instead (see pt.c), which broke
 * that assumption: those frees arrive addressed to an arena that never issued
 * them.
 *
 * So resolve the owner rather than judging by the caller's block. A pointer
 * that belongs to some other live arena is redirected there and freed
 * properly; only a pointer that belongs to NO arena is a real defect.
 */
static inline struct hg_block *hg_owner(const void *p)
{
	int i;

	for (i = 0; i < HG_ARENA_REG_MAX; i++) {
		if (!hg_arena_reg[i].base)
			continue;
		if ((const char *)p >= hg_arena_reg[i].base &&
		    (const char *)p <  hg_arena_reg[i].base + hg_arena_reg[i].size)
			return hg_arena_reg[i].hb;
	}
	return NULL;
}

static inline int hg_owns_any(const void *p)
{
	return hg_owner(p) != NULL;
}

static inline unsigned long hg_frag_size(void *p)
{
	unsigned char c;

	if (!p)
		return 0;

	/* the header may not be mapped at all - check before reading it.
	 * Returning 0 matches the "unknown size" answer this function already
	 * gives for an out-of-range class. */
	if (!hg_owns_any(HG_HDR(p)))
		return 0;

	c = HG_CLASS(p);
	if (c == HG_LARGE_MARKER)
		return hg_large_frag_size_at(HG_HDR(p) - HG_LFRAG_HDR_SIZE);
	if (c >= HG_NCLASSES)
		return 0;

	return hg_cell_total_size(c);
}

#define HG_FRAG_OVERHEAD (HG_CELL_HDR)

#ifdef SHM_EXTRA_STATS
void hg_stats_core_init(struct hg_block *hb, int core_index);
unsigned long hg_stats_get_index(void *ptr);
void hg_stats_set_index(void *ptr, unsigned long idx);

#ifdef DBG_MALLOC
static inline const char *hg_frag_file(void *p)
{
	if (!hg_owns_any(HG_HDR(p)))
		return NULL;
	return *(const char **)(HG_HDR(p) + HG_ROUNDTO);
}
static inline const char *hg_frag_func(void *p)
{
	if (!hg_owns_any(HG_HDR(p)))
		return NULL;
	return *(const char **)(HG_HDR(p) + HG_ROUNDTO * 2);
}
static inline unsigned long hg_frag_line(void *p)
{
	if (!hg_owns_any(HG_HDR(p)))
		return 0;
	return *(unsigned long *)(HG_HDR(p) + HG_ROUNDTO * 3);
}
#else
static inline const char *hg_frag_file(void *p) { return NULL; }
static inline const char *hg_frag_func(void *p) { return NULL; }
static inline unsigned long hg_frag_line(void *p) { return 0; }
#endif
#endif

/*
 * Fast-path stats helpers.
 *
 * hg_pstat_mine() picks this process's slot. process_no is -1 in the
 * attendant and 0 in the main process before fork, so it is biased by one
 * and wrapped: two processes sharing a slot would only reintroduce the
 * lost-update race for those two, never corrupt anything, and with
 * HG_STAT_SLOTS slots that needs a genuinely enormous process table.
 */
static inline struct hg_pstat *hg_pstat_mine(struct hg_block *hb)
{
	return &hb->pstat[((unsigned int)(process_no + 1)) % HG_STAT_SLOTS];
}

/* summed on read; clamped at 0 because individual slots go negative when
 * one process frees another's cells and a torn sum could otherwise
 * underflow an unsigned return */
static inline unsigned long hg_used(struct hg_block *hb)
{
	long total = 0;
	int i;

	for (i = 0; i < HG_STAT_SLOTS; i++)
		total += hb->pstat[i].used;
	return total < 0 ? 0 : (unsigned long)total;
}

static inline unsigned long hg_fragments(struct hg_block *hb)
{
	long total = 0;
	int i;

	for (i = 0; i < HG_STAT_SLOTS; i++)
		total += hb->pstat[i].fragments;
	return total < 0 ? 0 : (unsigned long)total;
}

/* carved-but-idle cell capacity; defined in hg_arena.c (declared here too,
 * since hg_arena.h includes THIS header and cannot be included back) */
unsigned long hg_slab_recycled(struct hg_block *hb);

/*
 * Register HG_MALLOC's SHM arena statistics with the statistics collector, so
 * the allocator's own state is scrapeable and not only reachable through the
 * hg_stats MI command.  Called from init_stats_collector(); a no-op unless
 * HG_MALLOC is the shm allocator actually in use.  Returns 0 on success.
 *
 * SHM ONLY, deliberately.  Every allocator (qm/fm/hp/f_parallel/hg) reports
 * through the same 7-field struct mem_info, and the core already turns that
 * into shmem: plus per-process pkmem: and proc_ statistics - so HG's pkg memory
 * is ALREADY visible by that generic route.  No allocator exposes
 * allocator-specific per-process statistics, and doing so would mean extending
 * the signal_pkg_status()/pkg_status[][] mechanism, since a pkg arena is
 * private memory that no other process can read.  The shm arena has no such
 * problem: there is exactly one, and it is shared.
 */
#ifdef HG_MALLOC
int hg_register_stats(void);
#else
/* No-op without the allocator, so callers (statistics.c) need no #ifdef of
 * their own. Building with -DHG_MALLOC removed previously failed at link with
 * an undefined reference here - the definition lives in mem/hg_arena.c, which
 * is entirely inside "#ifdef HG_MALLOC". */
static inline int hg_register_stats(void) { return 0; }
#endif

/* total cell-slot bytes handed out across every process */
static inline unsigned long hg_cell_live(struct hg_block *hb)
{
	long total = 0;
	int i;

	for (i = 0; i < HG_STAT_SLOTS; i++)
		total += hb->pstat[i].cell_live;
	return total < 0 ? 0 : (unsigned long)total;
}

#ifdef STATISTICS
static inline unsigned long hg_get_size(struct hg_block *hb)
{
	return hb->size;
}
static inline unsigned long hg_get_used(struct hg_block *hb)
{
	return hg_used(hb);
}
/* These feed the shmem: and pkgmem: statistics (the SHM_GET_ and PKG_GET_
 * macros in shm_mem.h and mem.h) - a DIFFERENT path from hg_info(), so the
 * recycled
 * subtraction has to be applied here too or the published statistics still
 * report the raw carve footprint. */
static inline unsigned long hg_get_free(struct hg_block *hb)
{
	/* Headroom left to CARVE - deliberately NOT size minus the live figure.
	 * Carved-but-recycled cells are reusable only within their own size
	 * class, so counting them as free reports ~95% available right up until
	 * an allocation of a DIFFERENT size fails with "no more HG_MALLOC arena
	 * memory". hb->real_used is the carve footprint, so this is the number
	 * that actually predicts that failure.
	 *
	 * Consequence: free + real_used != size here (real_used is live), unlike
	 * q_malloc. The invariant that does hold is free + carve == size. Each
	 * figure answers a different question: used = live payload, real_used /
	 * max_used = live commitment and its peak, free = room left to carve. */
	return hb->size - hb->real_used;
}
/* The large tier's counterpart to hg_slab_recycled(): backing held from the
 * buddy grid that is not currently handed out as a fragment.  O(1), unlike the
 * slab version, because both terms are maintained under hb->lock as the
 * fragments come and go. */
static inline unsigned long hg_large_recycled(struct hg_block *hb)
{
	return hb->large_backing > hb->large_live ?
	       hb->large_backing - hb->large_live : 0;
}
static inline unsigned long hg_get_real_used(struct hg_block *hb)
{
	/* real_used is now the true carve footprint of BOTH tiers - slab blocks
	 * plus whole large chunks - so both tiers' idle-but-held bytes have to
	 * come back out to leave what is genuinely handed out. */
	unsigned long recycled = hg_slab_recycled(hb) + hg_large_recycled(hb);
	unsigned long live = hb->real_used > recycled ?
	                     hb->real_used - recycled : 0;

	if (live > hb->max_live_used)
		hb->max_live_used = live;
	return live;
}
static inline unsigned long hg_get_max_real_used(struct hg_block *hb)
{
	/* refresh the mark first, so reading max on its own is not stale */
	(void)hg_get_real_used(hb);
	return hb->max_live_used;
}
static inline unsigned long hg_get_frags(struct hg_block *hb)
{
	return hg_fragments(hb);
}
#endif /* STATISTICS */

#endif /* hg_malloc_h */
