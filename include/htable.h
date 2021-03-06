/*
 * This file is part of libkern.
 *
 * libkern is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libkern is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libkern.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HTABLE_H_
#define HTABLE_H_

#include "jhash.h"
#include "hlist.h"
#include "kernel.h"
#include "log2.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

#define BLOOM_BIT_LENGTH 8

#define bloom_init(bitvect) ({ \
    (bitvect) = (uint8_t *)malloc((1ULL << BLOOM_BIT_LENGTH) / BITS_PER_BYTE); \
    assert((bitvect)); \
    memset((bitvect), 0, (1ULL << BLOOM_BIT_LENGTH) / BITS_PER_BYTE); })
#define bloom_finit(bitvect) free((bitvect))

#define bloom_set_bit(bits, idx) \
  (bits[(idx) / BITS_PER_BYTE] |= (1U << ((idx) % BITS_PER_BYTE)))
#define bloom_test_bit(bits, idx) \
  (bits[(idx) / BITS_PER_BYTE] & (1U << ((idx) % BITS_PER_BYTE)))

#define bloom_set(bitvect, hash) \
  bloom_set_bit((bitvect), (hash & (uint32_t)((1ULL << BLOOM_BIT_LENGTH) - 1)))
#define bloom_test(bitvect, hash) \
  bloom_test_bit((bitvect), (hash & (uint32_t)((1ULL << BLOOM_BIT_LENGTH) - 1)))

/** Default number of buckets */
#define HASH_NUM_BUCKETS 16
/** Expand when bucket count reach threshold */
#define HASH_BUCKET_THRESH 10

/** Hash table entry identified by key */
struct htable_entry {
  /** Linked list node */
  struct hlist_node node;
  /** Pointer to enclosing struct's key */
  void *key;
  /** Enclosing struct's key length */
  size_t len;
  /** Result of hash function applied to key */
  unsigned hash;
};

/** Hash table bucket */
struct htable_bucket {
  /** Head of list of entries in the bucket */
  struct hlist_head head;
  /** Number of entries in the bucket */
  size_t count;
  /** Expansion multiplier, postpones multiplication */
  unsigned expand_mult;
};

/** Hash table containing buckets full of entries */
struct htable {
  /** Buckets containing table elements */
  struct hlist_head *bucks;
  /** Number of allocated buckets */
  size_t size;
  /** Number of entries in the table */
  size_t count;
  /** Bloom filter bit vector */
  uint8_t *bitvect;
};

#define htable_which_bucket(table, hash) ((hash) & ((table)->size - 1))

/**
 * Initialize new hash table entry.
 */
static inline void INIT_HTABLE_ENTRY(struct htable_entry *entry, void *key, size_t len) {
  INIT_HLIST_NODE(&entry->node);
  entry->key = key;
  entry->len = len;
}

/**
 * Initialize new hash table.
 *
 * @param table hash table
 */
static inline int htable_init(struct htable *table) {
  if (!table) {
    return -1;
  }

  table->size = HASH_NUM_BUCKETS;
  table->bucks = (struct hlist_head *)malloc(sizeof(struct hlist_head) * table->size);
  assert(table->bucks);

  for (int i = 0; i < table->size; ++i) {
    INIT_HLIST_HEAD(&table->bucks[i]);
  }

  table->count = 0;
  bloom_init(table->bitvect);

  return 0;
}

/**
 * Initialize new table of given size.
 *
 * @param table hash table
 * @param n aproximate size
 */
static inline int htable_init_n(struct htable *table, size_t n) {
  if (!table) {
    return -1;
  }

  table->size = n > 0 ? n : HASH_BUCKET_THRESH;
  table->bucks = (struct hlist_head *)malloc(sizeof(struct hlist_head) * table->size);
  assert(table->bucks);

  for (int i = 0; i < table->size; ++i) {
    INIT_HLIST_HEAD(&table->bucks[i]);
  }
  table->count = 0;
  bloom_init(table->bitvect);

  return 0;
}

/**
 * Destroy hash table.
 *
 * @param table hash table
 */
static inline void htable_destroy(struct htable *table) {
  if (table && table->bucks) {
    free(table->bucks);
  }
  bloom_finit(table->bitvect);
}

/**
 * Add a new entry into hash table.
 *
 * @param h the hash table to insert entry into
 * @param e the hash entry
 * @param key the pointer to entry key
 * @param len the key length
 */
