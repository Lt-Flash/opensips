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

#ifdef HG_MALLOC

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include "hg_version.h"
#include "hg_malloc.h"
#include "hg_arena.h"
#include "hg_large.h"
#include "../dprint.h"
#include "../globals.h"
#include "../statistics.h"

#ifdef DBG_MALLOC
#include "mem_dbg_hash.h"
#endif

#include "../lib/dbg/struct_hist.h"

/*
 * The huge-page ladder is Linux-only, and the fallback values below are
 * Linux's. They are defined ONLY under __OS_linux on purpose: the same bit
 * means something else elsewhere (0x40000 is MAP_PREFAULT_READ on FreeBSD),
 * so defining them unconditionally would make mmap() succeed with unrelated
 * semantics and this code would then report a huge-page tier it never got.
 * On every other OS the reservation is a plain anonymous mapping.
 */
#ifdef __OS_linux
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#endif /* __OS_linux */

/*
 * The system's default huge page size, probed once rather than assumed.
 *
 * It is 2M on x86_64 and on arm64 with 4K base pages, but 32M on arm64
 * with 16K pages and 512M with 64K pages. Getting it wrong is not
 * cosmetic: mmap(MAP_HUGETLB) without MAP_HUGE_* bits uses the system
 * default, and the kernel rounds the mapping up to it - so a hardcoded
 * 2M would (a) leave hsize describing a smaller region than the VMA,
 * making the matching munmap() fail with EINVAL and leak the arena, and
 * (b) align the THP tiers to 2M instead of the real PMD granularity, so
 * MADV_HUGEPAGE/MADV_COLLAPSE quietly do nothing and every arena silently
 * degrades to 4K while still reporting a huge-page tier.
 *
 * Falls back to 2M only if /proc/meminfo cannot be read at all.
 */
#define HG_HPS_FALLBACK (2UL * 1024 * 1024)

static unsigned long hg_hps_cached;

static unsigned long hg_hps(void)
{
	FILE *f;
	char line[256];
	unsigned long kb = 0;

	if (hg_hps_cached)
		return hg_hps_cached;

	f = fopen("/proc/meminfo", "r");
	if (f) {
		while (fgets(line, sizeof line, f)) {
			if (!strncmp(line, "Hugepagesize:", 13)) {
				kb = strtoul(line + 13, NULL, 10);
				break;
			}
		}
		fclose(f);
	}

	/* must be a power of two for the alignment masks below to work */
	if (kb == 0 || (kb * 1024UL) & ((kb * 1024UL) - 1))
		hg_hps_cached = HG_HPS_FALLBACK;
	else
		hg_hps_cached = kb * 1024UL;

	return hg_hps_cached;
}

#define HG_HPS (hg_hps())

/* round @s up to a whole number of huge pages. Used for the reservation
 * length, the alignment of the THP tiers, AND the matching munmap length -
 * they must agree exactly or the unmap fails and the arena leaks, so they
 * all go through this one macro rather than repeating the expression. */
#define HG_HPS_ROUND(s) (((s) + HG_HPS - 1) & ~(HG_HPS - 1))

/* HG_ROUNDTO=2^k so the following works (same trick as f_malloc.c) */
#define ROUNDTO_MASK   (~((unsigned long)HG_ROUNDTO-1))
#define ROUNDUP_TO(s)  (((s)+(HG_ROUNDTO-1))&ROUNDTO_MASK)

/*
 * Tier ladder + verification, ported near-verbatim from cachedb_perf's
 * pcache_mem.c pcache_mem_reserve() (DESIGN 2.6.1/2.6.2 there): every tier
 * is proven by TRYING it and verifying the result through /proc, never
 * inferred from kernel version or sysfs config alone.
 */

static long hg_read_shmem_huge_kb(void)
{
	FILE *f;
	char line[256];
	long kb = -1;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "ShmemHugePages:", 15)) {
			kb = strtol(line + 15, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb;
}

/* is the huge-page-sized range starting at @addr PMD-mapped here? */
static int hg_range_is_huge(unsigned long addr)
{
	FILE *f;
	char line[256], *p;
	unsigned long start, end, kb;
	int in_range = 0, huge = 0;

	f = fopen("/proc/self/smaps", "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%lx-%lx ", &start, &end) == 2) {
			in_range = (start <= addr && addr < end);
			continue;
		}
		if (!in_range)
			continue;
		if (!strncmp(line, "AnonHugePages:", 14) ||
		        !strncmp(line, "ShmemPmdMapped:", 15) ||
		        !strncmp(line, "FilePmdMapped:", 14)) {
			p = strchr(line, ':');
			kb = strtoul(p + 1, NULL, 10);
			if (kb >= HG_HPS / 1024) {
				huge = 1;
				break;
			}
		}
	}

	fclose(f);
	return huge;
}

