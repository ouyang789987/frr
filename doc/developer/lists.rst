List implementations
====================

.. note::

   The term *list* is used generically for lists, skiplists, trees and hash
   tables in this document.

Common list interface
---------------------

FRR includes a set of list-like data structure implementations with abstracted
common APIs.  The purpose of this is easily allow swapping out one
data structure for another while also making the code easier to read and write.
There is one API for unsorted lists and a similar but not identical API for
sorted lists.

For unsorted lists, the following implementations exist:

- single-linked list with tail pointer (e.g. STAILQ in BSD)

- atomic single-linked list with tail pointer


For sorted lists, these data structures are implemented:

- single-linked list

- atomic single-linked list

- skiplist

- red-black tree (based on OpenBSD RB_TREE)

- hash table (note below)


The following sorted structures are likely to be implemented at some point
in the future:

- atomic skiplist

- atomic hash table (note below)


Hash tables mostly follow the "sorted" API but use the hash value as sorting
key.  Also, iterating while modifying does not work with hash tables.

The APIs are all designed to be as type-safe as possible.  This means that
there will be a compiler warning when an item doesn't match the list, or
the return value has a different type, or other similar situations.  **You
should never use casts with these APIs.**  If a cast is neccessary in relation
to these APIs, there is probably something wrong with the overall design.

Only the following pieces use dynamically allocated memory:

- the hash table itself is dynamically grown and shrunk

- skiplists store up to 4 next pointers inline but will dynamically allocate
  memory to hold an item's 5th up to 16th next pointer (if they exist)

Datastructure type setup
------------------------

Each of the data structures has a ``*_MAKEITEM`` and a ``*_MAKEFUNCS`` macro
to set up an "instantiation" of the list.  This works somewhat similar to C++
templating, though much simpler.

**In all following text, the dollar sign ($) is replaced with a name choosen
for the instance of the datastructure.**

The common setup pattern will look like this:

.. code-block:: c

   XXX_MAKEITEM($)
   struct item {
       int otherdata;
       struct $_item mylistitem;
   }

   struct $_head mylisthead;

   /* unsorted: */
   XXX_MAKEFUNCS($, struct item, mylistitem)

   /* sorted: */
   int compare_func(const struct item *a, const struct item *b);
   XXX_MAKEFUNCS($, struct item, mylistitem, compare_func)

   /* hash tables: */
   int compare_func(const struct item *a, const struct item *b);
   uint32_t hash_func(const struct item *a);
   XXX_MAKEFUNCS($, struct item, mylistitem, compare_func, hash_func)

``XXX`` is replaced with the name of the data structure, e.g. ``TYPEDSKIP``
or ``ATOMLIST``.  The ``XXX_MAKEFUNCS`` invocation can either occur in a `.h`
file (if the list needs to be accessed from several C files) or it can be
placed in a `.c` file (if the list is only accessed from that file.)  The
``XXX_MAKEITEM`` invocation defines the ``struct $_item`` and ``struct
$_head`` types and must therefore occur before these are used.

To switch between compatible data structures, only these two lines need to be
changes.  To switch to a data structure with a different API, some source
changes are necessary.

Common iteration macros
-----------------------

The following iteration macros work across all data structures:

.. c:function:: for_each($, item, head)

   Equivalent to:

   .. code-block:: c

      for (item = $_first(head); item; item = $_next(head, item))

   Note that this will fail if the list is modified while being iterated
   over.

.. c:function:: for_each_safe($, item, head)

   Same as the previous, but the next element is pre-loaded into a "hidden"
   variable (named ``$_safe``.)  Equivalent to:

   .. code-block:: c

      for (item = $_first(head); item; item = next) {
          next = $_next_safe(head, item);
          ...
      }

   .. warning::

      Iterating over hash tables while adding or removing items is not
      possible.  The iteration position will be corrupted when the hash
      tables is resized while iterating.  This will cause items to be
      skipped or iterated over twice.

.. c:function:: for_each_from($, item, head, from)

   Iterates over the list, starting at item ``from``.  This variant is "safe"
   as in the previous macro.  Equivalent to:

   .. code-block:: c

      for (item = from; item; item = from) {
          from = $_next_safe(head, item);
          ...
      }

   .. note::

      The ``from`` variable is written to.  This is intentional - you can
      resume iteration after breaking out of the loop by keeping the ``from``
      value persistent and reusing it for the next loop.