static inline void htable_add(struct htable *table, struct htable_entry *entry, void *key, size_t len) {
  INIT_HTABLE_ENTRY(entry, key, len);

  unsigned hash = jhash(key, len, 0);
  unsigned buck = htable_which_bucket(table, hash);
  hlist_add_head(&entry->node, &table->bucks[buck]);

  //  struct htable_bucket *bucket = &table->buckets[bucket];
  //  if (bucket->count > (bucket->expand_mult + 1) * HASH_BUCKET_THRESH && !table->noexpand)
  //      htable_expand_buckets(table);

  bloom_set(table->bitvect, hash);

  table->count++;
}

/**
 * Looks up the hash table for the presence of key.
 *
 * @param h the hash table to look into
 * @param key the key to look for
 * @param len the length of the key
 * @return a pointer to the entry that matches the key, NULL otherwise
 */
static inline struct htable_entry *htable_find(const struct htable *h, const void *key, size_t len) {
  struct htable_entry *e;
  struct hlist_node *n;

  unsigned hash = jhash(key, len, 0);
  unsigned buck = htable_which_bucket(h, hash);

  if (bloom_test(h->bitvect, hash)) {
    if (!hlist_empty(&h->bucks[buck])) {
      hlist_for_each_entry(e, n, &h->bucks[buck], node) {
        if (e->len == len && memcmp(e->key, key, len) == 0) {
          return e;
        }
      }
    }
  }
  return NULL;
}

static inline struct htable_entry *htable_del_key(struct htable *table, const void *key, size_t len) {
  struct htable_entry *entry;

  if ((entry = htable_find(table, key, len))) {
    hlist_del_init(&entry->node);
    table->count--;

    return entry;
  }
  return NULL;
}

static inline struct htable_entry *htable_del_entry(struct htable *table, struct htable_entry *entry) {
  return htable_del_key(table, entry->key, entry->len);
}

/**
 * Get the user data for this entry.
 *
 * @param ptr the hash table pointer
 * @param type the type of the user data embedded in this entry
 * @param member the name of the entry within the struct
 */
#define hash_entry(ptr, type, member) \
  container_of(ptr, type, member)

/**
 * Looks up the hash table for the presence of key.
 *
 * @param member the name of the entry within the struct
 */
#define hash_find_entry(table, key, len, type, member) ({ \
    struct htable_entry *e = htable_find((table), (key), (len)); \
    (type *)(e ? hash_entry(e, type, member) : NULL); })

/**
 * Iterate over hash table elements.
 *
 * @param pos struct htable entry to use as a loop counter
 * @param table your table
 */
#define htable_for_each(pos, table) \
  for (int i = 0; i < (table)->size; ++i) \
    for (pos = hlist_entry((table)->bucks[i].first, typeof(*pos), node); \
         pos; pos = hlist_entry(pos->node.next, typeof(*pos), node))

/**
 * Iterate over hash table elements safe against removal of table entry.
 *
 * @param pos struct htable entry to use as a loop counter
 * @param n another struct htable entry to use as temporary storage
 * @param table your table
 */
#define htable_for_each_safe(pos, n, table) \
  for (int i = 0; i < (table)->size; ++i) \
    for (pos = hlist_entry((table)->bucks[i].first, typeof(*pos), node); \
         pos && ({ n = hlist_entry(pos->node.next, typeof(*pos), node); 1; }); \
         pos = n)

/**
 * Iterate over hash table elements of given type.
 *
 * @param tpos type pointer to use as a loop cursor
 * @param pos entry pointer to use as a loop cursor
 * @param table your table
 * @param member the name of the enry within the struct
 */
#define htable_for_each_entry(tpos, pos, table, member) \
  for (int i = 0; i < (table)->size; ++i) \
    for (pos = hlist_entry((table)->bucks[i].first, typeof(*pos), node); \
         pos && ({ tpos = hash_entry(pos, typeof(*tpos), member); 1;}); \
         pos = hlist_entry(pos->node.next, typeof(*pos), node))

/**
 * Iterate over hash table elements of given type safe against removal of table entry.
 *
 * @param tpos type pointer to use as a loop cursor
 * @param pos entry pointer to use as a loop cursor
 * @param table your table
 * @param member the name of the enry within the struct
 */
#define htable_for_each_entry_safe(tpos, pos, n, table, member) \
  for (int i = 0; i < (table)->size; ++i) \
    for (pos = hlist_entry((table)->bucks[i].first, typeof(*pos), node); \
         pos && ({ n = hlist_entry(pos->node.next, typeof(*pos), node); 1; }) && ({ tpos = hash_entry(pos, typeof(*tpos), member); 1;}); \
         pos = n)

#endif // HTABLE_H_
