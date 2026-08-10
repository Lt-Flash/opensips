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

#ifdef HG_MALLOC

#include <string.h>

#include "hg_version.h"
#include "hg_malloc.h"
#include "hg_buddy.h"
#include "hg_arena.h"
#include "../dprint.h"

/*
 * Two records describe the same tree, because they answer different
 * questions and neither answers both cheaply:
 *
 *   leaforder[leaf]  the order of the block STARTING at that leaf, whether
 *                    free or allocated, or HG_LEAF_NONE if no block starts
 *                    there. This is what turns a bare pointer back into a
 *                    block - hg_buddy_free() is handed an address and must
 *                    learn its size without being told.
 *
 *   bitmap[node]     1 iff that tree node is a WHOLE FREE block, i.e. it is
 *                    sitting in a free list right now. Not "free" in the
 *                    sense of "contains free space": a split node is 0 even
 *                    though both its halves may be free. That is exactly the
 *                    predicate the merge step needs - "is my buddy free AND
 *                    entire" - and it answers it in one bit test.
 *
 * Keeping both is what makes split and merge O(1) instead of a search.
 */

/* node id in the per-page tree. Level 0 is the whole page (one node), level
 * `top` is the leaves; a complete tree over 2^top leaves has 2^(top+1)-1
 * nodes - 511 for a 2 MB page with 8 KB leaves, which is the 64 byte bitmap
 * the design budgets. */
static inline unsigned long node_id(unsigned int top, unsigned int order,
                                    unsigned long leaf)
{
	unsigned int level = top - order;

	return (1UL << level) - 1 + (leaf >> order);
}

static inline unsigned long nodes_per_page(unsigned int top)
{
	return (1UL << (top + 1)) - 1;
}

static inline int bit_test(const unsigned long *bm, unsigned long n)
{
	return (bm[n / (sizeof(long) * 8)] >> (n % (sizeof(long) * 8))) & 1UL;
}

static inline void bit_set(unsigned long *bm, unsigned long n)
{
	bm[n / (sizeof(long) * 8)] |= 1UL << (n % (sizeof(long) * 8));
}

static inline void bit_clear(unsigned long *bm, unsigned long n)
{
	bm[n / (sizeof(long) * 8)] &= ~(1UL << (n % (sizeof(long) * 8)));
}

/* --- free lists ------------------------------------------------------- */

static inline void fl_push(struct hg_block *hb, void *p, unsigned int order)
{
	struct hg_free_blk *b = (struct hg_free_blk *)p;

	b->prev = NULL;
	b->next = hb->bfree[order];
	b->magic = HG_FREE_MAGIC;
	if (b->next)
		b->next->prev = b;
	hb->bfree[order] = b;
	hb->nfree[order]++;
}

static inline void fl_unlink(struct hg_block *hb, void *p, unsigned int order)
{
	struct hg_free_blk *b = (struct hg_free_blk *)p;

	if (b->prev)
		b->prev->next = b->next;
	else
		hb->bfree[order] = b->next;
	if (b->next)
		b->next->prev = b->prev;
	b->magic = 0;
	hb->nfree[order]--;
}

/* --- geometry --------------------------------------------------------- */

static inline struct hg_page *page_of(struct hg_block *hb, const void *p)
{
	return &hb->pages[hg_page_of(hb, p)];
}

/* --- init ------------------------------------------------------------- */

/*
 * Publish one whole page as a single top-order free block. Only valid for a
 * page nothing has been carved out of yet.
 */
static void page_publish_whole(struct hg_block *hb, struct hg_page *pg)
{
	unsigned int top = hb->buddy_top;

	pg->leaforder[0] = (unsigned char)top;
	bit_set(pg->bitmap, node_id(top, top, 0));
	fl_push(hb, pg->base, top);
	pg->free_leaves = (unsigned int)hg_leaves_per_page(hb);
	hb->buddy_free_leaves += pg->free_leaves;
}

/*
 * Release the leaves [from, to) of a page that started out wholly reserved.
 *
 * Used for the one page that the block header and the buddy's own metadata
 * partly occupy. Done leaf by leaf through the ordinary free path so the
 * merges happen by the ordinary rules: freeing 24 consecutive leaves yields
 * whatever mix of orders the alignment actually permits, which is fiddly to
 * compute directly and trivial to get by construction.
 *
 * The alternative - refusing to use a partly-occupied page at all - would
 * throw away up to a whole huge page. That is 0.8% of a 256 MB shm arena but
 * 25% of an 8 MB pkg arena, which is not affordable.
 */
static void page_release_range(struct hg_block *hb, struct hg_page *pg,
                               unsigned long from, unsigned long to)
{
	unsigned long leaf;

	for (leaf = from; leaf < to; leaf++) {
		/* hand it to the free path as a legitimately allocated leaf */
		pg->leaforder[leaf] = 0;
		hg_buddy_free(hb, pg->base + (leaf << HG_LEAF_SHIFT), 0);
	}
}

