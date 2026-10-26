/*
 * buddy allocator over the HG_MALLOC page grid
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

/* One buddy tree per huge page, no cross-page merging, and nothing live
 * moves. All of it runs under hb->lock; the cell fast path never calls it. */

#ifndef HG_BUDDY_H
#define HG_BUDDY_H

#include "hg_malloc.h"

/* HG_MAX_ORDERS is in hg_malloc.h: struct hg_block needs it */

/* A free block keeps its list links in its own first bytes, with no magic:
 * a double free is caught by the bitmap, out of reach of freed memory. */
struct hg_free_blk {
	struct hg_free_blk *next;
	struct hg_free_blk *prev;
};

/* Per-page descriptor, in the metadata region at the front of the arena, NOT
 * in the page it describes, so that the page can become wholly free. */
/* deferred-collapse attempts per page before it is left on 4 KB pages */
#define HG_COLLAPSE_TRIES 16

struct hg_page {
	char          *base;       /* first byte of this page */
	unsigned char *leaforder;  /* per leaf: order of the block it starts */
	unsigned long *bitmap;     /* per node: 1 = free, whole, not split */

	/* fullness lists, unused until a page-preference allocation policy
	 * lands; they fit the descriptor's 64 byte budget */
	struct hg_page *next;
	struct hg_page *prev;

	unsigned int idx;          /* page index within the arena */
	unsigned int free_leaves;  /* leaves not currently allocated */
	/* >0 on the FIRST page of a multi-page run, giving its length; the
	 * other pages carry HG_RUN_MEMBER (see hg_buddy_alloc_run()) */
	unsigned int run_len;
	/* the byte fields below fit existing padding: the descriptor stays
	 * 56 bytes on 64-bit builds */
	/* backing tier (enum hg_mem_tier) of the commit that brought this
	 * page in: shrink takes from any delta, so it debits tier_bytes */
	unsigned char tier;
	/* backing punched out (MADV_REMOVE) while whole-free, buddy tree kept:
	 * the next carve from this page must re-commit and re-pin it first */
	unsigned char punched;
	/* set while a trim or punch has the arena lock dropped mid-release:
	 * every scan that reads the bitmap rather than the list must skip it */
	unsigned char releasing;
	/* collapse attempts left for a page a worker's grow published on 4 KB
	 * pages (0 = none); worked off by hg_collapse_deferred */
	unsigned char want_collapse;
};

/* run_len marker for a page that belongs to a run but does not head it */
#define HG_RUN_MEMBER  0xffffffffu
/* no per-order free count per page: that array would more than double the
 * descriptor; per-order counts are global, on hg_block */

/* leaforder[] value for a leaf that does not START a block (it is in the
 * middle of a larger one, or its block is allocated and recorded elsewhere) */
#define HG_LEAF_NONE  0xff

/* Carve the per-page metadata from the arena front, publish the rest as
 * free. Once, from hg_arena_init() after pages_init(); 0, or -1. */
int hg_buddy_init(struct hg_block *hb);

/* Allocate one block of exactly (HG_LEAF_SIZE << order) bytes, naturally
 * aligned. hb->lock must be held. Returns NULL when no page can serve it. */
void *hg_buddy_alloc(struct hg_block *hb, unsigned int order);

/* the *_unlocks functions and the three ticks DROP hb->lock inside: held
 * on entry and on return, but nothing read before the call is valid after */
/* commit up to max(granule, @need) more and publish the new whole pages
 * as free; 0 = grew (retry the allocation), -1 = cannot */
int hg_buddy_grow_unlocks(struct hg_block *hb, unsigned long need, enum hg_grow_why why);
/* the always-on headroom rule on its own (the maintenance process ticks it
 * every second); hb->lock held; 1 = grew */
int hg_grow_headroom_tick(struct hg_block *hb);

/* end a grow-blocked episode (logged if one was latched) and re-arm its
 * report; hb->lock held. @how ends "GROW-BLOCKED cleared - " */
void hg_grow_unblock(struct hg_block *hb, const char *how);

/* the sweep's promoter for an armed grow-blocked episode (the GC pass
 * cannot latch with nothing reclaimable); disarms a quiet one. hb->lock held */
void hg_grow_blocked_tick(struct hg_block *hb);

/* the down-slow shrink gate, once per sweep interval, hb->lock held; never
 * below -m/-M. Only its owner can shrink a pkg arena, from its flush path */
void hg_shrink_tick(struct hg_block *hb);
/* one drain step (trim + punch) if the quiet window is open; every
 * maintenance tick, hb->lock held */
void hg_drain_tick(struct hg_block *hb);

/* the profile's proactive grow gate - same cadence, call sites
 * and lock contract as hg_shrink_tick(). No-op without a policy. */
void hg_grow_tick(struct hg_block *hb);

/* Return a block previously handed out by hg_buddy_alloc(), merging it with
 * its buddy as far up as it will go. hb->lock must be held. */
void hg_buddy_free(struct hg_block *hb, void *p, unsigned int order);

/* Order of the block containing @p, or -1 if @p is not in buddy space. */
int hg_buddy_order_of(const struct hg_block *hb, const void *p);

/* The block CONTAINING @p, from any address inside it, without a search;
 * NULL if @p is outside the page grid or in a multi-page run. */
static inline void *hg_buddy_block_of(const struct hg_block *hb, const void *p)
{
	const struct hg_page *pg;
	unsigned long leaf;
	unsigned char o;

	if (!hb->buddy_ready || !hg_in_pages(hb, p))
		return NULL;
	pg = &hb->pages[hg_page_of(hb, p)];
	/* a punched page keeps its leaf orders but not its memory: a block
	 * there is a stale pointer's, and its header must not be read */
	if (pg->punched || pg->releasing)
		return NULL;
	leaf = hg_leaf_of(hb, p);
	o = pg->leaforder[leaf];
	/* a damaged order is not shifted by */
	if (o == HG_LEAF_NONE || o > hb->buddy_top)
		return NULL;
	return pg->base + ((leaf & ~((1UL << o) - 1)) << HG_LEAF_SHIFT);
}

/* A run of @npages CONTIGUOUS whole pages, for a large-tier allocation
 * over a huge page; a linear scan, hb->lock held. First byte, or NULL */
void *hg_buddy_alloc_run(struct hg_block *hb, unsigned long npages);

/* Release a run obtained from hg_buddy_alloc_run(). hb->lock must be held. */
void hg_buddy_free_run(struct hg_block *hb, void *p);

/* Length in pages of the run starting at @p, or 0 if @p heads no run. */
unsigned long hg_buddy_run_len(const struct hg_block *hb, const void *p);

/* Highest order this arena can serve, i.e. the whole-page order. */
static inline unsigned int hg_buddy_top_order(const struct hg_block *hb)
{
	return hb->hps_shift - HG_LEAF_SHIFT;
}

/* Smallest order whose block is at least @bytes. Returns -1 if @bytes exceeds
 * a whole page, which the caller must route to the large tier instead. */
static inline int hg_buddy_order_for(const struct hg_block *hb,
                                     unsigned long bytes)
{
	unsigned int o = 0;

	while ((HG_LEAF_SIZE << o) < bytes) {
		if (o >= hg_buddy_top_order(hb))
			return -1;
		o++;
	}
	return (int)o;
}

#endif /* HG_BUDDY_H */