/*
 * Keep the arena out of core dumps - unless someone is trying to debug the
 * allocator, in which case the arena is the only thing worth having.
 *
 * HG_MALLOC pre-faults and mlocks its whole reservation, so unlike the
 * lazily-faulted F_MALLOC/Q_MALLOC pools every page is resident - a
 * crashing worker would otherwise write the ENTIRE arena (-m plus -M, and
 * the shm_memlog_size-derived debug pool on top) into its core file. With
 * one core per worker that is multi-GB of core dumps per crash, which is
 * enough page-cache churn to push a busy box into reclaim, and it buries
 * the actually-useful stack/heap in gigabytes of allocator slab.
 *
 * The cost of that default only became clear when a core was actually needed:
 * VM_DONTDUMP wins over coredump_filter, so no filter setting can bring the
 * arena back, and every core taken during the 2026-08 crash investigation had
 * "Cannot access memory" where shm_block should be. The free lists, the class
 * counters, the cell headers - the entire state that decides whether a crash
 * was corruption or a race - are all inside the region being skipped.
 *
 * So it is opt-in: set HG_DUMP_ARENA=1 in the environment (a systemd
 * Environment= line is enough) and the arena is dumped. Sized deliberately as
 * an environment variable rather than a config parameter, because it must take
 * effect during allocator init, long before the config file is parsed.
 *
 * Best-effort: MADV_DONTDUMP is Linux 3.4+, and failure is harmless
 * (bigger cores, nothing incorrect), so the return value is ignored.
 */
static void hg_exclude_from_core(void *base, unsigned long size)
{
#ifdef MADV_DONTDUMP
	const char *want = getenv("HG_DUMP_ARENA");

	if (want && *want && *want != '0') {
		/* explicit, not merely "leave the default alone" - the mapping
		 * may have inherited VM_DONTDUMP from a previous madvise on an
		 * overlapping range */
		madvise(base, size, MADV_DODUMP);
		return;
	}

	madvise(base, size, MADV_DONTDUMP);
#endif
}

/*
 * Reserve a huge-page-aligned, huge-page-backed (best effort) region of at least
 * @size bytes, mlock-pinned against swap. Never unmapped until
 * hg_malloc_destroy(). Returns NULL on total mmap failure only - a huge-page
 * miss still returns a valid plain-4K mapping (degrade, don't fail), per
 * hg_mem_tier_str()'s HG_MEM_4K case.
 *
 * @shared picks MAP_SHARED vs MAP_PRIVATE, and it is NOT cosmetic:
 *
 *   shm  -> MAP_SHARED:  one arena visible to every forked worker, which is
 *                        the entire point of shm.
 *   pkg  -> MAP_PRIVATE: every worker must get its OWN copy-on-write arena
 *                        after fork. Mapping the pkg arena MAP_SHARED (as
 *                        this function originally did unconditionally) put
 *                        the pkg hg_block - including its embedded
 *                        gen_lock_t and its per-class gpool free lists - in
 *                        memory shared by all workers. Under FAST_LOCK that
 *                        lock is a *spinlock*, so every worker's pkg
 *                        allocations serialized on one contended spinlock,
 *                        and "process-private" pkg cells silently migrated
 *                        between processes through the shared gpool.
 */
static void *hg_mem_reserve(unsigned long size, unsigned long *cap,
		enum hg_mem_tier *tier, unsigned long *locked_mb, int shared)
{
	unsigned long asize = HG_HPS_ROUND(size);
	unsigned long csize = HG_HPS_ROUND(*cap < size ? size : *cap);
	int vis = shared ? MAP_SHARED : MAP_PRIVATE;
	char *resv, *base;
	long shmem_kb;
	void *p;