.. todo::

   maybe flip the order of ``item`` & ``head``, i.e.
   ``for_each($, head, item)``

Common API
----------

The following documentation assumes that a list has been defined using
``$`` as the name, and ``itemtype`` being the type of the list items (e.g.
``struct item``.)

.. c:function:: void $_init(struct $_head *)

   Initializes the list for use.  For most implementations, this just sets
   some values.  Hash tables are the only implementation that allocates
   memory in this call.

.. c:function:: void $_fini(struct $_head *)

   Reverse the effects of :c:func:`$_init()`.  The list must be empty
   when this function is called.

   .. warning::

      This function may ``assert()`` if the list is not empty.

.. c:function:: size_t $_count(struct $_head *)

   Returns the number of items in a structure.  All structures store a
   counter in their `$_head` so that calling this function completes
   in O(1).

   .. note::

      For atomic lists with concurrent access, the value will already be
      outdated by the time this function returns and can therefore only be
      used as an estimate.

.. c:function:: itemtype *$_first(struct $_head *)

   Returns the first item in the structure, or ``NULL`` if the structure is
   empty.  This is O(1) for all data structures except red-black trees
   where it is O(log n).

.. c:function:: itemtype *$_pop(struct $_head *)

   Remove and return the first item in the structure, or ``NULL`` if the
   structure is empty.  Like :c:func:`$_first`, this is O(1) for all
   data structures except red-black trees where it is O(log n) again.

   This function can be used to build queues (with unsorted structures) or
   priority queues (with sorted structures.)

   Another common pattern is deleting all list items:

   .. code-block:: c

      while ((item = $_pop(head)))
          item_free(item);

   .. note::

      This function can - and should - be used with hash tables.  It is not
      affected by the "modification while iterating" problem.  To remove
      all items from a hash table, use the loop demonstrated above.

.. c:function:: itemtype *$_next(struct $_head *, itemtype *prev)

   Return the item that follows after ``prev``, or ``NULL`` if ``prev`` is
   the last item.

   .. warning::

      ``prev`` must not be ``NULL``!  Use :c:func:`$_next_safe()` if
      ``prev`` might be ``NULL``.

.. c:function:: itemtype *$_next_safe(struct $_head *, itemtype *prev)

   Same as :c:func:`$_next()`, except that ``NULL`` is returned if
   ``prev`` is ``NULL``.

