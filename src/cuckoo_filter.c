
#include <errno.h>
#include "cuckoo_filter.h"

#define CUCKOO_NESTS_PER_BUCKET     4

typedef struct {
  uint16_t              fingerprint;
} __attribute__((packed)) cuckoo_nest_t;

typedef struct {
  uint32_t              fingerprint;
  uint32_t              h1;
  uint32_t              h2;
  uint32_t              padding;
} __attribute__((packed)) cuckoo_item_t;

typedef struct {
  bool                  was_found;
  cuckoo_item_t         item;
} cuckoo_result_t;

struct cuckoo_filter_t {
  uint32_t              bucket_count;
  uint32_t              nests_per_bucket;
  uint32_t              mask;
  uint32_t              max_kick_attempts;
  uint32_t              seed;
  uint32_t              padding;
  size_t                mem_size;
  size_t	              array_mem_size;
  cuckoo_item_t         victim;
  cuckoo_item_t        *last_victim;
  cuckoo_nest_t         bucket[1];
} __attribute__((packed));

/* ------------------------------------------------------------------------- */

static inline size_t
next_power_of_two (size_t x) {
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;

  if (8 == sizeof(size_t)) {
    x |= x >> 32;
  }

  return ++x;
}

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */

static inline uint32_t
murmurhash (
  const void           *key,
  uint32_t              key_length_in_bytes,
  uint32_t              seed
) {
  uint32_t              c1 = 0xcc9e2d51;
  uint32_t              c2 = 0x1b873593;
  uint32_t              r1 = 15;
  uint32_t              r2 = 13;
  uint32_t              m = 5;
  uint32_t              n = 0xe6546b64;
  uint32_t              h = 0;
  uint32_t              k = 0;
  uint8_t              *d = (uint8_t *) key;
  const uint32_t       *chunks = NULL;
  const uint8_t        *tail = NULL;
  int                   i = 0;
  int                   l = (key_length_in_bytes / sizeof(uint32_t));

  h = seed;

  chunks = (const uint32_t *) (d + l * sizeof(uint32_t));
  tail = (const uint8_t *) (d + l * sizeof(uint32_t));

  for (i = -l; i != 0; ++i) {
    k = chunks[i];
    k *= c1;
    k = (k << r1) | (k >> (32 - r1));
    k *= c2;
    h ^= k;
    h = (h << r2) | (h >> (32 - r2));
    h = h * m + n;
  }

  k = 0;
  switch (key_length_in_bytes & 3) {
    case 3: k ^= (tail[2] << 16);
    case 2: k ^= (tail[1] << 8);
    case 1:
      k ^= tail[0];
      k *= c1;
      k = (k << r1) | (k >> (32 - r1));
      k *= c2;
      h ^= k;
  }

  h ^= key_length_in_bytes;
  h ^= (h >> 16);
  h *= 0x85ebca6b;
  h ^= (h >> 13);
  h *= 0xc2b2ae35;
  h ^= (h >> 16);

  return h;

} /* murmurhash() */

/* ------------------------------------------------------------------------- */

static inline uint32_t
hash (
  const void           *key,
  uint32_t              key_length_in_bytes,
  uint32_t              size,
  uint32_t              n,
  uint32_t              seed
) {
  uint32_t h1 = murmurhash(key, key_length_in_bytes, seed);
  uint32_t h2 = murmurhash(key, key_length_in_bytes, h1);

  return ((h1 + (n * h2)) % size);

} /* hash() */

/* ------------------------------------------------------------------------- */

static inline uint32_t
get_fingerprint (
  cuckoo_filter_t      *filter,
  const void*           key,
  uint32_t              key_length_in_bytes
) {
  uint32_t h =  hash(key, key_length_in_bytes, filter->bucket_count,
    1000, filter->seed) & filter->mask;

  // We use fingerprint 0 to indicate an empty slot, so this is a forbidden
  // value for any given key.
  return h + (h == 0);

} /* get_fingerprint() */

/* ------------------------------------------------------------------------- */

static inline uint32_t
get_alt_bucket (
  cuckoo_filter_t      *filter,
  uint32_t              fp,
  uint32_t              h
) {
  return ((h ^ hash(&fp, sizeof(fp),
    filter->bucket_count, 900, filter->seed)) % filter->bucket_count);

} /* get_alt_bucket() */

/* ------------------------------------------------------------------------- */

