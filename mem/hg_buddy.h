/*
 * buddy allocator over the HG_MALLOC huge-page grid
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

/*
 * One buddy tree per huge page - see mem/README.hg_arena_v2, "Structure".
 *
 * Order 0 is the leaf (HG_LEAF_SIZE, 8 KB); the top order is the whole page,
 * so the hierarchy terminates at the page and there is no cross-page merging
 * to implement. A wholly free top block IS one huge page.
 *
 * Splitting is pure bookkeeping: the arena is already mapped and pre-faulted
 * at init, so turning a 16 KB block into two 8 KB blocks changes records
 * only. Merging is the reverse and is the only legal defragmentation here,
 * because it moves free space rather than live objects (constraint 1 of the
 * design: callers hold raw pointers, so nothing live can ever be relocated).
 *
 * Everything in here runs under hb->lock, on the slow path that already takes
 * it. Nothing on the cell fast path calls into this file.
 */

#ifndef HG_BUDDY_H
#define HG_BUDDY_H

#include <stdint.h>
#include "hg_malloc.h"

/* HG_MAX_ORDERS lives in hg_malloc.h - struct hg_block needs it for the
 * free-list array, and this header includes that one, not the other way. */

/* A free block stores its own list linkage in its first bytes. Legitimate
 * because the block is free - nothing else is using those bytes - and it is
 * what keeps the free lists free of external metadata. The smallest block is
 * HG_LEAF_SIZE, vastly larger than this struct. */
struct hg_free_blk {
	struct hg_free_blk *next;
	struct hg_free_blk *prev;
	/* Set when the block goes on a free list, cleared when it comes off, so
	 * a block that is already free is distinguishable from a live one.
	 *
	 * NOTHING READS THIS YET. fl_push writes it and fl_unlink clears it and
	 * that is all - grep the tree for HG_FREE_MAGIC. The double-free and
	 * wild-pointer guard the previous wording of this comment claimed is not
	 * implemented, so do not rely on it; hg_buddy_free()'s existing checks
	 * (alignment, order, page bounds) are the whole of the protection today.
	 *
	 * uint64_t, not unsigned long. On ILP32 the constant below does not fit
	 * an unsigned long, so the STORE truncated it to 0x796672ee (arm32
	 * reports that as -Woverflow) while a COMPARE did not truncate at all:
	 * per C11 6.4.4.1p5 the UL suffix's type list continues to unsigned long
	 * long, so the macro kept its full value and `b->magic == HG_FREE_MAGIC`
	 * promoted the 32-bit field back to 64 bits and compared 0x796672ee
	 * against 0x62756464796672ee. Measured with gcc -m32: stores 0x796672ee,
	 * comparison yields 0. So the check this field exists for would have
	 * failed for EVERY legitimately free block on 32-bit - not merely lost
	 * entropy - and it would have compiled without a warning, because the
	 * -Woverflow fires on the store, never on the compare. */
	uint64_t magic;
};

#define HG_FREE_MAGIC  0x62756464796672eeULL   /* "buddyfr" */

/*
 * Per-page descriptor. Lives in the metadata region carved from the front of
 * the arena, NOT inside the page it describes - a page must be able to become
 * wholly free, and it cannot if its own bookkeeping sits in it.
 */
struct hg_page {
	char          *base;       /* first byte of this page */
	unsigned char *leaforder;  /* per leaf: order of the block starting here */
	unsigned long *bitmap;     /* per tree node: 1 = free, whole, not split */

	/* Fullness lists. Unused until the allocation policy lands (task #58,
	 * "page preference"); kept here because the descriptor is sized to the
	 * design's 64 byte budget and these two fit inside it. */
	struct hg_page *next;
	struct hg_page *prev;

	unsigned int idx;          /* page index within the arena */
	unsigned int free_leaves;  /* leaves not currently allocated */
	/* >0 on the FIRST page of a multi-page run, giving its length; the
	 * other pages of the run carry HG_RUN_MEMBER. See hg_buddy_alloc_run(). */
	unsigned int run_len;
};

/* run_len marker for a page that belongs to a run but does not head it */
#define HG_RUN_MEMBER  0xffffffffu
/* Deliberately NOT carrying a per-order free count per page: at HG_MAX_ORDERS
 * that array alone is 100 bytes and would take the descriptor from 48 to 148,
 * over double the design's 64 byte budget and, on a 5 GB arena, from 160 KB to
 * 370 KB of descriptors. Per-order counts are global, on hg_block. */

/* leaforder[] value for a leaf that does not START a block (it is in the
 * middle of a larger one, or its block is allocated and recorded elsewhere) */
#define HG_LEAF_NONE  0xff

/*
 * Set up the page grid's buddy state. Carves the metadata for every page from
 * the front of the arena, marks the region already consumed by the block
 * header and that metadata as allocated, and publishes the rest as free
 * blocks. Call once, from hg_arena_init(), after pages_init().
 *
 * Returns 0 on success, -1 if the metadata cannot be carved.
 */
int hg_buddy_init(struct hg_block *hb);

/* Allocate one block of exactly (HG_LEAF_SIZE << order) bytes, naturally
 * aligned. hb->lock must be held. Returns NULL when no page can serve it. */
void *hg_buddy_alloc(struct hg_block *hb, unsigned int order);

/* Return a block previously handed out by hg_buddy_alloc(), merging it with
 * its buddy as far up as it will go. hb->lock must be held. */
void hg_buddy_free(struct hg_block *hb, void *p, unsigned int order);

/* Order of the block containing @p, or -1 if @p is not in buddy space. */
int hg_buddy_order_of(const struct hg_block *hb, const void *p);

/*
 * The block CONTAINING @p, from any address inside it - two shifts, a table
 * byte and a mask, no search. This is the lookup the design is built around:
 * a cell being freed sits somewhere in the middle of its block, and reaching
 * the block is the only way its live count can ever be decremented, which is
 * the only way a block can ever be recognised as empty and reclaimed.
 *
 * Returns NULL if @p is outside the page grid or in a multi-page run.
 */
static inline void *hg_buddy_block_of(const struct hg_block *hb, const void *p)
{
	const struct hg_page *pg;
	unsigned long leaf;
	unsigned char o;

	if (!hb->buddy_ready || !hg_in_pages(hb, p))
		return NULL;
	pg = &hb->pages[hg_page_of(hb, p)];
	leaf = hg_leaf_of(hb, p);
	o = pg->leaforder[leaf];
	if (o == HG_LEAF_NONE)
		return NULL;
	return pg->base + ((leaf & ~((1UL << o) - 1)) << HG_LEAF_SHIFT);
}

/*
 * A run of @npages CONTIGUOUS whole pages, for the one thing the tree cannot
 * express: an allocation larger than a huge page. The large tier needs it -
 * a dialog or usrloc hash table is routinely several MB in one piece - and
 * before the buddy those went to the bump allocator, which had no upper bound.
 * Losing that would be a regression, not a simplification.
 *
 * Deliberately a linear scan for a free run, not an index: it is reached only
 * when the large tier grows past a page, the scan is over page descriptors
 * (2560 of them on a 5 GB arena) and it happens orders of magnitude less often
 * than a cell allocation. Paying for an index here would be paying for
 * nothing.
 *
 * hb->lock must be held. Returns the run's first byte, or NULL.
 */
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
