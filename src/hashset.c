#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <string.h>

#include "hashset.h"

typedef struct entry {
  unsigned hash;
  char* data;
  struct entry* next;
} entry;

struct hashset {
  /* Array of head entries.
   * An entry is unused if entry.data == NULL.
   * The set is defunct if entries == NULL.
   */
  entry* entries;
  /* The length of the above array. This must be a power of two. */
  unsigned table_size;
  /* The bitmask to convert hashes to indices. */
  unsigned mask;
  /* The total number of entries (including links). */
  unsigned num_entries;
  /* The threshhold at which to become defunct. */
  unsigned defunct_thresh;
};

/* Calculates the Jenkins One-at-a-Time hash function for the
 * given string.
 * See: http://en.wikipedia.org/wiki/Jenkins_hash_function
 */
static unsigned hs_hash(char* str) {
  unsigned hash = 0;
  while (*str) {
    hash += *str++;
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  hash += (hash << 3);
  hash ^= (hash >> 11);
  hash += (hash << 15);
  return hash;
}

hashset hs_create(void) {
  hashset ret = malloc(sizeof(struct hashset));
  if (!ret) return ret;

  ret->table_size = 16;
  ret->mask = ret->table_size - 1;
  /* If this fails, entries is NULL, which just marks the table as defunct,
   * which is exactly what we want anyway.
   */
  ret->entries = malloc(ret->table_size*sizeof(entry));
  if (ret->entries)
    memset(ret->entries, 0, ret->table_size*sizeof(entry));
  ret->num_entries = 0;
  ret->defunct_thresh = ~0u;
  return ret;
}

static void hs_free_entries(hashset hs) {
  unsigned i;
  entry* curr, * next;
  for (i = 0; i < hs->table_size; ++i) {
    if (hs->entries[i].data) {
      /* Entry exists, free its data. */
      free(hs->entries[i].data);
      /* Now free any links it may have and their data */
      for (curr = hs->entries[i].next; curr; curr = next) {
        next = curr->next;
        free(curr->data);
        free(curr);
      }
    }
  }

  hs->entries = NULL;
}

void hs_free(hashset hs) {
  if (!hs) return;

  if (hs->entries)
    hs_free_entries(hs);

  free(hs);
}

void hs_defunct_at(hashset hs, unsigned thresh) {
  if (!hs) return;

  hs->defunct_thresh = thresh;
}

int hs_test(hashset hs, char* data) {
  unsigned hash, ix;
  entry* curr;

  /* Consider a null hashset defunct */
  if (!hs || !hs->entries) return 1;

  hash = hs_hash(data);
  ix = hash & hs->mask;

  /* If the entry does not exist, the rest of the entry struct should be
   * considered uninitialised, so return now.
   */
  if (!hs->entries[ix].data) return 0;
  /* Search each item in the chain until a match is found or the end is
   * reached. */
  for (curr = &hs->entries[ix]; curr; curr = curr->next)
    if (hash == curr->hash && 0 == strcmp(data, curr->data))
      return 1;
  return 0;
}

static void hs_put_one(hashset hs, char* data, unsigned hash) {
  unsigned ix;
  entry* new_entry;

  if (!hs || !hs->entries) return;

  /* Make a copy of the data for the table to own */
  data = strdup(data);
  if (!data) {
    /* Allocation failed, make defunct */
    hs_free_entries(hs);
    return;
  }

  ix = hash & hs->mask;
  if (!hs->entries[ix].data) {
    /* No collision, can insert directly. */
    hs->entries[ix].data = data;
    hs->entries[ix].hash = hash;
    hs->entries[ix].next = NULL;
    return;
  }

  /* Collision, must create link. */
  new_entry = malloc(sizeof(entry));
  if (!new_entry) {
    /* Allocation failed, make defunct */
    hs_free_entries(hs);
    return;
  }

  /* OK, set the new entry up and insert it. */
  new_entry->data = data;
  new_entry->hash = hash;
  new_entry->next = hs->entries[ix].next;
  hs->entries[ix].next = new_entry;
}

void hs_put(hashset hs, char* data) {
  struct hashset old;
  unsigned i;
  entry* curr;

  if (!hs || !hs->entries) return;

  /* Make defunct if will exceed limit */
  if (hs->num_entries+1 >= hs->defunct_thresh) {
    hs_free_entries(hs);
    return;
  }

  /* If this would bring the load average above 50%, double size. */
  if (hs->num_entries*2 >= hs->table_size) {
    /* Move hashset data to temporary */
    old = *hs;
    /* Double size and allocate */
    hs->table_size *= 2;
    hs->mask = hs->table_size-1;
    hs->entries = malloc(hs->table_size * sizeof(entry));
    if (hs->entries) {
      /* This part could be made more efficient by reusing the strings and even
       * some of the dynamically-allocated entries. However, this would
       * significantly complicate the code for what would not be a huge
       * performance benefit in most cases.
       */
      /* Initialise entries to zero */
      memset(hs->entries, 0, hs->table_size * sizeof(entry));
      /* Insert old entries into new table */
      for (i = 0; i < old.table_size; ++i)
        if (old.entries[i].data)
          for (curr = &old.entries[i]; curr; curr = curr->next)
            hs_put_one(hs, curr->data, curr->hash);
    }
    /* Free old data */
    hs_free_entries(&old);

    /* Return now if became defunct */
    if (!hs->entries) return;
  }

  /* OK, add single datum */
  hs_put_one(hs, data, hs_hash(data));
}