static inline CUCKOO_FILTER_RETURN
add_fingerprint_to_bucket (
  cuckoo_filter_t      *filter,
  uint32_t              fp,
  uint32_t              h
) {
  for (size_t ii = 0; ii < filter->nests_per_bucket; ++ii) {
    cuckoo_nest_t *nest =
      &filter->bucket[(h * filter->nests_per_bucket) + ii];
    if (0 == nest->fingerprint) {
      nest->fingerprint = fp;
      return CUCKOO_FILTER_OK;
    }
  }

  return CUCKOO_FILTER_FULL;

} /* add_fingerprint_to_bucket() */

/* ------------------------------------------------------------------------- */

static inline CUCKOO_FILTER_RETURN
remove_fingerprint_from_bucket (
  cuckoo_filter_t      *filter,
  uint32_t              fp,
  uint32_t              h
) {
  for (size_t ii = 0; ii < filter->nests_per_bucket; ++ii) {
    cuckoo_nest_t *nest =
      &filter->bucket[(h * filter->nests_per_bucket) + ii];
    if (fp == nest->fingerprint) {
      nest->fingerprint = 0;
      return CUCKOO_FILTER_OK;
    }
  }

  return CUCKOO_FILTER_NOT_FOUND;

} /* remove_fingerprint_from_bucket() */

/* ------------------------------------------------------------------------- */

static inline CUCKOO_FILTER_RETURN
cuckoo_filter_move (
  cuckoo_filter_t      *filter,
  uint32_t              fingerprint,
  uint32_t              h1,
  int                   depth
) {
  uint32_t h2 = ((h1 ^ hash(&fingerprint, sizeof(fingerprint),
    filter->bucket_count, 900, filter->seed)) % filter->bucket_count);

  if (CUCKOO_FILTER_OK == add_fingerprint_to_bucket(filter,
    fingerprint, h1)) {
    return CUCKOO_FILTER_OK;
  }

  if (CUCKOO_FILTER_OK == add_fingerprint_to_bucket(filter,
    fingerprint, h2)) {
    return CUCKOO_FILTER_OK;
  }

  if (filter->max_kick_attempts == depth) {
    return CUCKOO_FILTER_FULL;
  }

  size_t row = (0 == (rand() % 2) ? h1 : h2);
  size_t col = (rand() % filter->nests_per_bucket);
  size_t elem =
    filter->bucket[(row * filter->nests_per_bucket) + col].fingerprint;
  filter->bucket[(row * filter->nests_per_bucket) + col].fingerprint =
    fingerprint;

  return cuckoo_filter_move(filter, elem, row, (depth + 1));

} /* cuckoo_filter_move() */

/* ------------------------------------------------------------------------- */

CUCKOO_FILTER_RETURN
cuckoo_filter_new (
  cuckoo_filter_t     **filter,
  size_t                max_key_count,
  size_t                max_kick_attempts,
  uint32_t              seed
) {
  cuckoo_filter_t      *new_filter;

  size_t bucket_count =
    next_power_of_two(max_key_count / CUCKOO_NESTS_PER_BUCKET);
  if (0.96 < (double) max_key_count / bucket_count / CUCKOO_NESTS_PER_BUCKET) {
    bucket_count <<= 1;
  }

  size_t array_in_bytes = (bucket_count * CUCKOO_NESTS_PER_BUCKET * sizeof(cuckoo_nest_t));
  size_t allocation_in_bytes = (sizeof(cuckoo_filter_t) + array_in_bytes);

  if (0 != posix_memalign((void **) &new_filter, sizeof(uint64_t),
    allocation_in_bytes)) {
    return CUCKOO_FILTER_ALLOCATION_FAILED;
  }

  memset(new_filter, 0, allocation_in_bytes);

  new_filter->mem_size = allocation_in_bytes;
  new_filter->array_mem_size = array_in_bytes;
  new_filter->last_victim = NULL;
  memset(&new_filter->victim, 0, sizeof(new_filter)->victim);
  new_filter->bucket_count = bucket_count;
  new_filter->nests_per_bucket = CUCKOO_NESTS_PER_BUCKET;
  new_filter->max_kick_attempts = max_kick_attempts;
  new_filter->seed = (size_t) time(NULL);
  //new_filter->seed = (size_t) 10301212;
  new_filter->mask = (uint32_t) ((1U << (sizeof(cuckoo_nest_t) * 8)) - 1);

  *filter = new_filter;

  return CUCKOO_FILTER_OK;

} /* cuckoo_filter_new() */

/* ------------------------------------------------------------------------- */

CUCKOO_FILTER_RETURN
cuckoo_filter_free (
  cuckoo_filter_t     **filter
) {
  free(*filter);
  *filter = NULL;

  return CUCKOO_FILTER_OK;
}

