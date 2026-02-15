/*
 * pgo.c — Profile-Guided Optimization data I/O.
 *
 * Reads and writes binary profile data files produced by instrumented binaries
 * compiled with -fprofile-generate.  Used by -fprofile-use to guide inlining
 * and branch prediction decisions.
 */

#include "pgo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PGOProfile *pgo_load_profile(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "[PGO] Cannot open profile: %s\n", filename);
        return NULL;
    }

    /* Read and verify magic */
    char magic[PGO_MAGIC_SIZE];
    if (fread(magic, 1, PGO_MAGIC_SIZE, f) != PGO_MAGIC_SIZE ||
        memcmp(magic, PGO_MAGIC, PGO_MAGIC_SIZE) != 0) {
        fprintf(stderr, "[PGO] Invalid profile magic in %s\n", filename);
        fclose(f);
        return NULL;
    }

    /* Read entry count */
    uint32_t num_entries = 0;
    if (fread(&num_entries, 4, 1, f) != 1) {
        fprintf(stderr, "[PGO] Truncated profile header in %s\n", filename);
        fclose(f);
        return NULL;
    }

    if (num_entries > 100000) {
        fprintf(stderr, "[PGO] Suspiciously large entry count: %u\n", num_entries);
        fclose(f);
        return NULL;
    }

    PGOProfile *prof = (PGOProfile *)calloc(1, sizeof(PGOProfile));
    prof->entry_count = (int)num_entries;
    prof->entries = (PGOEntry *)calloc(num_entries, sizeof(PGOEntry));

    /* Read entries */
    uint64_t max_func = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        if (fread(prof->entries[i].name, 1, PGO_NAME_SIZE, f) != PGO_NAME_SIZE) {
            fprintf(stderr, "[PGO] Truncated entry %u in %s\n", i, filename);
            break;
        }
        prof->entries[i].name[PGO_NAME_SIZE - 1] = '\0'; /* safety */
        if (fread(&prof->entries[i].count, 8, 1, f) != 1) {
            fprintf(stderr, "[PGO] Truncated counter %u in %s\n", i, filename);
            break;
        }
        /* Track max for function entries (no ':' in name) */
        if (!strchr(prof->entries[i].name, ':')) {
            if (prof->entries[i].count > max_func)
                max_func = prof->entries[i].count;
        }
    }
    prof->max_func_count = max_func;

    fclose(f);
    return prof;
}

void pgo_free_profile(PGOProfile *prof) {
    if (!prof) return;
    free(prof->entries);
    free(prof);
}

uint64_t pgo_func_count(PGOProfile *prof, const char *func_name) {
    if (!prof) return 0;
    for (int i = 0; i < prof->entry_count; i++) {
        if (strcmp(prof->entries[i].name, func_name) == 0)
            return prof->entries[i].count;
    }
    return 0;
}

int pgo_is_hot(PGOProfile *prof, const char *func_name) {
    if (!prof || prof->max_func_count == 0) return 0;
    uint64_t count = pgo_func_count(prof, func_name);
    /* Hot = at least 10% of the max function count */
    return count >= prof->max_func_count / 10;
}

int pgo_is_cold(PGOProfile *prof, const char *func_name) {
    if (!prof) return 0;
    uint64_t count = pgo_func_count(prof, func_name);
    if (count == 0) return 1;
    if (prof->max_func_count == 0) return 0;
    /* Cold = at most 1% of max function count */
    return count <= prof->max_func_count / 100;
}

double pgo_branch_probability(PGOProfile *prof, const char *func_name, int branch_id) {
    if (!prof) return 0.5;

    char taken_key[PGO_NAME_SIZE];
    char not_taken_key[PGO_NAME_SIZE];
    snprintf(taken_key, PGO_NAME_SIZE, "%s:B%dT", func_name, branch_id);
    snprintf(not_taken_key, PGO_NAME_SIZE, "%s:B%dN", func_name, branch_id);

    uint64_t taken = 0, not_taken = 0;
    for (int i = 0; i < prof->entry_count; i++) {
        if (strcmp(prof->entries[i].name, taken_key) == 0)
            taken = prof->entries[i].count;
        else if (strcmp(prof->entries[i].name, not_taken_key) == 0)
            not_taken = prof->entries[i].count;
    }

    uint64_t total = taken + not_taken;
    if (total == 0) return 0.5;
    return (double)taken / (double)total;
}
