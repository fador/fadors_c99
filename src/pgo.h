#ifndef PGO_H
#define PGO_H

#include <stdint.h>
#include <stddef.h>

/*
 * PGO profile data structures and I/O.
 *
 * Binary profile format (default.profdata):
 *   Header:  "PGO1" (4 bytes magic) + uint32_t num_entries
 *   Entry[]: 64 bytes name (null-padded) + uint64_t count
 *
 * Entry name conventions:
 *   "funcname"       — function entry counter
 *   "funcname:B0T"   — branch 0, taken counter
 *   "funcname:B0N"   — branch 0, not-taken counter
 */

#define PGO_MAGIC      "PGO1"
#define PGO_MAGIC_SIZE 4
#define PGO_NAME_SIZE  64
#define PGO_ENTRY_SIZE (PGO_NAME_SIZE + 8)  /* 72 bytes per entry */

typedef struct {
    char name[PGO_NAME_SIZE];
    uint64_t count;
} PGOEntry;

typedef struct {
    PGOEntry *entries;
    int entry_count;
    uint64_t max_func_count; /* highest function entry count (for hot/cold classification) */
} PGOProfile;

/* Load profile from binary file.  Returns NULL on error. */
PGOProfile *pgo_load_profile(const char *filename);

/* Free profile data. */
void pgo_free_profile(PGOProfile *prof);

/* Query function execution count (0 if not found). */
uint64_t pgo_func_count(PGOProfile *prof, const char *func_name);

/* Return 1 if function is "hot" (count >= 10% of max). */
int pgo_is_hot(PGOProfile *prof, const char *func_name);

/* Return 1 if function is "cold" (count <= 1% of max, or zero). */
int pgo_is_cold(PGOProfile *prof, const char *func_name);

/* Query branch taken probability (0.0–1.0).  Returns 0.5 if not found. */
double pgo_branch_probability(PGOProfile *prof, const char *func_name, int branch_id);

#endif /* PGO_H */