/* ------------------------------------------------------------------------- */

static inline CUCKOO_FILTER_RETURN
cuckoo_filter_lookup (
  cuckoo_filter_t      *filter,
  cuckoo_result_t      *result,
  void                 *key,
  size_t                key_length_in_bytes
) {
  uint32_t fingerprint = get_fingerprint(filter, key, key_length_in_bytes);
  uint32_t h1 = hash(key, key_length_in_bytes, filter->bucket_count, 0,
    filter->seed);
  uint32_t h2 = ((h1 ^ hash(&fingerprint, sizeof(fingerprint),
    filter->bucket_count, 900, filter->seed)) % filter->bucket_count);

  result->was_found = false;
  result->item.fingerprint = 0;
  result->item.h1 = 0;
  result->item.h2 = 0;

  for (size_t ii = 0; ii < filter->nests_per_bucket; ++ii) {
    cuckoo_nest_t *n1 =
      &filter->bucket[(h1 * filter->nests_per_bucket) + ii];
    if (fingerprint == n1->fingerprint) {
      result->was_found = true;
      break;
    }

    cuckoo_nest_t *n2 =
      &filter->bucket[(h2 * filter->nests_per_bucket) + ii];
    if (fingerprint == n2->fingerprint) {
      result->was_found = true;
      break;
    }
  }

  result->item.fingerprint = fingerprint;
  result->item.h1 = h1;
  result->item.h2 = h2;

  return ((true == result->was_found)
    ? CUCKOO_FILTER_OK : CUCKOO_FILTER_NOT_FOUND);

} /* cuckoo_filter_lookup() */

/* ------------------------------------------------------------------------- */

CUCKOO_FILTER_RETURN
cuckoo_filter_add (
  cuckoo_filter_t      *filter,
  void                 *key,
  size_t                key_length_in_bytes
) {
  cuckoo_result_t   result;
  uint32_t fingerprint = get_fingerprint(filter, key, key_length_in_bytes);
  uint32_t h1, h2;
  size_t slot, aux;
  int kick_count = 0;

  h1 = hash(key, key_length_in_bytes, filter->bucket_count, 0,
  filter->seed);

  if (CUCKOO_FILTER_OK == add_fingerprint_to_bucket(filter,
    fingerprint, h1)) {
    return CUCKOO_FILTER_OK;
  }

  h2 = ((h1 ^ hash(&fingerprint, sizeof(fingerprint),
    filter->bucket_count, 900, filter->seed)) % filter->bucket_count);

  if (CUCKOO_FILTER_OK == add_fingerprint_to_bucket(filter,
    fingerprint, h2)) {
    return CUCKOO_FILTER_OK;
  }

  // Choose bucket to kick element from
  h2 = (0 == (rand() % 2) ? h1 : h2);

  while(kick_count <= filter->max_kick_attempts){

    // Choose element to kick out
    slot = (rand() % filter->nests_per_bucket);

    // Swap kicked element with new one
    aux = filter->bucket[(h2 * filter->nests_per_bucket) + slot].fingerprint;
    filter->bucket[(h2 * filter->nests_per_bucket) + slot].fingerprint =
      fingerprint;
    fingerprint = aux;

    h2 = ((h2 ^ hash(&fingerprint, sizeof(fingerprint),
    filter->bucket_count, 900, filter->seed)) % filter->bucket_count);

    if (CUCKOO_FILTER_OK == add_fingerprint_to_bucket(filter,
      fingerprint, h2)) {
      return CUCKOO_FILTER_OK;
    }
  }

  return CUCKOO_FILTER_FULL;

} /* cuckoo_filter_add() */

/* ------------------------------------------------------------------------- */

CUCKOO_FILTER_RETURN
cuckoo_filter_remove (
  cuckoo_filter_t      *filter,
  void                 *key,
  size_t                key_length_in_bytes
) {
  bool              was_deleted = false;

  uint32_t h1 = hash(key, key_length_in_bytes, filter->bucket_count, 0,
    filter->seed);
  uint32_t fingerprint = get_fingerprint(filter,key,key_length_in_bytes);

  if (CUCKOO_FILTER_OK == remove_fingerprint_from_bucket(filter,
    fingerprint, h1)) {
    was_deleted = true;
  } else if (CUCKOO_FILTER_OK == remove_fingerprint_from_bucket(filter,
    fingerprint, get_alt_bucket(filter,fingerprint,h1))) {
    was_deleted = true;
  }

  if ((true == was_deleted) & (NULL != filter->last_victim)) {

  }

  return ((true == was_deleted) ? CUCKOO_FILTER_OK : CUCKOO_FILTER_NOT_FOUND);

} /* cuckoo_filter_remove() */