int hg_buddy_init(struct hg_block *hb)
{
	unsigned long i, lpp, bmwords, meta, consumed_leaves;
	unsigned int top;
	char *meta_base, *cur;
	struct hg_page *pg;

	if (hb->npages == 0) {
		LM_INFO("%s: no whole pages, buddy reclaim inactive\n", hb->name);
		return 0;
	}

	top = hg_buddy_top_order(hb);
	if (top > HG_MAX_ORDERS) {
		LM_ERR("%s: %u buddy orders exceeds the %d the free-list array "
			"holds\n", hb->name, top, HG_MAX_ORDERS);
		return -1;
	}
	hb->buddy_top = top;

	lpp = hg_leaves_per_page(hb);
	bmwords = (nodes_per_page(top) + sizeof(long) * 8 - 1) /
	          (sizeof(long) * 8);

	/*
	 * One contiguous metadata carve for all pages, from the FRONT of the
	 * arena via the ordinary bump allocator - so it inherits whatever tier
	 * the reservation achieved, is shared for shm and private for pkg with
	 * no decision to make, and costs no extra huge pages. A dedicated page
	 * would be 25% overhead on an 8 MB pkg arena, and 30 workers each
	 * wanting one would burn 60 MB to hold 45 KB.
	 */
	meta = hb->npages * (sizeof(struct hg_page) + lpp +
	                     bmwords * sizeof(long));
	meta_base = hg_chunk_backing(hb, meta);
	if (!meta_base) {
		LM_ERR("%s: cannot carve %lu bytes of buddy metadata for %lu "
			"pages\n", hb->name, meta, hb->npages);
		return -1;
	}
	memset(meta_base, 0, meta);

	hb->pages = (struct hg_page *)(void *)meta_base;
	cur = meta_base + hb->npages * sizeof(struct hg_page);
	for (i = 0; i < hb->npages; i++) {
		pg = &hb->pages[i];
		pg->idx = (unsigned int)i;
		pg->base = hb->pbase + (i << hb->hps_shift);
		pg->leaforder = (unsigned char *)cur;
		cur += lpp;
		pg->bitmap = (unsigned long *)(void *)cur;
		cur += bmwords * sizeof(long);
		memset(pg->leaforder, HG_LEAF_NONE, lpp);
		/* starts wholly reserved; the loop below publishes what is free */
	}

	/*
	 * Everything below hoff is spoken for - the block header, then the
	 * metadata just carved. hoff is leaf aligned (hg_arena_init), so the
	 * boundary lands on a leaf and no partially-consumed leaf can be handed
	 * out. Round UP anyway: it costs at most one leaf and it means a future
	 * change to the bump allocator cannot silently start handing out memory
	 * that is already in use.
	 */
	consumed_leaves = 0;
	{
		unsigned long hoff = hb->hoff;
		const char *cend = hb->hbase + ((hoff + HG_LEAF_SIZE - 1) &
		                               ~(HG_LEAF_SIZE - 1));

		if (cend > hb->pbase)
			consumed_leaves = (unsigned long)(cend - hb->pbase) >>
			                  HG_LEAF_SHIFT;
	}

	for (i = 0; i < hb->npages; i++) {
		unsigned long first = i * lpp, last = first + lpp;

		pg = &hb->pages[i];
		if (consumed_leaves >= last)
			continue;                       /* wholly consumed */
		if (consumed_leaves <= first) {
			page_publish_whole(hb, pg);     /* wholly free */
			continue;
		}
		/* the single straddling page */
		page_release_range(hb, pg, consumed_leaves - first, lpp);
	}

	hb->buddy_ready = 1;
	LM_DBG("%s buddy: %lu pages, orders 0..%u (%lu B..%lu B), %lu B metadata "
		"(%lu B/page, %.3f%%), %lu of %lu leaves free after reserving %lu\n",
		hb->name, hb->npages, top, HG_LEAF_SIZE, HG_LEAF_SIZE << top,
		meta, meta / hb->npages,
		100.0 * (double)meta / (double)hb->hsize,
		hb->buddy_free_leaves, hb->npages * lpp, consumed_leaves);
	return 0;
}

/* --- allocate --------------------------------------------------------- */