.. c:function:: itemtype *$_del(struct $_head *, itemtype *item)

   Remove ``item`` from the list and return it.

   .. note::

      This function's behaviour is undefined if ``item`` is not actually
      on the list.  Some structures return ``NULL`` in this case while others
      return ``item``.  The function may also call ``assert()`` (but most
      don't.)

.. todo::

   ``$_del_after()`` / ``$_del_hint()``?

API for unsorted structures
---------------------------

Since the insertion position is not pre-defined for unsorted data, there
are several functions exposed to insert data:

.. note::

   ``item`` must not be ``NULL`` for any of the following functions.

.. c:function:: XXX_MAKEFUNCS($, type, field)

   :param listtype XXX: ``TYPEDLIST`` or ``ATOMLIST`` to select a data structure
      implementation.
   :param token $: Gives the name prefix that is used for the functions
      created for this instantiation.  ``XXX_MAKEFUNCS(foo, ...)``
      gives ``struct foo_item``, ``foo_add_head()``, ``foo_count()``, etc.  Note
      that this must match the value given in ``XXX_MAKEITEM(foo)``.
   :param typename type: Specifies the data type of the list items, e.g.
      ``struct item``.  Note that ``struct`` must be added here, it is not
      automatically added.
   :param token field: References a struct member of ``type`` that must be
      typed as ``struct foo_item``.  This struct member is used to
      store "next" pointers or other data structure specific data.

.. c:function:: void $_add_head(struct $_head *, itemtype *item)

   Insert an item at the beginning of the structure, before the first item.
   This is an O(1) operation for non-atomic lists.

.. c:function:: void $_add_tail(struct $_head *, itemtype *item)

   Insert an item at the end of the structure, after the last item.
   This is also an O(1) operation for non-atomic lists.

.. c:function:: void $_add_after(struct $_head *, itemtype *after, itemtype *item)

   Insert ``item`` behind ``after``. If ``after`` is ``NULL``, the item is
   inserted at the beginning of the list as with :c:func:`$_add_head`.
   This is also an O(1) operation for non-atomic lists.

   A common pattern is to keep a "previous" pointer around while iterating:

   .. code-block:: c

      itemtype *prev = NULL, *item;

      for_each_safe($, head, item) {
          if (something) {
              $_add_after(head, prev, item);
              break;
          }
          prev = item;
      }

   .. todo::

      maybe flip the order of ``item`` & ``after``?
      ``$_add_after(head, item, after)``

API for sorted structures
-------------------------

Sorted data structures do not need to have an insertion position specified,
therefore the insertion calls are different from unsorted lists.  Also,
sorted lists can be searched for a value.

.. c:function:: XXX_MAKEFUNCS($, type, field, compare_func)

   :param listtype XXX: One of the following:
       ``TYPEDSORT`` (single-linked sorted list), ``TYPEDSKIP`` (skiplist),
       ``TYPEDRB`` (RB-tree) or ``ATOMSORT`` (atomic single-linked list).
   :param token $: Gives the name prefix that is used for the functions
      created for this instantiation.  ``XXX_MAKEFUNCS(foo, ...)``
      gives ``struct foo_item``, ``foo_add()``, ``foo_count()``, etc.  Note
      that this must match the value given in ``XXX_MAKEITEM(foo)``.
   :param typename type: Specifies the data type of the list items, e.g.
      ``struct item``.  Note that ``struct`` must be added here, it is not
      automatically added.
   :param token field: References a struct member of ``type`` that must be
      typed as ``struct foo_item``.  This struct member is used to
      store "next" pointers or other data structure specific data.
   :param funcptr compare_func: Item comparison function, must have the
      following function signature:
      ``int function(const itemtype *, const itemtype*)``.  This function
      may be static if the list is only used in one file.

   .. warning::

      Sorted data structures do not permit adding two items that compare
      equal (i.e. ``compare_func(a, b)`` returns 0.)  If this is needed,
      the comparison function should, when 0 would be returned otherwise,
      instead compare the pointer values of ``a`` and ``b``.  (This means
      that two items would only be equal if they are actually the same
      location in memory, i.e. the same item is being added twice.)

      This applies in particular to priority queues that need to support
      multiple items with the same prioriy value.

.. c:function:: itemtype *$_add(struct $_head *, itemtype *item)

   Insert an item at the appropriate sorted position.  If another item exists
   in the list that compares as equal (``compare_func()`` == 0), ``item`` is
   not inserted into the list and the already-existing item in the list is
   returned.  Otherwise, on successful insertion, ``NULL`` is returned.

.. c:function:: itemtype *$_find(struct $_head *, const itemtype *ref)

   Search the list for an item that compares equal to ``ref``.  If no equal
   item is found, return ``NULL``.

   This function is likely used with a temporary stack-allocated value for
   ``ref`` like so:

   .. code-block:: c

      itemtype searchfor = { .foo = 123 };

      itemtype *item = $_find(head, &searchfor);

.. todo::

   Need something like ``$_find_before``, ``$_find_after`` or similar
   to return closest-match.

   Also, ``$_add_after()`` / ``$_add_hint()``?

API for hash tables
-------------------

.. c:function:: XXX_MAKEFUNCS($, type, field, compare_func, hash_func)

   :param listtype XXX: Only ``TYPEDHASH`` is currently available.
   :param token $: Gives the name prefix that is used for the functions
      created for this instantiation.  ``XXX_MAKEFUNCS(foo, ...)``
      gives ``struct foo_item``, ``foo_add()``, ``foo_count()``, etc.  Note
      that this must match the value given in ``XXX_MAKEITEM(foo)``.
   :param typename type: Specifies the data type of the list items, e.g.
      ``struct item``.  Note that ``struct`` must be added here, it is not
      automatically added.
   :param token field: References a struct member of ``type`` that must be
      typed as ``struct foo_item``.  This struct member is used to
      store "next" pointers or other data structure specific data.
   :param funcptr compare_func: Item comparison function, must have the
      following function signature:
      ``int function(const itemtype *, const itemtype*)``.  This function
      may be static if the list is only used in one file.  For hash tables,
      this function is only used to check for equality, the ordering is
      ignored.
   :param funcptr hash_func: Hash calculation function, must have the
      following function signature:
      ``uint32_t function(const itemtype *)``.  The hash value for items
      stored in a hash table is cached in each item, so this value need not
      be cached by the user code.

   .. warning::

      Items that compare as equal cannot be inserted.  Refer to the notes
      about sorted structures in the previous section.

.. c:function:: void $_init_size(struct $_head *, size_t size)

   Same as :c:func:`$_init()` but preset the minimum hash table to
   ``size``.

Hash tables also support :c:func:`$_add()` and :c:func:`$_find()` with
the same semantics as noted above.


Atomic lists
------------

`atomlist.h` provides an unsorted and a sorted atomic single-linked list.
Since atomic memory accesses can be considerably slower than plain memory
accessses (depending on the CPU type), these lists should only be used where
neccessary.

The following guarantees are provided regarding concurrent access:

- the operations are lock-free but not wait-free.

  Lock-free means that it is impossible for all threads to be blocked.  Some
  thread will always make progress, regardless of what other threads do.  (This
  even includes a random thread being stopped by a debugger in a random
  location.)

  Wait-free implies that the time any single thread might spend in one of the
  calls is bounded.  This is not provided here since it is not normally
  relevant to practical operations.  What this means is that if some thread is
  hammering a particular list with requests, it is possible that another
  thread is blocked for an extended time.  The lock-free guarantee still
  applies since the hammering thread is making progress.

- without a RCU mechanism in place, the point of contention for atomic lists
  is memory deallocation.  As it is, **a rwlock is required for correct
  operation**.  The *read* lock must be held for all accesses, including
  reading the list, adding items to the list, and removing items from the
  list.  The *write* lock must be acquired and released before deallocating
  any list element.  If this is not followed, an use-after-free can occur
  as a MT race condition when an element gets deallocated while another
  thread is accessing the list.

  .. note::

     The *write* lock does not need to be held for deleting items from the
     list, and there should not be any instructions between the
     ``pthread_rwlock_wrlock`` and ``pthread_rwlock_unlock``.  The write lock
     is used as a sequence point, not as an exclusion mechanism.

- insertion operations are always safe to do with the read lock held.
  Added items are immediately visible after the insertion call returns and
  should not be touched anymore.

- when removing a *particular* (pre-determined) item, the caller must ensure
  that no other thread is attempting to remove that same item.  If this cannot
  be guaranteed by architecture, a separate lock might need to be added.

- concurrent `pop` calls are always safe to do with only the read lock held.
  This does not fall under the previous rule since the `pop` call will select
  the next item if the first is already being removed by another thread.

  **Deallocation locking still applies.**  Assume another thread starts
  reading the list, but gets task-switched by the kernel while reading the
  first item.  `pop` will happily remove and return that item.  If it is
  deallocated without acquiring and releasing the write lock, the other thread
  will later resume execution and try to access the now-deleted element.

- the list count should be considered an estimate.  Since there might be
  concurrent insertions or removals in progress, it might already be outdated
  by the time the call returns.  No attempt is made to have it be correct even
  for a nanosecond.

Overall, atomic lists are well-suited for MT queues; concurrent insertion,
iteration and removal operations will work with the read lock held.

Code snippets
^^^^^^^^^^^^^

Iteration:

.. code-block:: c

   struct item *i;

   pthread_rwlock_rdlock(&itemhead_rwlock);
   for_each(itemlist, i, &itemhead) {
     /* lock must remain held while iterating */
     ...
   }
   pthread_rwlock_unlock(&itemhead_rwlock);

Head removal (pop) and deallocation:

.. code-block:: c

   struct item *i;

   pthread_rwlock_rdlock(&itemhead_rwlock);
   i = itemlist_pop(&itemhead);
   pthread_rwlock_unlock(&itemhead_rwlock);

   /* i might still be visible for another thread doing an
    * for_each() (but won't be returned by another pop()) */
   ...

   pthread_rwlock_wrlock(&itemhead_rwlock);
   pthread_rwlock_unlock(&itemhead_rwlock);
   /* i now guaranteed to be gone from the list.
    * note nothing between wrlock() and unlock() */
   XFREE(MTYPE_ITEM, i);

FRR lists
---------

.. TODO::

   document

BSD lists
---------

.. TODO::

   refer to external docs