/* ------------------------------------------------------------------------- */

CUCKOO_FILTER_RETURN
cuckoo_filter_contains (
  cuckoo_filter_t      *filter,
  void                 *key,
  size_t                key_length_in_bytes
) {
  cuckoo_result_t   result;

  return cuckoo_filter_lookup(filter, &result, key, key_length_in_bytes);

} /* cuckoo_filter_contains() */

/* ------------------------------------------------------------------------- */

size_t
cuckoo_filter_memsize (
  cuckoo_filter_t      *filter
) {
  return filter->mem_size;
} /* cuckoo_filter_memsize() */

/* ------------------------------------------------------------------------- */

// This function stores the filter in a file and clears its contents
void
cuckoo_filter_store_and_clean (
  cuckoo_filter_t      *filter,
  char                 *filename
) {
  FILE *fout;
  
  // TODO: Maybe we can make this faster
  //sprintf(filename,"%s_%d.cuckoo",basename,id);
  fout = fopen(filename,"wb");

  // Write entire filter to file
  fwrite(filter,sizeof(char),filter->mem_size,fout);
 
  // Clear the filter here (only the buckets)
  memset(filter->bucket, 0, filter->array_mem_size);

  fclose(fout);

} /* cuckoo_filter_store_and_clean() */

/* ------------------------------------------------------------------------- */

// This function loads a filter structure from a binary file
CUCKOO_FILTER_RETURN
cuckoo_filter_load (
  cuckoo_filter_t      **filter,
  char                 *filename
) {
  FILE *fin;
  long filesize;

  fin = fopen(filename,"rb");
  if(fin == NULL)
    return CUCKOO_FILTER_NOT_FOUND;

  // Get file size
  fseek(fin, 0L, SEEK_END);
  filesize = ftell(fin);
  fseek(fin, 0, SEEK_SET); // Go to beginning

  // Try to allocate memory for the filter
  if (0 != posix_memalign((void **) filter, sizeof(uint64_t),
    filesize)) {
    // switch(errno){
    //   case ENOMEM:
    //     printf("Error: Insufficient memory to allocate filter\n");
    //     break;
    //   case EINVAL:
    //     printf("Error: invalid arguments\n");
    //     break;
    //   default:
    //     printf("Error: Something else...\n");
    //     break;
    // }
    return CUCKOO_FILTER_ALLOCATION_FAILED;
  }

  // Read entire filter from file
  fread((void*) *filter,sizeof(char),filesize,fin);

  fclose(fin);

  return CUCKOO_FILTER_OK;

} /* cuckoo_filter_load() */

/* ------------------------------------------------------------------------- */
/* https://stackoverflow.com/questions/7775991/how-to-get-hexdump-of-a-structure-data */

void
hexDump (
  char      *desc,
  void      *addr,
  int	      len
) {
  int i;
  unsigned char buff[17];
  unsigned char *pc = (unsigned char*)addr;

  // Output description if given.
  if (desc != NULL)
      printf ("%s:\n", desc);

  if (len == 0) {
      printf("  ZERO LENGTH\n");
      return;
  }
  if (len < 0) {
      printf("  NEGATIVE LENGTH: %i\n",len);
      return;
  }

  // Process every byte in the data.
  for (i = 0; i < len; i++) {
      // Multiple of 16 means new line (with line offset).

      if ((i % 16) == 0) {
          // Just don't print ASCII for the zeroth line.
          if (i != 0)
              printf ("  %s\n", buff);

          // Output the offset.
          printf ("  %04x ", i);
      }

      // Now the hex code for the specific character.
      printf (" %02x", pc[i]);

      // And store a printable ASCII character for later.
      if ((pc[i] < 0x20) || (pc[i] > 0x7e))
          buff[i % 16] = '.';
      else
          buff[i % 16] = pc[i];
      buff[(i % 16) + 1] = '\0';
  }

  // Pad out last line if not exactly 16 characters.
  while ((i % 16) != 0) {
      printf ("   ");
      i++;
  }

  // And print the final ASCII bit.
  printf ("  %s\n", buff);
}

/* ------------------------------------------------------------------------- */

void
cuckoo_filter_hexdump (
  cuckoo_filter_t      *filter
) {
  hexDump ("cuckoo_filter", filter, filter->mem_size);
}