void *hg_buddy_alloc(struct hg_block *hb, unsigned int order)
{
	unsigned int o, top = hb->buddy_top;
	struct hg_page *pg;
	unsigned long leaf;
	char *blk;

	if (!hb->buddy_ready || order > top)
		return NULL;

	/*
	 * Smallest free block that fits, so large free blocks are preserved by
	 * construction (design, "allocation policy"). Scanning UP from the
	 * requested order is exactly that: the first non-empty list is the
	 * smallest one that can serve it.
	 */
	for (o = order; o <= top; o++)
		if (hb->bfree[o])
			break;
	if (o > top)
		return NULL;

	blk = (char *)hb->bfree[o];
	fl_unlink(hb, blk, o);
	pg = page_of(hb, blk);
	leaf = hg_leaf_of(hb, blk);
	bit_clear(pg->bitmap, node_id(top, o, leaf));

	/* split down, publishing the upper half at each step. The lower half
	 * stays in hand, so the returned address never moves. */
	while (o > order) {
		unsigned long bleaf;
		char *buddy;

		o--;
		bleaf = leaf + (1UL << o);
		buddy = pg->base + (bleaf << HG_LEAF_SHIFT);
		pg->leaforder[bleaf] = (unsigned char)o;
		bit_set(pg->bitmap, node_id(top, o, bleaf));
		fl_push(hb, buddy, o);
	}

	pg->leaforder[leaf] = (unsigned char)order;
	pg->free_leaves -= 1U << order;
	hb->buddy_free_leaves -= 1UL << order;
	return blk;
}

/* --- free ------------------------------------------------------------- */

void hg_buddy_free(struct hg_block *hb, void *p, unsigned int order)
{
	unsigned int o = order, top = hb->buddy_top;
	struct hg_page *pg;
	unsigned long leaf;
	char *blk = p;

	if (!hg_in_pages(hb, p)) {
		LM_CRIT("%s: buddy free of %p, which is outside the page grid - "
			"ignoring\n", hb->name, p);
		return;
	}
	pg = page_of(hb, p);
	leaf = hg_leaf_of(hb, p);

	if (((unsigned long)p & ((HG_LEAF_SIZE << order) - 1)) !=
	    ((unsigned long)pg->base & ((HG_LEAF_SIZE << order) - 1))) {
		LM_CRIT("%s: buddy free of %p at order %u, which is not aligned to "
			"its own size - ignoring\n", hb->name, p, order);
		return;
	}
	if (pg->leaforder[leaf] != order) {
		LM_CRIT("%s: buddy free of %p as order %u, but leaf %lu records "
			"order %u - ignoring\n", hb->name, p, order, leaf,
			pg->leaforder[leaf]);
		return;
	}
	/*
	 * Double free. The leaforder check above does NOT catch it: a block that
	 * failed to merge still records its own order, so freeing it twice would
	 * look entirely legitimate and push it onto the free list a second time,
	 * after which two callers get the same address. The bitmap is the
	 * authority on "already free and entire", which is precisely this.
	 */
	if (bit_test(pg->bitmap, node_id(top, order, leaf))) {
		LM_CRIT("%s: double buddy free of %p at order %u - ignoring\n",
			hb->name, p, order);
		return;
	}

	pg->free_leaves += 1U << order;
	hb->buddy_free_leaves += 1UL << order;

	/*
	 * Merge upwards while the buddy is free and entire. The buddy's address
	 * is this block's with one bit flipped, which is what keeps each step
	 * O(1); the loop runs at most `top` times.
	 *
	 * Note the merge stops at the page. The top order IS the page, so there
	 * is no cross-page merging to implement and a wholly free top block is
	 * exactly one huge page - which is the unit the reclaim in task #57 will
	 * hand back.
	 */
	while (o < top) {
		unsigned long bleaf = leaf ^ (1UL << o);
		char *buddy = pg->base + (bleaf << HG_LEAF_SHIFT);

		if (!bit_test(pg->bitmap, node_id(top, o, bleaf)))
			break;                    /* allocated, or split */
		if (pg->leaforder[bleaf] != o)
			break;                    /* free but not at this order */

		fl_unlink(hb, buddy, o);
		bit_clear(pg->bitmap, node_id(top, o, bleaf));
		pg->leaforder[bleaf] = HG_LEAF_NONE;

		if (bleaf < leaf) {           /* we are the upper half - move down */
			pg->leaforder[leaf] = HG_LEAF_NONE;
			leaf = bleaf;
			blk = buddy;
		}
		o++;
	}

	pg->leaforder[leaf] = (unsigned char)o;
	bit_set(pg->bitmap, node_id(top, o, leaf));
	fl_push(hb, blk, o);
}

int hg_buddy_order_of(const struct hg_block *hb, const void *p)
{
	const struct hg_page *pg;
	unsigned long leaf;

	if (!hb->buddy_ready || !hg_in_pages(hb, p))
		return -1;
	pg = &hb->pages[hg_page_of(hb, p)];
	leaf = hg_leaf_of(hb, p);
	if (pg->leaforder[leaf] == HG_LEAF_NONE)
		return -1;
	return pg->leaforder[leaf];
}

#endif /* HG_MALLOC */
