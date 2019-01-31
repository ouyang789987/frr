/*
 * Copyright (c) 2019  David Lamparter, for NetDEF, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>

#include "typesafe.h"
#include "memory.h"

DEFINE_MTYPE_STATIC(LIB, TYPEDHASH_BUCKET, "Typed-hash bucket")
DEFINE_MTYPE_STATIC(LIB, SKIPLIST_OFLOW, "Skiplist overflow")

void typesafe_hash_grow(struct thash_head *head)
{
	uint32_t newsize = head->count, i, j;

	newsize |= newsize >> 1;
	newsize |= newsize >> 2;
	newsize |= newsize >> 4;
	newsize |= newsize >> 8;
	newsize |= newsize >> 16;
	newsize++;

	if (head->maxsize && newsize > head->maxsize)
		newsize = head->maxsize;
	if (newsize == head->tabsize)
		return;

	head->entries = XREALLOC(MTYPE_TYPEDHASH_BUCKET, head->entries,
			sizeof(head->entries[0]) * newsize);
	memset(head->entries + head->tabsize, 0,
			sizeof(head->entries[0]) * (newsize - head->tabsize));
	for (i = 0; i < head->tabsize; i++) {
		struct thash_item **apos = &head->entries[i], *item;
		j = i + head->tabsize;
		while ((item = *apos)) {
			if ((item->hashval & (newsize - 1)) >= j) {
				*apos = NULL;
				head->entries[j] = item;
				j += head->tabsize;
			}
			apos = &item->next;
		}
	}
	head->tabsize = newsize;
}

void typesafe_hash_shrink(struct thash_head *head)
{
	uint32_t newsize = head->count, i, j;

	if (!head->count) {
		XFREE(MTYPE_TYPEDHASH_BUCKET, head->entries);
		head->tabsize = 0;
		return;
	}

	newsize |= newsize >> 1;
	newsize |= newsize >> 2;
	newsize |= newsize >> 4;
	newsize |= newsize >> 8;
	newsize |= newsize >> 16;
	newsize++;

	if (head->minsize && newsize < head->minsize)
		newsize = head->minsize;
	if (newsize == head->tabsize)
		return;

	for (i = 0; i < newsize; i++) {
		struct thash_item **apos = &head->entries[i];
		for (j = i + newsize; j < head->tabsize; j += newsize) {
			while (*apos)
				apos = &(*apos)->next;
			*apos = head->entries[j];
		}
	}
	head->entries = XREALLOC(MTYPE_TYPEDHASH_BUCKET, head->entries,
			sizeof(head->entries[0]) * newsize);
	head->tabsize = newsize;
}

/* skiplist */

static inline struct sskip_item *sl_level_get(struct sskip_item *item,
			size_t level)
{
	if (level < SKIPLIST_OVERFLOW)
		return item->next[level];
	if (level == SKIPLIST_OVERFLOW && !((uintptr_t)item->next[level] & 1))
		return item->next[level];

	uintptr_t ptrval = (uintptr_t)item->next[SKIPLIST_OVERFLOW];
	ptrval &= UINTPTR_MAX - 3;
	struct sskip_overflow *oflow = (struct sskip_overflow *)ptrval;
	return oflow->next[level - SKIPLIST_OVERFLOW];
}

static inline void sl_level_set(struct sskip_item *item, size_t level,
		struct sskip_item *value)
{
	if (level < SKIPLIST_OVERFLOW)
		item->next[level] = value;
	else if (level == SKIPLIST_OVERFLOW && !((uintptr_t)item->next[level] & 1))
		item->next[level] = value;
	else {
		uintptr_t ptrval = (uintptr_t)item->next[SKIPLIST_OVERFLOW];
		ptrval &= UINTPTR_MAX - 3;
		struct sskip_overflow *oflow = (struct sskip_overflow *)ptrval;
		oflow->next[level - SKIPLIST_OVERFLOW] = value;
	}
}

void typesafe_skiplist_add(struct sskip_head *head, struct sskip_item *item,
		int (*cmpfn)(const struct sskip_item *a,
				const struct sskip_item *b))
{
	size_t level = SKIPLIST_MAXDEPTH, newlevel;
	struct sskip_item *prev = &head->hitem, *next;
	int cmpval;

	memset(item, 0, sizeof(*item));

	/* level / newlevel are 1-counted here */
	newlevel = __builtin_ctz(random()) + 1;
	if (newlevel > SKIPLIST_MAXDEPTH)
		newlevel = SKIPLIST_MAXDEPTH;

	if (newlevel > SKIPLIST_EMBED) {
		struct sskip_overflow *oflow;
		oflow = XMALLOC(MTYPE_SKIPLIST_OFLOW, sizeof(void *)
				* (newlevel - SKIPLIST_OVERFLOW));
		item->next[SKIPLIST_OVERFLOW] = (struct sskip_item *)
				((uintptr_t)oflow | 1);
	}

	while (level >= newlevel) {
		next = sl_level_get(prev, level - 1);
		if (!next) {
			level--;
			continue;
		}
		cmpval = cmpfn(next, item);
		if (cmpval < 0) {
			prev = next;
			continue;
		}
		level--;
	}
	sl_level_set(item, level, next);
	sl_level_set(prev, level, item);
	/* level is now 0-counted and < newlevel*/
	while (level) {
		level--;
		next = sl_level_get(prev, level);
		while (next && cmpfn(next, item) < 0) {
			prev = next;
			next = sl_level_get(prev, level);
		}

		sl_level_set(item, level, next);
		sl_level_set(prev, level, item);
	};
}

/* NOTE: level counting below is 1-based since that makes the code simpler! */

struct sskip_item *typesafe_skiplist_find(struct sskip_head *head,
		const struct sskip_item *item, int (*cmpfn)(
				const struct sskip_item *a,
				const struct sskip_item *b))
{
	size_t level = SKIPLIST_MAXDEPTH;
	struct sskip_item *prev = &head->hitem, *next;
	int cmpval;

	while (level) {
		next = sl_level_get(prev, level - 1);
		if (!next) {
			level--;
			continue;
		}
		cmpval = cmpfn(next, item);
		if (cmpval < 0) {
			prev = next;
			continue;
		}
		if (cmpval == 0)
			return next;
		level--;
	}
	return NULL;
}

void typesafe_skiplist_del(struct sskip_head *head, struct sskip_item *item,
		int (*cmpfn)(const struct sskip_item *a,
				const struct sskip_item *b))
{
	size_t level = SKIPLIST_MAXDEPTH;
	struct sskip_item *prev = &head->hitem, *next;
	int cmpval;

	while (level) {
		next = sl_level_get(prev, level - 1);
		if (!next) {
			level--;
			continue;
		}
		if (next == item) {
			sl_level_set(prev, level - 1,
				sl_level_get(item, level - 1));
			level--;
			continue;
		}
		cmpval = cmpfn(next, item);
		if (cmpval < 0) {
			prev = next;
			continue;
		}
		level--;
	}
	if ((uintptr_t)item->next[SKIPLIST_OVERFLOW] & 1) {
		uintptr_t ptrval = (uintptr_t)item->next[SKIPLIST_OVERFLOW];
		ptrval &= UINTPTR_MAX - 3;
		struct sskip_overflow *oflow = (struct sskip_overflow *)ptrval;
		XFREE(MTYPE_SKIPLIST_OFLOW, oflow);
	}
	memset(item, 0, sizeof(*item));
}
