/*
Copyright (c) 2012 Jason Lingle
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of the author nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
*/

#ifndef HASHSET_H_
#define HASHSET_H_

/* Defines a simple hashset for recording filenames.
 *
 * The hashset uses exact string comparison to check for existence (after using
 * a hash function to locate the entry in the table). When allocation fails for
 * adding a new entry to a hashset, or when the number of entries exceeds a
 * specified limit, the hashset is marked defunct. A defunct hashtable contains
 * no data but is considered to contain all strings.
 */
struct hashset;
typedef struct hashset* hashset;

/* Creates and returns a new, empty hashset.
 *
 * Returns NULL on failure.
 */
hashset __attribute__((visibility("internal"))) hs_create(void);
/* Frees the given hashset.
 *
 * Does nothing if the argument is NULL.
 */
void __attribute__((visibility("internal"))) hs_free(hashset);

/* Sets the threshold for the item count that results in the hashset becomming
 * defunct. By default, this value is ~0u (ie, the largest possible unsigned
 * integer value).
 *
 * Does nothing if the hashset is NULL.
 */
void __attribute__((visibility("internal"))) hs_defunct_at(hashset,unsigned);

/* Tests whether the given item exists in the set.
 *
 * Returns 1 if it does, 0 otherwise.
 *
 * Considers a NULL hashset defunct.
 */
int __attribute__((visibility("internal"))) hs_test(hashset, char*);

/* Adds the given item to the given set.
 *
 * If memory allocation fails, the hashset will be considered to contain all
 * possible strings.
 *
 * Does nothing on a NULL or defunct hashset.
 */
void __attribute__((visibility("internal"))) hs_put(hashset, char*);

#endif /* HASHSET_H_ */