	*locked_mb = 0;
	*tier = HG_MEM_4K;
	*cap = csize;        /* rewritten below if a fallback shrinks it */

#ifndef __OS_linux
	/* No verified huge-page route outside Linux: take a plain anonymous
	 * mapping and report the 4K tier honestly rather than claiming one we
	 * cannot check. Still pinned and pre-faulted. */
	p = mmap(NULL, csize, PROT_READ|PROT_WRITE, vis|MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	hg_exclude_from_core(p, csize);
	if (mlock(p, asize) == 0)
		*locked_mb = asize >> 20;
	else
		memset(p, 0, asize);
	return p;
#else

	/*
	 * tier 1: MAP_HUGETLB - unswappable, exempt from RLIMIT_MEMLOCK.
	 *
	 * Try the whole cap first, then fall back to the committed size alone.
	 * hugetlb mappings are backed by a fixed pool, so a cap larger than the
	 * pool can hold makes this mmap fail outright - and silently dropping
	 * to THP because the admin asked for growth room would be a far worse
	 * trade than simply not being able to grow. A cap-less tier-1 arena is
	 * what v2 shipped; losing the tier is a real regression.
	 */
	p = mmap(NULL, csize, PROT_READ|PROT_WRITE,
	         vis|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
	if (p == MAP_FAILED && csize > asize) {
		p = mmap(NULL, asize, PROT_READ|PROT_WRITE,
		         vis|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
		if (p != MAP_FAILED) {
			LM_NOTICE("hugetlb pool cannot back a %lu MB cap; "
				"reserving the %lu MB in use instead - the arena "
				"keeps huge pages but cannot grow. Raise "
				"vm.nr_hugepages to allow growth.\n",
				csize >> 20, asize >> 20);
			*cap = asize;      /* the arena is fixed after all */
		}
		/* on total failure csize stays at the full cap for tiers 2-4:
		 * THP reservations are plain VA, which CAN hold the cap */
	}
	if (p != MAP_FAILED) {
		hg_exclude_from_core(p, *cap);
		memset(p, 0, asize);
		*tier = HG_MEM_HUGETLB;
		*locked_mb = asize >> 20;
		return p;
	}

	/*
	 * tiers 2-4: huge-page-aligned anon mapping. For the shmem
	 * (MAP_SHARED) case the VA and shmem *file offset* must be congruent
	 * modulo the huge page size for THP eligibility, so reserve PROT_NONE
	 * first, then MAP_FIXED the real mapping at a huge-page boundary
	 * inside it - an atomic replace, no race with other mappings.
	 * Harmless (and keeps the alignment) for MAP_PRIVATE.
	 *
	 * The real mapping covers the whole CAP, readable and writable, even
	 * though only asize of it is committed now. That is the load-bearing
	 * part of v3 growth, not an accident: this mapping is created before
	 * fork, so it is the one VMA every worker inherits, all of them backed
	 * by the same shmem object. Growing later means faulting more of that
	 * object in - visible to every process by construction. The obvious
	 * alternative - keep the tail PROT_NONE and mmap/mprotect it live at
	 * grow time - changes only the GROWER's page tables: measured on the
	 * 5.4 kernel, the grower reads its new pages fine and a forked worker
	 * SIGSEGVs on the same addresses (scratchpad rig vatest.c, test A vs
	 * B). An untouched R/W tail costs a few hundred kB of page-table
	 * entries, not memory - test B: 64 MB of mapped-untouched span held
	 * RSS at 576 kB.
	 */
	resv = mmap(NULL, csize + HG_HPS, PROT_NONE,
	            MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (resv == MAP_FAILED)
		return NULL;
	base = (char *)(((unsigned long)resv + HG_HPS - 1) & ~(HG_HPS - 1));
	p = mmap(base, csize, PROT_READ|PROT_WRITE,
	         vis|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
	if (p == MAP_FAILED) {
		munmap(resv, csize + HG_HPS);
		return NULL;
	}

	hg_exclude_from_core(base, csize);

	/* advise huge before first touch (tier 2), then pin+populate: a cold
	 * mlock populates to pin, so it doubles as the pre-fault. The advice
	 * covers the whole cap so growth deltas inherit it - each delta still
	 * gets its backing VERIFIED at grow time, never assumed from here. */
	madvise(base, csize, MADV_HUGEPAGE);
	shmem_kb = hg_read_shmem_huge_kb();
	if (mlock(base, asize) == 0) {
		*locked_mb = asize >> 20;
	} else {
		LM_WARN("mlock of the %lu MB HG_MALLOC arena failed (%s): "
			"continuing unpinned (swappable). If running under "
			"systemd, add LimitMEMLOCK=infinity to the unit.\n",
			asize >> 20, strerror(errno));
		memset(base, 0, asize);        /* still pre-fault */
	}

	if (hg_range_is_huge((unsigned long)base)) {
		*tier = HG_MEM_THP_ADVISE;
	} else if (shmem_kb >= 0 &&
	           madvise(base, asize, MADV_COLLAPSE) == 0 &&
	           hg_read_shmem_huge_kb() - shmem_kb >= (long)(asize / 1024)) {
		*tier = HG_MEM_THP_COLLAPSE;
	} else {
		*tier = HG_MEM_4K;         /* reserved+pinned but 4K */
	}
	return base;
#endif /* __OS_linux */
}

/* v3 admin caps - see the declaration comment in hg_malloc.h. 0 = fixed. */
unsigned long hg_shm_cap_bytes;
unsigned long hg_pkg_cap_bytes;

/*
 * Commit [hbase+off, +delta) of the reservation: populate, pin, verify the
 * achieved backing. The range is already mapped R/W (the whole cap is, since
 * reserve time - that is what makes the commit visible to every forked
 * worker with no page-table surgery here), so the only work is faulting the
 * pages in and finding out what the kernel faulted them in AS.
 *
 * mlock() is the commit primitive for every tier, chosen for one property:
 * it populates the exact range and reports failure through errno instead of
 * raising SIGBUS in whichever worker touches the shortfall later. A grow
 * that cannot be backed must fail HERE, atomically, while the buddy still
 * considers the range nonexistent.
 *   - tiers 2-4: mlock is also the pin, same as init.
 *   - tier 1: hugetlb pages are unswappable regardless; mlock is used only
 *     as the populate-with-clean-errno vehicle. The pool-exhaustion path
 *     (mlock ENOMEM, nothing SIGBUSes, VM_LOCKED rolled back) is PROVEN by
 *     the hgstress grow harness against a deliberately undersized pool -
 *     do not take this comment's word for it, run the harness.
 *
 * Returns the achieved hg_mem_tier of the delta, or -1 with the range
 * munlock'd again (refuse, never half-commit). No hg_exclude_from_core()
 * here: reserve time already excluded the whole cap.
 */
int hg_mem_commit(struct hg_block *hb, unsigned long off, unsigned long delta)
{
	char *base = hb->hbase + off;

	if (off + delta > hb->hcap) {
		LM_BUG("%s: commit of %lu@%lu overruns the %lu byte cap\n",
			hb->name, delta, off, hb->hcap);
		return -1;
	}

	if (hb->tier == HG_MEM_HUGETLB) {
		if (mlock(base, delta) != 0) {
			/* once per episode - see grow_refuse_said's comment */
			if (!hb->grow_refuse_said) {
				hb->grow_refuse_said = 1;
				LM_WARN("%s: cannot grow by %lu MB: the hugetlb "
					"pool is exhausted (%s). Raise "
					"vm.nr_hugepages.\n",
					hb->name, delta >> 20, strerror(errno));
			}
			munlock(base, delta);
			return -1;
		}
		hb->locked_mb += delta >> 20;
		return HG_MEM_HUGETLB;
	}

#ifdef __OS_linux
	{
		long shmem_kb = hg_read_shmem_huge_kb();

		/* re-advise the delta: cheap, and correct even though reserve
		 * time advised the whole cap - a later madvise elsewhere in the
		 * VMA may have split it */
		madvise(base, delta, MADV_HUGEPAGE);

		if (mlock(base, delta) != 0) {
			/* once per episode - see grow_refuse_said's comment */
			if (!hb->grow_refuse_said) {
				hb->grow_refuse_said = 1;
				LM_WARN("%s: cannot grow by %lu MB: mlock failed "
					"(%s). If running under systemd, add "
					"LimitMEMLOCK=infinity to the unit.\n",
					hb->name, delta >> 20, strerror(errno));
			}
			munlock(base, delta);
			return -1;
		}
		hb->locked_mb += delta >> 20;

		/*
		 * The delta's backing is a fresh negotiation - the arena's init
		 * tier says NOTHING about what this range just got. Verify it
		 * the same way init does: read what the kernel actually did.
		 */
		if (hg_range_is_huge((unsigned long)base))
			return HG_MEM_THP_ADVISE;
		if (shmem_kb >= 0 &&
		    madvise(base, delta, MADV_COLLAPSE) == 0 &&
		    hg_read_shmem_huge_kb() - shmem_kb >= (long)(delta / 1024))
			return HG_MEM_THP_COLLAPSE;
		return HG_MEM_4K;
	}
#else
	if (mlock(base, delta) != 0) {
		memset(base, 0, delta);        /* still pre-fault */
	} else {
		hb->locked_mb += delta >> 20;
	}
	return HG_MEM_4K;
#endif
}

const char *hg_mem_tier_str(enum hg_mem_tier tier)
{
	switch (tier) {
	case HG_MEM_HUGETLB:
		return "MAP_HUGETLB 2M pages";
	case HG_MEM_THP_ADVISE:
		return "THP 2M pages via MADV_HUGEPAGE (huge at fault)";
	case HG_MEM_THP_COLLAPSE:
		return "THP 2M pages via MADV_COLLAPSE (post-fill retrofit)";
	case HG_MEM_4K:
		return "plain 4K pages";
	}
	return "unknown";
}

/*
 * Registry of live arena ranges, for the ownership tests in hg_malloc.h.
 *
 * hg_owns_any() needs to answer "is this pointer from ANY of our arenas?"
 * without being handed a block, because hg_frag_size() is installed into a
 * shared function-pointer interface whose signature we do not control. A
 * fixed-size array is deliberate: this is bootstrap bookkeeping for the
 * allocator itself, so it must not be allocated THROUGH the allocator.
 *
 * Process-local, and correct under fork by construction: a child inherits the
 * parent's entries (its shm mapping is genuinely the same memory) and adds its
 * own private pkg arena when pt.c swaps one in. Keeping the parent's stale pkg
 * entry is a feature here - a parent-allocated pkg pointer freed in a child
 * still resolves to mapped memory, so it is declined rather than dereferenced.
 */
struct hg_arena_range hg_arena_reg[HG_ARENA_REG_MAX];

/*
 * How many frees this process redirected to an arena other than the one the
 * caller named. Not an error count - see hg_owner(). It is expected to be a
 * small constant per child, set at startup and never moving again; a figure
 * that climbs with traffic would mean something is handing pointers across
 * arenas at runtime, which nothing should.
 */
unsigned long hg_xarena_frees;

static void hg_arena_reg_add(struct hg_block *hb)
{
	int i;

	for (i = 0; i < HG_ARENA_REG_MAX; i++) {
		if (!hg_arena_reg[i].base) {
			hg_arena_reg[i].base = hb->hbase;
			/* the CAP, not hsize: growth must not invalidate the
			 * registry entry, or a pointer into grown space would be
			 * misread as foreign and "routed" to another arena. The
			 * whole cap's VA belongs to this arena from reserve time;
			 * uncommitted ranges cannot hold live cells, so the wider
			 * range cannot misattribute anything that exists. */
			hg_arena_reg[i].size = hb->hcap;
			hg_arena_reg[i].hb   = hb;
			return;
		}
	}
	/* Not fatal: hg_owns_any() then declines pointers it cannot vouch for,
	 * which costs diagnostics, never correctness. */
	LM_WARN("%s: more than %d live HG_MALLOC arenas in one process - "
		"ownership checks will be incomplete\n", hb->name,
		HG_ARENA_REG_MAX);
}

static void hg_arena_reg_del(struct hg_block *hb)
{
	int i;

	for (i = 0; i < HG_ARENA_REG_MAX; i++) {
		if (hg_arena_reg[i].base == hb->hbase) {
			hg_arena_reg[i].base = NULL;
			hg_arena_reg[i].size = 0;
			hg_arena_reg[i].hb   = NULL;
			return;
		}
	}
}

/*
 * hg_malloc_init() reserves its own memory (unlike fm_malloc_init(), which
 * receives an already-mmap'd address from shm_getmem()) and lays the block
 * control structure out at the very start of that reservation - the same
 * "control struct lives inside the memory it manages" pattern fm_block/
 * hp_block use, chosen specifically so HG_MALLOC never needs to call
 * shm_malloc()/pkg_malloc() on itself to bootstrap its own bookkeeping
 * (it cannot: HG_MALLOC IS what those macros dispatch to when selected).
 */
struct hg_block *hg_malloc_init(unsigned long size, char *name, int shared,
		const char *proc_desc)
{
	enum hg_mem_tier tier;
	unsigned long locked_mb;
	unsigned long cap;
	char *base;
	struct hg_block *hb;

	/* v3: the reservation may exceed the committed size, by admin cap.
	 * cap comes back as what was actually achieved (a hugetlb pool that
	 * cannot hold the cap degrades to a fixed arena, not to no arena).
	 * By NAME, not by @shared: shm_dbg is also shared but is a fixed-size
	 * diagnostic pool computed by hg_get_dbg_pool_size() - handing it the
	 * shm cap would reserve gigabytes of VA for a pool that must never
	 * grow past its formula.
	 *
	 * The HG_*_CAP_MB environment fallback is SCAFFOLDING for the v3
	 * mechanism work: the real config surface (auto_scaling_profile
	 * grammar) is a later step, and the growth path needs to be provable
	 * end-to-end before it exists. The globals stay authoritative - the
	 * env is consulted only while they are unset - so wiring the config
	 * up later removes the fallback's reach without touching this code. */
	if (!strcmp(name, "shm")) {
		cap = hg_shm_cap_bytes;
		if (!cap && getenv("HG_SHM_CAP_MB"))
			cap = strtoul(getenv("HG_SHM_CAP_MB"), NULL, 10) << 20;
	} else if (!strcmp(name, "pkg")) {
		cap = hg_pkg_cap_bytes;
		if (!cap && getenv("HG_PKG_CAP_MB"))
			cap = strtoul(getenv("HG_PKG_CAP_MB"), NULL, 10) << 20;
	} else {
		cap = 0;
	}
	base = hg_mem_reserve(size, &cap, &tier, &locked_mb, shared);
	if (!base) {
		LM_ERR("failed to reserve %lu bytes for %s HG_MALLOC arena\n",
			size, name);
		return NULL;
	}

	/* the block header itself lives inside the reservation it describes */
	if (size < ROUNDUP_TO(sizeof(struct hg_block))) {
		LM_ERR("%s arena of %lu bytes too small for the block header "
			"(%zu bytes)\n", name, size, sizeof(struct hg_block));
		munmap(base, cap);
		return NULL;
	}

	hb = (struct hg_block *)(void *)base;
	memset(hb, 0, sizeof *hb);
	hb->name = name;
	hb->size = size;
	hb->lo = ~0UL;
	hb->hbase = base;
	hb->hsize = HG_HPS_ROUND(size);
	hb->hcap = cap;
	/* one committed-size step per grow: big enough that a growth spurt is
	 * a handful of commits, small enough that the pre-fault under the
	 * arena lock stays bounded. Overridable by config later. */
	hb->grow_granule = HG_HPS_ROUND(16UL << 20);
	/* hg_hps() is private to this file, and hg_arena_init() needs the probed
	 * value to lay out the page grid - hand it over rather than re-probing */
	hb->hps = HG_HPS;
	hb->tier = tier;
	hb->locked_mb = locked_mb;
	hb->tier_bytes[tier] = hb->hsize;

	if (!lock_init(&hb->lock)) {
		LM_ERR("failed to init the %s arena lock\n", name);
		munmap(base, hb->hcap);
		return NULL;
	}

	hg_arena_reg_add(hb);

	/* the region right after the block header is the first thing chunks
	 * bump-carve from - hg_arena_init() sets hoff past it */
	if (hg_arena_init(hb, ROUNDUP_TO(sizeof(struct hg_block))) < 0) {
		LM_ERR("failed to init the %s arena\n", name);
		lock_destroy(&hb->lock);
		munmap(base, hb->hcap);
		return NULL;
	}

	/* "pinned from swapping" is the real guarantee this reports: tier-1
	 * MAP_HUGETLB pages are non-swappable by construction (no mlock()
	 * needed or taken), tiers 2-4 rely on an explicit mlock() instead -
	 * either way, the reported MB are equally protected against swap,
	 * just via a different mechanism. Plain "pinned" reads ambiguously
	 * (looks like "an mlock() call happened") and was caught live during
	 * a real diagnosis session mid-2026-08-07 being misread that way. */
	if (proc_desc)
		LM_NOTICE("%s " HG_MALLOC_NAME " arena (%s): %lu MB on %s, %lu MB "
			"pinned from swapping\n",
			name, proc_desc, size >> 20, hg_mem_tier_str(tier), locked_mb);
	else
		LM_NOTICE("%s " HG_MALLOC_NAME " arena: %lu MB on %s, %lu MB "
			"pinned from swapping\n",
			name, size >> 20, hg_mem_tier_str(tier), locked_mb);
	if (hb->hcap > hb->hsize)
		LM_NOTICE("%s arena can grow to %lu MB (%lu MB headroom "
			"reserved, uncommitted)\n", name, hb->hcap >> 20,
			(hb->hcap - hb->hsize) >> 20);

	return hb;
}

/* mirrors fm_get_dbg_pool_size()'s structure, HG_CELL_HDR substituted for
 * FRAG_OVERHEAD - see the "why" note on the declaration in hg_malloc.h */
unsigned long hg_get_dbg_pool_size(unsigned int hist_size)
{
	return ROUNDUP_TO(sizeof(struct hg_block)) + HG_CELL_HDR +
		HG_CELL_HDR + 56 /* sizeof(struct struct_hist_list) */ + 2 * hist_size *
		(HG_CELL_HDR + 88 /* sizeof(struct struct_hist) */ +
		HG_CELL_HDR + sizeof(struct struct_hist_action));
}

void hg_malloc_destroy(struct hg_block *hb)
{
	if (!hb)
		return;

	hg_arena_reg_del(hb);
	hg_arena_destroy(hb);
	lock_destroy(&hb->lock);
	/* munmap last: hb itself lives inside hbase. The whole cap, not just
	 * the committed part - the reservation is one mapping */
	munmap(hb->hbase, hb->hcap);
}

void hg_malloc_child_init(struct hg_block *hb)
{
	if (hb)
		hg_arena_child_init(hb);
}

#ifdef SHM_EXTRA_STATS
#include "module_info.h"
unsigned long hg_stats_get_index(void *ptr)
{
	if (!ptr)
		return GROUP_IDX_INVALID;

	return HG_STATS_IDX(ptr);
}

void hg_stats_set_index(void *ptr, unsigned long idx)
{
	if (!ptr)
		return;

	HG_STATS_IDX(ptr) = idx;
}

/* called once, pre-fork, after the statistics engine but before any worker
 * exists yet (matches hp_init_shm_statistics()'s call site) - single
 * process at this point, so walking "not on any known free list" is exact,
 * unlike hg_status_dbg()'s post-fork best-effort walk (see hg_arena.c's
 * hg_arena_walk_live() comment) */
void hg_stats_core_init(struct hg_block *hb, int core_index)
{
	hg_arena_stats_core_init(hb, core_index);
}
#endif

/* fills a malloc info structure with info about the block */
void hg_info(struct hg_block *hb, struct mem_info *info)
{
	unsigned long recycled;

	memset(info, 0, sizeof *info);
	info->total_size = hb->size;
	info->min_frag = 64; /* smallest cell class, see hg_arena.c cell_sizes */
	info->used = hg_used(hb);

	/* Report carved-but-idle cell capacity as FREE rather than USED, so
	 * real_used/free track live demand and fall again when load drops -
	 * the same thing q_malloc/f_malloc do for a fragment sitting on a free
	 * list. hb->real_used on its own is the arena's carve footprint, which
	 * never decreases and would otherwise look like a leak. */
	recycled = hg_slab_recycled(hb);
	info->real_used = hb->real_used > recycled ? hb->real_used - recycled : 0;
	/* room left to CARVE, not size minus live - see hg_get_free() */
	info->free = hb->size - hb->real_used;

	/* the peak of what real_used above actually reached - NOT the peak
	 * carve (hb->max_real_used), which only ever grows and would drift
	 * away from real_used forever */
	if (info->real_used > hb->max_live_used)
		hb->max_live_used = info->real_used;
	info->max_used = hb->max_live_used;
	info->total_frags = hg_fragments(hb);
}

void hg_status(struct hg_block *hb)
{
	unsigned int nchunks;
	unsigned long bytes;

	LM_GEN1(memdump, "hg_status (%p):\n", hb);
	if (!hb)
		return;

	hg_arena_stats(hb, &nchunks, &bytes);
	LM_GEN1(memdump, " heap size= %lu, tier=%s, pinned=%lu MB\n",
		hb->size, hg_mem_tier_str(hb->tier), hb->locked_mb);
	LM_GEN1(memdump, " chunks= %u, chunk bytes= %lu\n", nchunks, bytes);
}

#if !defined INLINE_ALLOC && defined DBG_MALLOC
struct hg_dbg_dump_ctx {
	mem_dbg_htable_t *allocd;
	unsigned long skipped_notlive;   /* see hg_dbg_dump_cb() */
};

static void hg_dbg_dump_cb(void *payload, void *ctx)
{
	struct hg_dbg_dump_ctx *c = ctx;
	char *tag = HG_HDR(payload);
	const char *file, *func;
	unsigned long line;

	/*
	 * The walker derives cell addresses from chunk bookkeeping rather than
	 * from a live-cell list, so a single corrupted chunk hands us an
	 * address that need not be mapped - and this is a DIAGNOSTIC path. A
	 * memory dump must never be the thing that kills the process, which is
	 * exactly what happened on 2026-08-09 before this check existed.
	 */
	if (!hg_owns_any(tag)) {
		hg_corrupt(NULL, HG_C_FOREIGN_PTR);
		LM_CRIT("%s: dump walker produced %p, outside every arena - "
			"skipping it\n", HG_MALLOC_NAME, payload);
		return;
	}

	/*
	 * Read the DBG fields directly from the tag region, like
	 * fm_status_dbg reads f->file/func/line straight off the frag
	 * struct - NOT via hg_frag_file()/func()/line() (hg_malloc.h),
	 * which are nested inside "#ifdef SHM_EXTRA_STATS" (they exist only
	 * to serve the shm_frag_file/func/line stats ladder). This function
	 * is gated on DBG_MALLOC alone, so it must not depend on
	 * SHM_EXTRA_STATS also being on.
	 */
	file = *(const char **)(tag + HG_ROUNDTO);
	if (!file)
		return;   /* stamped before any hg_malloc_dbg() call ever ran
		           * on this cell (e.g. still on its very first carve
		           * without having been freed+realloc'd) - matches
		           * fm_status_dbg's own "if (f->file)" guard */
	func = *(const char **)(tag + HG_ROUNDTO * 2);
	line = *(unsigned long *)(tag + HG_ROUNDTO * 3);

	/*
	 * Defence in depth, on top of the walker sizing its set exactly.
	 *
	 * The walker infers liveness by absence from that set, so anything that
	 * leaves the set incomplete turns a FREE cell into an apparently live
	 * one - and a free cell's payload holds the free-list link, so these
	 * file/func would be pointers INTO THE ARENA rather than string
	 * literals. dbg_ht_update() would then consume them as strings.
	 *
	 * hg_owns_any() is the exact discriminator: a real __FILE__ lives in the
	 * binary's rodata and can never be inside an arena; a free-list link
	 * always is. Costs nothing on a diagnostic path, and turns what was an
	 * abort into a skipped line.
	 */
	if (hg_owns_any((void *)file) || hg_owns_any((void *)func)) {
		c->skipped_notlive++;
		return;
	}

	if (dbg_ht_update(*c->allocd, file, func, line, hg_frag_size(payload)) < 0)
		LM_ERR("unable to update the %s allocation summary\n", HG_MALLOC_NAME);
}

/*
 * f_malloc-equivalent per-allocation-site summary: walk every live cell/
 * frag, aggregate by (file,func,line) via mem_dbg_hash (mem_dbg_hash.c -
 * same plain-malloc'd, allocator-independent structure fm_status_dbg
 * uses), dump, free. See hg_arena_walk_live()'s comment in hg_arena.c for
 * the one real accuracy caveat: exact pre-fork, best-effort post-fork
 * (can't see cells idling in ANOTHER worker's private free stack) - large
 * frags (hg_large_walk_live) don't share that caveat, they're always exact.
 */
void hg_status_dbg(struct hg_block *hb)
{
	mem_dbg_htable_t allocd;
	struct hg_dbg_dump_ctx ctx;
	struct mem_dbg_entry *it;
	unsigned int i;

	LM_GEN1(memdump, "hg_status_dbg (%p):\n", hb);
	if (!hb)
		return;

	hg_status(hb);

	dbg_ht_init(allocd);
	ctx.allocd = &allocd;
	ctx.skipped_notlive = 0;

	hg_arena_walk_live(hb, hg_dbg_dump_cb, &ctx);
	hg_large_walk_live(hb, hg_dbg_dump_cb, &ctx);

	LM_GEN1(memdump, " dumping summary of all alloc'ed. fragments:\n");
	LM_GEN1(memdump, "------------+---------------------------------------\n");
	LM_GEN1(memdump, "total_bytes | num_allocations x [file: func, line]\n");
	LM_GEN1(memdump, "------------+---------------------------------------\n");
	for (i = 0; i < DBG_HASH_SIZE; i++) {
		for (it = allocd[i]; it; it = it->next)
			LM_GEN1(memdump, " %10lu : %lu x [%s: %s, line %lu]\n",
				it->size, it->no_fragments, it->file, it->func, it->line);
	}
	LM_GEN1(memdump, "----------------------------------------------------\n");
	if (ctx.skipped_notlive)
		LM_GEN1(memdump, " %lu cell(s) skipped: header held free-list linkage, "
			"so the cell was free despite not being in the free set\n",
			ctx.skipped_notlive);

	dbg_ht_free(allocd);
}
#endif

/*
 * hg_cell_alloc()/hg_cell_free() (hg_arena.c, a separate translation unit)
 * have a signature fixed ONCE by hg_arena.h's *original* DBG_MALLOC state -
 * unlike f_malloc.c's internal helpers (fm_split_frag etc.), which live
 * inside f_malloc_dyn.h itself and get recompiled fresh on each pass below,
 * so their signature tracks the local #undef/#define correctly.
 * hg_malloc_dyn.h's PASS 2 (after the #undef below) must therefore NOT use
 * a bare "#ifdef DBG_MALLOC" to decide the hg_cell_alloc()/hg_cell_free()
 * call arity - that macro is locally stale during pass 2. This sentinel
 * captures the true, original state before any undef games. */
#ifdef DBG_MALLOC
#define HG_CELL_TAKES_DBG_ARGS 1
#else
#define HG_CELL_TAKES_DBG_ARGS 0
#endif

#include "hg_malloc_dyn.h"

#if !defined INLINE_ALLOC && defined DBG_MALLOC
#undef DBG_MALLOC
#include "hg_malloc_dyn.h"
#define DBG_MALLOC
#endif

#endif /* HG_MALLOC */
