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

struct hg_chunk {
	struct hg_chunk *next;    /* global registry, append-only */
	unsigned int cls;         /* immutable */
	unsigned int cell_size;   /* total slot size, header included */
	unsigned int cells;
} __attribute__ ((aligned (64)));

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
	struct hg_region *regions;
	void *gpool[HG_NCLASSES];       /* global free cells, per class */
	unsigned int gpool_n[HG_NCLASSES];
	unsigned long lo, hi;           /* extent watermarks */

	/* large-object tier (hg_large.c): list of independently-carved
	 * chunks, each an f_malloc-style boundary-tag heap, sharing ONE free
	 * list across all of them; hb->lock guards all of it (slow path
	 * already, no separate lock needed) */
	struct hg_large_chunk *large_chunks;
	struct hg_lfrag *large_free;

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

	unsigned char size2class[(HG_CELL_MAX / HG_ROUNDTO) + 1];
} __attribute__ ((aligned (HG_ROUNDTO)));

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
int hg_register_stats(void);

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
static inline unsigned long hg_get_real_used(struct hg_block *hb)
{
	unsigned long recycled = hg_slab_recycled(hb);
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
