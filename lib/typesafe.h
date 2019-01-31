/*
 * Copyright (c) 2016-2019  David Lamparter, for NetDEF, Inc.
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

#ifndef _FRR_TYPESAFE_H
#define _FRR_TYPESAFE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "compiler.h"

/* generic macros for all list-like types */

#define for_each(prefix, item, head) \
	for (item = prefix##_first(head); item;                                \
			item = prefix##_next(head, item))
#define for_each_safe(prefix, item, head) \
	for (__auto_type prefix##_safe = prefix##_next_safe(head,              \
			(item = prefix##_first(head)));                        \
		item;                                                          \
		item = prefix##_safe,                                          \
			prefix##_safe = prefix##_next_safe(head, prefix##_safe))
#define for_each_from(prefix, item, head, from)                                \
	for (item = from, from = prefix##_next_safe(head, item);               \
		item;                                                          \
		item = from, from = prefix##_next_safe(head, from))

/* single-linked list, unsorted/arbitrary.
 * can be used as queue with add_tail / pop
 */

/* don't use these structs directly */
struct slist_item {
	struct slist_item *next;
};

struct slist_head {
	struct slist_item *first, **last_next;
	size_t count;
};

/* use as:
 *
 * SLIST_MAKEITEM(namelist)
 * struct name {
 *   struct namelist_item nlitem;
 * }
 * SLIST_MAKEFUNCS(namelist, struct name, nlitem)
 */
#define TYPEDLIST_MAKEITEM(prefix) \
struct prefix ## _item { struct slist_item si; };                              \
struct prefix ## _head { struct slist_head sh; };

#define TYPEDLIST_MAKEFUNCS(prefix, type, field)                               \
                                                                               \
macro_inline void prefix ## _init(struct prefix##_head *h)                     \
{                                                                              \
	memset(h, 0, sizeof(*h));                                              \
	h->sh.last_next = &h->sh.first;                                        \
}                                                                              \
macro_inline void prefix ## _add_head(struct prefix##_head *h, type *item)     \
{                                                                              \
	item->field.si.next = h->sh.first;                                     \
	h->sh.first = &item->field.si;                                         \
	if (h->sh.last_next == &h->sh.first)                                   \
		h->sh.last_next = &item->field.si.next;                        \
	h->sh.count++;                                                         \
}                                                                              \
macro_inline void prefix ## _add_tail(struct prefix##_head *h, type *item)     \
{                                                                              \
	item->field.si.next = NULL;                                            \
	*h->sh.last_next = &item->field.si;                                    \
	h->sh.last_next = &item->field.si.next;                                \
	h->sh.count++;                                                         \
}                                                                              \
macro_inline void prefix ## _add_after(struct prefix##_head *h,                \
		type *after, type *item)                                       \
{                                                                              \
	struct slist_item **nextp;                                             \
	nextp = after ? &h->sh.first : &after->field.si.next;                  \
	item->field.si.next = *nextp;                                          \
	*nextp = &item->field.si;                                              \
	if (h->sh.last_next == nextp)                                          \
		h->sh.last_next = &item->field.si.next;                        \
	h->sh.count++;                                                         \
}                                                                              \
/* TODO: del_hint */                                                           \
macro_inline void prefix ## _del(struct prefix##_head *h, type *item)          \
{                                                                              \
	struct slist_item **iter = &h->sh.first;                               \
	while (*iter && *iter != &item->field.si)                              \
		iter = &(*iter)->next;                                         \
	if (!*iter)                                                            \
		return;                                                        \
	h->sh.count--;                                                         \
	*iter = item->field.si.next;                                           \
	if (!item->field.si.next)                                              \
		h->sh.last_next = iter;                                        \
}                                                                              \
macro_inline type *prefix ## _pop(struct prefix##_head *h)                     \
{                                                                              \
	struct slist_item *sitem = h->sh.first;                                \
	if (!sitem)                                                            \
		return NULL;                                                   \
	h->sh.count--;                                                         \
	h->sh.first = sitem->next;                                             \
	if (h->sh.first == NULL)                                               \
		h->sh.last_next = &h->sh.first;                                \
	return container_of(sitem, type, field.si);                            \
}                                                                              \
macro_inline type *prefix ## _first(struct prefix##_head *h)                   \
{                                                                              \
	return h->sh.first ? container_of(h->sh.first, type, field.si) : NULL; \
}                                                                              \
macro_inline type *prefix ## _next(struct prefix##_head * h, type *item)       \
{                                                                              \
	struct slist_item *sitem = &item->field.si;                            \
	return sitem->next ? container_of(sitem->next, type, field.si) : NULL; \
}                                                                              \
macro_inline type *prefix ## _next_safe(struct prefix##_head *h, type *item)   \
{                                                                              \
	struct slist_item *sitem;                                              \
	if (!item)                                                             \
		return NULL;                                                   \
	sitem = &item->field.si;                                               \
	return sitem->next ? container_of(sitem->next, type, field.si) : NULL; \
}                                                                              \
macro_inline size_t prefix ## _count(struct prefix##_head *h)                  \
{                                                                              \
	return h->sh.count;                                                    \
}                                                                              \
/* ... */

/* single-linked list, sorted.
 * can be used as priority queue with add / pop
 */

/* don't use these structs directly */
struct ssort_item {
	struct ssort_item *next;
};

struct ssort_head {
	struct ssort_item *first;
	size_t count;
};

/* use as:
 *
 * SLIST_MAKEITEM(namelist)
 * struct name {
 *   struct namelist_item nlitem;
 * }
 * SLIST_MAKEFUNCS(namelist, struct name, nlitem)
 */
#define TYPEDSORT_MAKEITEM(prefix) \
struct prefix ## _item { struct ssort_item si; };

#if 0
macro_inline int prefix ## _cmp(const struct ssort_item *a,                    \
		const struct ssort_item *b)                                    \
{                                                                              \
	return cmpfn(const_container_of(a, type, field.si),                    \
			const_container_of(b, type, field.si));                \
}                                                                              \
/**/
#endif

#define TYPEDSORT_MAKEFUNCS(prefix, type, field, cmpfn)                        \
struct prefix ## _head { struct ssort_head sh; };                              \
                                                                               \
macro_inline void prefix ## _init(struct prefix##_head *h)                     \
{                                                                              \
	memset(h, 0, sizeof(*h));                                              \
}                                                                              \
macro_inline void prefix ## _add(struct prefix##_head *h, type *item)          \
{                                                                              \
	struct ssort_item **np = &h->sh.first;                                 \
	while (*np && cmpfn(const_container_of(*np, type, field.si), item) < 0)\
		np = &(*np)->next;                                             \
	item->field.si.next = *np;                                             \
	*np = &item->field.si;                                                 \
	h->sh.count++;                                                         \
}                                                                              \
macro_inline type *prefix ## _find(struct prefix##_head *h, const type *item)  \
{                                                                              \
	struct ssort_item *sitem = h->sh.first;                                \
	int cmpval;                                                            \
	while (sitem && (cmpval = cmpfn(                                       \
			const_container_of(sitem, type, field.si), item) < 0)) \
		sitem = sitem->next;                                           \
	if (!sitem || cmpval > 0)                                              \
		return NULL;                                                   \
	return container_of(sitem, type, field.si);                            \
}                                                                              \
/* TODO: del_hint */                                                           \
macro_inline void prefix ## _del(struct prefix##_head *h, type *item)          \
{                                                                              \
	struct ssort_item **iter = &h->sh.first;                               \
	while (*iter && *iter != &item->field.si)                              \
		iter = &(*iter)->next;                                         \
	if (!*iter)                                                            \
		return;                                                        \
	h->sh.count--;                                                         \
	*iter = item->field.si.next;                                           \
}                                                                              \
macro_inline type *prefix ## _pop(struct prefix##_head *h)                     \
{                                                                              \
	struct ssort_item *sitem = h->sh.first;                                \
	if (!sitem)                                                            \
		return NULL;                                                   \
	h->sh.count--;                                                         \
	h->sh.first = sitem->next;                                             \
	return container_of(sitem, type, field.si);                            \
}                                                                              \
macro_inline type *prefix ## _first(struct prefix##_head *h)                   \
{                                                                              \
	return h->sh.first ? container_of(h->sh.first, type, field.si) : NULL; \
}                                                                              \
macro_inline type *prefix ## _next(struct prefix##_head *h, type *item)        \
{                                                                              \
	struct ssort_item *sitem = &item->field.si;                            \
	return sitem->next ? container_of(sitem->next, type, field.si) : NULL; \
}                                                                              \
macro_inline type *prefix ## _next_safe(struct prefix##_head *h, type *item)   \
{                                                                              \
	struct ssort_item *sitem;                                              \
	if (!item)                                                             \
		return NULL;                                                   \
	sitem = &item->field.si;                                               \
	return sitem->next ? container_of(sitem->next, type, field.si) : NULL; \
}                                                                              \
macro_inline size_t prefix ## _count(struct prefix##_head *h)                  \
{                                                                              \
	return h->sh.count;                                                    \
}                                                                              \
/* ... */

/* hash, "sorted" by hash value
 */

/* don't use these structs directly */
struct thash_item {
	struct thash_item *next;
	uint32_t hashval;
};

struct thash_head {
	struct thash_item **entries;
	uint32_t tabsize;
	uint32_t count;

	uint32_t maxsize;
	uint32_t minsize;
};

#define HASH_DEFAULT_SIZE

#define HASH_GROW_THRESHOLD(head)	((head).count >= (head).tabsize)
#define HASH_SHRINK_THRESHOLD(head)	((head).count <= ((head).tabsize - 1) / 2)

extern void typesafe_hash_grow(struct thash_head *head);
extern void typesafe_hash_shrink(struct thash_head *head);

/* use as:
 *
 * TYPEDHASH_MAKEITEM(namelist)
 * struct name {
 *   struct namelist_item nlitem;
 * }
 * TYPEDHASH_MAKEFUNCS(namelist, struct name, nlitem, cmpfunc, hashfunc)
 */
#define TYPEDHASH_MAKEITEM(prefix) \
struct prefix ## _item { struct thash_item hi; };

#define TYPEDHASH_MAKEFUNCS(prefix, type, field, cmpfn, hashfn)                \
struct prefix ## _head { struct thash_head hh; };                              \
                                                                               \
macro_inline void prefix ## _init(struct prefix##_head *h)                     \
{                                                                              \
	memset(h, 0, sizeof(*h));                                              \
}                                                                              \
macro_inline void prefix ## _fini(struct prefix##_head *h)                     \
{                                                                              \
	assert(h->hh.count == 0);                                              \
	h->hh.minsize = 0;                                                     \
	typesafe_hash_shrink(&h->hh);                                          \
	memset(h, 0, sizeof(*h));                                              \
}                                                                              \
macro_inline void prefix ## _add(struct prefix##_head *h, type *item)          \
{                                                                              \
	h->hh.count++;                                                         \
	if (!h->hh.tabsize || HASH_GROW_THRESHOLD(h->hh))                      \
		typesafe_hash_grow(&h->hh);                                    \
                                                                               \
	uint32_t hval = hashfn(item), hbits = hval & (h->hh.tabsize - 1);      \
	item->field.hi.hashval = hval;                                         \
	struct thash_item **np = &h->hh.entries[hbits];                        \
	while (*np && (*np)->hashval < hval)                                   \
		np = &(*np)->next;                                             \
	item->field.hi.next = *np;                                             \
	*np = &item->field.hi;                                                 \
}                                                                              \
macro_inline type *prefix ## _find(struct prefix##_head *h, const type *item)  \
{                                                                              \
	if (!h->hh.tabsize)                                                    \
		return NULL;                                                   \
	uint32_t hval = hashfn(item), hbits = hval & (h->hh.tabsize - 1);      \
	struct thash_item *hitem = h->hh.entries[hbits];                       \
	while (hitem && hitem->hashval < hval)                                 \
		hitem = hitem->next;                                           \
	while (hitem && hitem->hashval == hval) {                              \
		if (!cmpfn(const_container_of(hitem, type, field.hi), item))   \
			return container_of(hitem, type, field.hi);            \
		hitem = hitem->next;                                           \
	}                                                                      \
	return NULL;                                                           \
}                                                                              \
macro_inline void prefix ## _del(struct prefix##_head *h, type *item)          \
{                                                                              \
	if (!h->hh.tabsize)                                                    \
		return;                                                        \
	uint32_t hval = hashfn(item), hbits = hval & (h->hh.tabsize - 1);      \
	struct thash_item **np = &h->hh.entries[hbits];                        \
	while (*np && (*np)->hashval < hval)                                   \
		np = &(*np)->next;                                             \
	while (*np && *np != &item->field.hi && (*np)->hashval == hval)        \
		np = &(*np)->next;                                             \
	if (*np != &item->field.hi)                                            \
		return;                                                        \
	*np = item->field.hi.next;                                             \
	item->field.hi.next = NULL;                                            \
	h->hh.count--;                                                         \
	if (HASH_SHRINK_THRESHOLD(h->hh))                                      \
		typesafe_hash_shrink(&h->hh);                                  \
}                                                                              \
macro_inline type *prefix ## _pop(struct prefix##_head *h)                     \
{                                                                              \
	uint32_t i;                                                            \
	for (i = 0; i < h->hh.tabsize; i++)                                    \
		if (h->hh.entries[i]) {                                        \
			struct thash_item *hitem = h->hh.entries[i];           \
			h->hh.entries[i] = hitem->next;                        \
			h->hh.count--;                                         \
			hitem->next = NULL;                                    \
			if (HASH_SHRINK_THRESHOLD(h->hh))                      \
				typesafe_hash_shrink(&h->hh);                  \
			return container_of(hitem, type, field.hi);            \
		}                                                              \
	return NULL;                                                           \
}                                                                              \
macro_inline type *prefix ## _first(struct prefix##_head *h)                   \
{                                                                              \
	uint32_t i;                                                            \
	for (i = 0; i < h->hh.tabsize; i++)                                    \
		if (h->hh.entries[i])                                          \
			return container_of(h->hh.entries[i], type, field.hi); \
	return NULL;                                                           \
}                                                                              \
macro_inline type *prefix ## _next(struct prefix##_head *h, type *item)        \
{                                                                              \
	struct thash_item *hitem = &item->field.hi;                            \
	if (hitem->next)                                                       \
		return container_of(hitem->next, type, field.hi);              \
	uint32_t i = (hitem->hashval & (h->hh.tabsize - 1)) + 1;               \
        for (; i < h->hh.tabsize; i++)                                         \
		if (h->hh.entries[i])                                          \
			return container_of(h->hh.entries[i], type, field.hi); \
	return NULL;                                                           \
}                                                                              \
macro_inline type *prefix ## _next_safe(struct prefix##_head *h, type *item)   \
{                                                                              \
	if (!item)                                                             \
		return NULL;                                                   \
	return prefix ## _next(h, item);                                       \
}                                                                              \
macro_inline size_t prefix ## _count(struct prefix##_head *h)                  \
{                                                                              \
	return h->hh.count;                                                    \
}                                                                              \
/* ... */

/* skiplist, sorted.
 * can be used as priority queue with add / pop
 */

/* don't use these structs directly */
#define SKIPLIST_MAXDEPTH	16
#define SKIPLIST_EMBED		4
#define SKIPLIST_OVERFLOW	(SKIPLIST_EMBED - 1)

struct sskip_item {
	struct sskip_item *next[SKIPLIST_EMBED];
};

struct sskip_overflow {
	struct sskip_item *next[SKIPLIST_MAXDEPTH - SKIPLIST_OVERFLOW];
};

struct sskip_head {
	struct sskip_item hitem;
	struct sskip_item *overflow[SKIPLIST_MAXDEPTH - SKIPLIST_OVERFLOW];
	size_t count;
};

/* use as:
 *
 * TYPEDSKIP_MAKEITEM(namelist)
 * struct name {
 *   struct namelist_item nlitem;
 * }
 * TYPEDSKIP_MAKEFUNCS(namelist, struct name, nlitem, cmpfunc)
 */
#define TYPEDSKIP_MAKEITEM(prefix) \
struct prefix ## _head { struct sskip_head sh; };                              \
struct prefix ## _item { struct sskip_item si; };

#define TYPEDSKIP_MAKEFUNCS(prefix, type, field, cmpfn)                        \
                                                                               \
macro_inline int prefix ## _cmp(const struct sskip_item *a,                    \
		const struct sskip_item *b)                                    \
{                                                                              \
	return cmpfn(const_container_of(a, type, field.si),                    \
			const_container_of(b, type, field.si));                \
}                                                                              \
macro_inline void prefix ## _init(struct prefix##_head *h)                     \
{                                                                              \
	memset(h, 0, sizeof(*h));                                              \
	h->sh.hitem.next[SKIPLIST_OVERFLOW] = (struct sskip_item *)            \
		((uintptr_t)h->sh.overflow | 1);                               \
}                                                                              \
macro_inline void prefix ## _add(struct prefix##_head *h, type *item)          \
{                                                                              \
	typesafe_skiplist_add(&h->sh, &item->field.si, &prefix ## _cmp);       \
}                                                                              \
macro_inline type *prefix ## _find(struct prefix##_head *h, const type *item)  \
{                                                                              \
	struct sskip_item *sitem = typesafe_skiplist_find(&h->sh,              \
			&item->field.si, &prefix ## _cmp);                     \
	return sitem ? container_of(sitem, type, field.si) : NULL;             \
}                                                                              \
macro_inline void prefix ## _del(struct prefix##_head *h, type *item)          \
{                                                                              \
	typesafe_skiplist_del(&h->sh, &item->field.si, &prefix ## _cmp);       \
}                                                                              \
macro_inline type *prefix ## _pop(struct prefix##_head *h)                     \
{                                                                              \
	struct sskip_item *sitem = h->sh.hitem.next[0];                        \
	if (!sitem)                                                            \
		return NULL;                                                   \
	typesafe_skiplist_del(&h->sh, sitem, &prefix ## _cmp);                 \
	return container_of(sitem, type, field.si);                            \
}                                                                              \
macro_inline type *prefix ## _first(struct prefix##_head *h)                   \
{                                                                              \
	struct sskip_item *first = h->sh.hitem.next[0];                        \
	return first ? container_of(first, type, field.si) : NULL;             \
}                                                                              \
macro_inline type *prefix ## _next(struct prefix##_head *h, type *item)        \
{                                                                              \
	struct sskip_item *next = item->field.si.next[0];                      \
	return next ? container_of(next, type, field.si) : NULL;               \
}                                                                              \
macro_inline type *prefix ## _next_safe(struct prefix##_head *h, type *item)   \
{                                                                              \
	struct sskip_item *next;                                               \
	next = item ? item->field.si.next[0] : NULL;                           \
	return next ? container_of(next, type, field.si) : NULL;               \
}                                                                              \
macro_inline size_t prefix ## _count(struct prefix##_head *h)                  \
{                                                                              \
	return h->sh.count;                                                    \
}                                                                              \
/* ... */

extern void typesafe_skiplist_add(struct sskip_head *head,
		struct sskip_item *item, int (*cmpfn)(
			const struct sskip_item *a,
			const struct sskip_item *b));
extern struct sskip_item *typesafe_skiplist_find(struct sskip_head *head,
		const struct sskip_item *item, int (*cmpfn)(
			const struct sskip_item *a,
			const struct sskip_item *b));
extern void typesafe_skiplist_del(struct sskip_head *head,
		struct sskip_item *item, int (*cmpfn)(
			const struct sskip_item *a,
			const struct sskip_item *b));

#endif /* _FRR_TYPESAFE_H */
