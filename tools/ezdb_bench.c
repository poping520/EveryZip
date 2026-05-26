#include "../src/ezdb/ezdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct SearchStats {
    uint32_t printed;
    uint32_t total;
} SearchStats;

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  EzdbBench build <input.txt> <output.ezdb>\n");
    printf("  EzdbBench info <db.ezdb>\n");
    printf("  EzdbBench get <db.ezdb> <id>\n");
    printf("  EzdbBench search <db.ezdb> <keyword> [limit]\n");
}

static void on_result(const EzdbSearchResult* result, void* user_data)
{
    SearchStats* stats = (SearchStats*)user_data;
    ++stats->total;
    if (stats->printed < 20) {
        printf("[%u] %s, %llu, %llu\n",
               result->id,
               result->path,
               (unsigned long long)result->size,
               (unsigned long long)result->modified_time);
        ++stats->printed;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "build") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        double start = now_ms();
        int rc = ezdb_build_from_text(argv[2], argv[3]);
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "build failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        printf("build ok: %.2f ms\n", elapsed);
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbStats stats;
        rc = ezdb_stats(db, &stats);
        if (rc != 0) {
            fprintf(stderr, "stats failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("records: %u\n", stats.record_count);
        printf("active: %u\n", stats.active_count);
        printf("file_size: %llu bytes\n", (unsigned long long)stats.file_size);
        printf("records_size: %llu bytes\n", (unsigned long long)stats.records_size);
        printf("dirs_size: %llu bytes\n", (unsigned long long)stats.dirs_size);
        printf("names_size: %llu bytes\n", (unsigned long long)stats.names_size);
        printf("index_size: %llu bytes\n", (unsigned long long)stats.index_size);
        printf("postings_size: %llu bytes\n", (unsigned long long)stats.postings_size);
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbSearchResult result;
        rc = ezdb_get_by_id(db, (uint32_t)strtoul(argv[3], NULL, 10), &result);
        if (rc != 0) {
            fprintf(stderr, "get failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("[%u] %s, %llu, %llu\n",
               result.id,
               result.path,
               (unsigned long long)result.size,
               (unsigned long long)result.modified_time);
        ezdb_free_result(&result);
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc < 4 || argc > 5) {
            print_usage();
            return 1;
        }
        uint32_t limit = argc == 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 100;
        Ezdb* db = NULL;
        double open_start = now_ms();
        int rc = ezdb_open(argv[2], &db);
        double open_elapsed = now_ms() - open_start;
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }

        SearchStats stats;
        memset(&stats, 0, sizeof(stats));
        double search_start = now_ms();
        rc = ezdb_search_path(db, argv[3], limit, on_result, &stats);
        double search_elapsed = now_ms() - search_start;
        if (rc != 0) {
            fprintf(stderr, "search failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("open_ms: %.2f\n", open_elapsed);
        printf("search_ms: %.2f\n", search_elapsed);
        printf("returned: %u\n", stats.total);
        ezdb_close(db);
        return 0;
    }

    print_usage();
    return 1;
}
