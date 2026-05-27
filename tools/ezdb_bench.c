#include "../src/ezdb/ezdb.h"

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

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
    printf("  EzdbBench open <db.ezdb> [limit]\n");
    printf("  EzdbBench insert <db.ezdb> <path> [size] [mtime]\n");
    printf("  EzdbBench update <db.ezdb> <id> <path> [size] [mtime]\n");
    printf("  EzdbBench delete <db.ezdb> <id>\n");
    printf("  EzdbBench crud <input.txt> <output.ezdb>\n");
}

static void print_memory_usage(const char* prefix)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);

    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
        printf("%s_memory_error: %lu\n", prefix, GetLastError());
        return;
    }

    printf("%s_working_set_mb: %.2f\n", prefix, (double)counters.WorkingSetSize / 1024.0 / 1024.0);
    printf("%s_peak_working_set_mb: %.2f\n", prefix, (double)counters.PeakWorkingSetSize / 1024.0 / 1024.0);
    printf("%s_private_mb: %.2f\n", prefix, (double)counters.PrivateUsage / 1024.0 / 1024.0);
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

static int run_search_once(Ezdb* db, const char* keyword, uint32_t limit, const char* memory_prefix)
{
    SearchStats stats;
    memset(&stats, 0, sizeof(stats));
    double search_start = now_ms();
    int rc = ezdb_search_path(db, keyword, limit, on_result, &stats);
    double search_elapsed = now_ms() - search_start;
    if (rc != 0) {
        fprintf(stderr, "search failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("search_ms: %.2f\n", search_elapsed);
    printf("returned: %u\n", stats.total);
    print_memory_usage(memory_prefix);
    return 0;
}

typedef struct FindIdStats {
    uint32_t wanted;
    uint32_t found;
} FindIdStats;

static void on_find_id(const EzdbSearchResult* result, void* user_data)
{
    FindIdStats* stats = (FindIdStats*)user_data;
    if (result->id == stats->wanted) stats->found = 1;
}

static int search_contains_id(Ezdb* db, const char* keyword, uint32_t id, int* out_found)
{
    FindIdStats stats;
    stats.wanted = id;
    stats.found = 0;
    int rc = ezdb_search_path(db, keyword, 0, on_find_id, &stats);
    if (rc != 0) return rc;
    *out_found = stats.found ? 1 : 0;
    return 0;
}

static int expect_search_id(Ezdb* db, const char* keyword, uint32_t id, int expected)
{
    int found = 0;
    int rc = search_contains_id(db, keyword, id, &found);
    if (rc != 0) return rc;
    if (found != expected) {
        fprintf(stderr, "verification failed: keyword '%s' id %u expected %d got %d\n", keyword, id, expected, found);
        return 3;
    }
    return 0;
}

static uint64_t parse_u64_arg(const char* text, uint64_t fallback)
{
    if (!text) return fallback;
    return (uint64_t)_strtoui64(text, NULL, 10);
}

static int run_crud_bench(const char* input_txt, const char* output_ezdb)
{
    DeleteFileA(output_ezdb);
    double start = now_ms();
    int rc = ezdb_build_from_text(input_txt, output_ezdb);
    double build_elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "build failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return 2;
    }
    printf("crud_build_ms: %.2f\n", build_elapsed);
    print_memory_usage("crud_build");

    Ezdb* db = NULL;
    start = now_ms();
    rc = ezdb_open(output_ezdb, &db);
    double open_elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return 2;
    }
    printf("crud_open_ms: %.2f\n", open_elapsed);
    printf("crud_baseline_count: %u\n", ezdb_count(db));
    printf("crud_baseline_active: %u\n", ezdb_active_count(db));
    printf("crud_baseline_file_size: %llu\n", (unsigned long long)ezdb_file_size(db));
    run_search_once(db, "device", 20, "crud_baseline_device");
    run_search_once(db, "config", 20, "crud_baseline_config");
    run_search_once(db, "zzznotfoundzzz", 20, "crud_baseline_notfound");
    run_search_once(db, "a", 20, "crud_baseline_a");

    uint32_t baseline_count = ezdb_count(db);
    uint32_t baseline_active = ezdb_active_count(db);
    EzdbFileRecord insert_rec = {"Z:\\EzdbCrud\\new_device_alpha.txt", 123, 456};
    uint32_t insert_id = 0;
    start = now_ms();
    rc = ezdb_insert(db, &insert_rec, &insert_id);
    printf("crud_insert_ms: %.2f\n", now_ms() - start);
    if (rc != 0 || insert_id != baseline_count || ezdb_active_count(db) != baseline_active + 1u) {
        fprintf(stderr, "insert verification failed: %s (%d), id=%u\n", ezdb_error_message(rc), rc, insert_id);
        ezdb_close(db);
        return 2;
    }
    rc = expect_search_id(db, "new_device_alpha", insert_id, 1);
    if (rc != 0) {
        ezdb_close(db);
        return rc;
    }

    EzdbFileRecord update_insert_rec = {"Z:\\EzdbCrud\\renamed_config_beta.txt", 789, 987};
    start = now_ms();
    rc = ezdb_update(db, insert_id, &update_insert_rec);
    printf("crud_update_insert_ms: %.2f\n", now_ms() - start);
    if (rc != 0) {
        fprintf(stderr, "update inserted failed: %s (%d)\n", ezdb_error_message(rc), rc);
        ezdb_close(db);
        return 2;
    }
    rc = expect_search_id(db, "new_device_alpha", insert_id, 0);
    if (rc == 0) rc = expect_search_id(db, "renamed_config_beta", insert_id, 1);
    if (rc != 0) {
        ezdb_close(db);
        return rc;
    }

    EzdbFileRecord update_base_rec = {"Z:\\EzdbCrud\\base_device_update.txt", 111, 222};
    start = now_ms();
    rc = ezdb_update(db, 0, &update_base_rec);
    printf("crud_update_base_ms: %.2f\n", now_ms() - start);
    if (rc != 0) {
        fprintf(stderr, "update base failed: %s (%d)\n", ezdb_error_message(rc), rc);
        ezdb_close(db);
        return 2;
    }
    rc = expect_search_id(db, "base_device_update", 0, 1);
    if (rc != 0) {
        ezdb_close(db);
        return rc;
    }

    start = now_ms();
    rc = ezdb_delete(db, insert_id);
    printf("crud_delete_ms: %.2f\n", now_ms() - start);
    if (rc != 0 || ezdb_active_count(db) != baseline_active) {
        fprintf(stderr, "delete failed: %s (%d)\n", ezdb_error_message(rc), rc);
        ezdb_close(db);
        return 2;
    }
    EzdbSearchResult result;
    rc = ezdb_get_by_id(db, insert_id, &result);
    if (rc != -5) {
        fprintf(stderr, "deleted get expected not found, got %s (%d)\n", ezdb_error_message(rc), rc);
        if (rc == 0) ezdb_free_result(&result);
        ezdb_close(db);
        return 2;
    }
    rc = expect_search_id(db, "renamed_config_beta", insert_id, 0);
    if (rc != 0) {
        ezdb_close(db);
        return rc;
    }

    printf("crud_after_count: %u\n", ezdb_count(db));
    printf("crud_after_active: %u\n", ezdb_active_count(db));
    printf("crud_after_file_size: %llu\n", (unsigned long long)ezdb_file_size(db));
    print_memory_usage("crud_after");
    ezdb_close(db);

    start = now_ms();
    rc = ezdb_open(output_ezdb, &db);
    printf("crud_reopen_ms: %.2f\n", now_ms() - start);
    if (rc != 0) {
        fprintf(stderr, "reopen failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return 2;
    }
    if (ezdb_count(db) != baseline_count + 1u || ezdb_active_count(db) != baseline_active) {
        fprintf(stderr, "reopen count mismatch: count=%u active=%u\n", ezdb_count(db), ezdb_active_count(db));
        ezdb_close(db);
        return 2;
    }
    rc = expect_search_id(db, "base_device_update", 0, 1);
    if (rc == 0) rc = expect_search_id(db, "renamed_config_beta", insert_id, 0);
    if (rc != 0) {
        ezdb_close(db);
        return rc;
    }
    run_search_once(db, "device", 20, "crud_reopen_device");
    run_search_once(db, "config", 20, "crud_reopen_config");
    run_search_once(db, "zzznotfoundzzz", 20, "crud_reopen_notfound");
    run_search_once(db, "a", 20, "crud_reopen_a");
    print_memory_usage("crud_reopen");
    ezdb_close(db);
    return 0;
}

static char* wide_to_utf8(const wchar_t* text)
{
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) return NULL;
    char* out = (char*)malloc((size_t)needed);
    if (!out) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, out, needed, NULL, NULL) != needed) {
        free(out);
        return NULL;
    }
    return out;
}

static void free_utf8_argv(int argc, char** argv)
{
    if (!argv) return;
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
}

static int make_utf8_argv(int* out_argc, char*** out_argv)
{
    int argc = 0;
    wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv) return 0;
    char** argv = (char**)calloc((size_t)argc, sizeof(char*));
    if (!argv) {
        LocalFree(wide_argv);
        return 0;
    }
    for (int i = 0; i < argc; ++i) {
        argv[i] = wide_to_utf8(wide_argv[i]);
        if (!argv[i]) {
            free_utf8_argv(argc, argv);
            LocalFree(wide_argv);
            return 0;
        }
    }
    LocalFree(wide_argv);
    *out_argc = argc;
    *out_argv = argv;
    return 1;
}

static int read_console_utf8_line(char* out, size_t out_size)
{
    if (!out || !out_size) return 0;
    out[0] = '\0';

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        wchar_t wide[4096];
        DWORD read = 0;
        if (!ReadConsoleW(input, wide, (DWORD)(sizeof(wide) / sizeof(wide[0]) - 1u), &read, NULL)) return 0;
        wide[read] = L'\0';
        while (read > 0 && (wide[read - 1u] == L'\n' || wide[read - 1u] == L'\r')) wide[--read] = L'\0';
        int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_size, NULL, NULL);
        if (needed <= 0) return 0;
        out[out_size - 1u] = '\0';
        return 1;
    }

    if (!fgets(out, (int)out_size, stdin)) return 0;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1u] == '\n' || out[len - 1u] == '\r')) out[--len] = '\0';
    return 1;
}

static char* trim_ascii(char* text)
{
    while (*text == ' ' || *text == '\t') ++text;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1u] == ' ' || text[len - 1u] == '\t')) text[--len] = '\0';
    return text;
}

static void print_interactive_help(uint32_t default_limit)
{
    printf("Commands:\n");
    printf("  help\n");
    printf("  info\n");
    printf("  get <id>\n");
    printf("  search <keyword> [limit]\n");
    printf("  <keyword> [limit]\n");
    printf("  insert <path> [size] [mtime]\n");
    printf("  update <id> <path> [size] [mtime]\n");
    printf("  delete <id>\n");
    printf("  exit | quit\n");
    printf("Default search limit: %u\n", default_limit);
}

static char* next_token(char** cursor)
{
    char* p = *cursor;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) {
        *cursor = p;
        return NULL;
    }
    char* start = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (*p) *p++ = '\0';
    *cursor = p;
    return start;
}

static int parse_interactive_query(char* line, char** out_keyword, uint32_t* out_limit, uint32_t default_limit)
{
    char* text = trim_ascii(line);
    *out_keyword = text;
    *out_limit = default_limit;
    if (!*text) return 0;
    if (strcmp(text, "exit") == 0 || strcmp(text, "quit") == 0) return -1;

    char* last_space = NULL;
    for (char* p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t') last_space = p;
    }
    if (last_space) {
        char* maybe_limit = trim_ascii(last_space + 1);
        if (*maybe_limit) {
            char* end = NULL;
            unsigned long value = strtoul(maybe_limit, &end, 10);
            if (end && *end == '\0') {
                *last_space = '\0';
                *out_keyword = trim_ascii(text);
                *out_limit = (uint32_t)value;
            }
        }
    }
    return **out_keyword ? 1 : 0;
}

static int print_db_info(Ezdb* db, const char* memory_prefix)
{
    EzdbStats stats;
    int rc = ezdb_stats(db, &stats);
    if (rc != 0) {
        fprintf(stderr, "stats failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("records: %u\n", stats.record_count);
    printf("active: %u\n", stats.active_count);
    printf("file_size: %llu bytes\n", (unsigned long long)stats.file_size);
    printf("records_size: %llu bytes\n", (unsigned long long)stats.records_size);
    printf("dirs_size: %llu bytes\n", (unsigned long long)stats.dirs_size);
    printf("names_size: %llu bytes\n", (unsigned long long)stats.names_size);
    printf("index_size: %llu bytes\n", (unsigned long long)stats.index_size);
    printf("postings_size: %llu bytes\n", (unsigned long long)stats.postings_size);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_get_once(Ezdb* db, uint32_t id, const char* memory_prefix)
{
    EzdbSearchResult result;
    double start = now_ms();
    int rc = ezdb_get_by_id(db, id, &result);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "get failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("[%u] %s, %llu, %llu\n",
           result.id,
           result.path,
           (unsigned long long)result.size,
           (unsigned long long)result.modified_time);
    printf("get_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    ezdb_free_result(&result);
    return 0;
}

static int run_insert_once(Ezdb* db, const char* path, uint64_t size, uint64_t mtime, const char* memory_prefix)
{
    EzdbFileRecord record;
    record.path = path;
    record.size = size;
    record.modified_time = mtime;
    uint32_t id = 0;
    double start = now_ms();
    int rc = ezdb_insert(db, &record, &id);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "insert failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("insert_id: %u\n", id);
    printf("insert_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_update_once(Ezdb* db, uint32_t id, const char* path, uint64_t size, uint64_t mtime, const char* memory_prefix)
{
    EzdbFileRecord record;
    record.path = path;
    record.size = size;
    record.modified_time = mtime;
    double start = now_ms();
    int rc = ezdb_update(db, id, &record);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "update failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("update_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_delete_once(Ezdb* db, uint32_t id, const char* memory_prefix)
{
    double start = now_ms();
    int rc = ezdb_delete(db, id);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "delete failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("delete_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_main(int argc, char** argv)
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
        print_memory_usage("build");
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
        print_memory_usage("info");
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
        print_memory_usage("get");
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

        printf("open_ms: %.2f\n", open_elapsed);
        rc = run_search_once(db, argv[3], limit, "search");
        if (rc != 0) {
            ezdb_close(db);
            return 2;
        }
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "open") == 0 || strcmp(argv[1], "interactive") == 0) {
        if (argc < 3 || argc > 4) {
            print_usage();
            return 1;
        }
        uint32_t default_limit = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 20u;
        Ezdb* db = NULL;
        double open_start = now_ms();
        int rc = ezdb_open(argv[2], &db);
        double open_elapsed = now_ms() - open_start;
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        printf("open_ms: %.2f\n", open_elapsed);
        print_memory_usage("open");
        print_interactive_help(default_limit);

        char line[4096];
        for (;;) {
            printf("ezdb> ");
            fflush(stdout);
            if (!read_console_utf8_line(line, sizeof(line))) break;
            char* text = trim_ascii(line);
            if (!*text || strcmp(text, "help") == 0 || strcmp(text, "?") == 0) {
                print_interactive_help(default_limit);
                continue;
            }
            if (strcmp(text, "exit") == 0 || strcmp(text, "quit") == 0) break;

            char* cursor = text;
            char* command = next_token(&cursor);
            if (!command) {
                print_interactive_help(default_limit);
                continue;
            }

            if (strcmp(command, "info") == 0) {
                rc = print_db_info(db, "open_info");
            } else if (strcmp(command, "get") == 0) {
                char* id_text = next_token(&cursor);
                if (!id_text) {
                    printf("usage: get <id>\n");
                    continue;
                }
                rc = run_get_once(db, (uint32_t)strtoul(id_text, NULL, 10), "open_get");
            } else if (strcmp(command, "search") == 0) {
                char* keyword = trim_ascii(cursor);
                uint32_t limit = default_limit;
                int parsed = parse_interactive_query(keyword, &keyword, &limit, default_limit);
                if (parsed <= 0) {
                    printf("usage: search <keyword> [limit]\n");
                    continue;
                }
                rc = run_search_once(db, keyword, limit, "open_search");
            } else if (strcmp(command, "insert") == 0) {
                char* path = next_token(&cursor);
                char* size_text = next_token(&cursor);
                char* mtime_text = next_token(&cursor);
                if (!path) {
                    printf("usage: insert <path> [size] [mtime]\n");
                    continue;
                }
                rc = run_insert_once(db,
                                     path,
                                     parse_u64_arg(size_text, 0),
                                     parse_u64_arg(mtime_text, 0),
                                     "open_insert");
            } else if (strcmp(command, "update") == 0) {
                char* id_text = next_token(&cursor);
                char* path = next_token(&cursor);
                char* size_text = next_token(&cursor);
                char* mtime_text = next_token(&cursor);
                if (!id_text || !path) {
                    printf("usage: update <id> <path> [size] [mtime]\n");
                    continue;
                }
                rc = run_update_once(db,
                                     (uint32_t)strtoul(id_text, NULL, 10),
                                     path,
                                     parse_u64_arg(size_text, 0),
                                     parse_u64_arg(mtime_text, 0),
                                     "open_update");
            } else if (strcmp(command, "delete") == 0) {
                char* id_text = next_token(&cursor);
                if (!id_text) {
                    printf("usage: delete <id>\n");
                    continue;
                }
                rc = run_delete_once(db, (uint32_t)strtoul(id_text, NULL, 10), "open_delete");
            } else {
                char* keyword = NULL;
                uint32_t limit = default_limit;
                int parsed = parse_interactive_query(text, &keyword, &limit, default_limit);
                if (parsed < 0) break;
                if (parsed == 0) {
                    print_interactive_help(default_limit);
                    continue;
                }
                rc = run_search_once(db, keyword, limit, "open_search");
            }
            if (rc != 0) printf("command failed, type help for usage.\n");
        }
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "insert") == 0) {
        if (argc < 4 || argc > 6) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbFileRecord record;
        record.path = argv[3];
        record.size = argc >= 5 ? parse_u64_arg(argv[4], 0) : 0;
        record.modified_time = argc >= 6 ? parse_u64_arg(argv[5], 0) : 0;
        uint32_t id = 0;
        double start = now_ms();
        rc = ezdb_insert(db, &record, &id);
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "insert failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("insert_id: %u\n", id);
        printf("insert_ms: %.2f\n", elapsed);
        print_memory_usage("insert");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        if (argc < 5 || argc > 7) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbFileRecord record;
        record.path = argv[4];
        record.size = argc >= 6 ? parse_u64_arg(argv[5], 0) : 0;
        record.modified_time = argc >= 7 ? parse_u64_arg(argv[6], 0) : 0;
        double start = now_ms();
        rc = ezdb_update(db, (uint32_t)strtoul(argv[3], NULL, 10), &record);
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "update failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("update_ms: %.2f\n", elapsed);
        print_memory_usage("update");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "delete") == 0) {
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
        double start = now_ms();
        rc = ezdb_delete(db, (uint32_t)strtoul(argv[3], NULL, 10));
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "delete failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("delete_ms: %.2f\n", elapsed);
        print_memory_usage("delete");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "crud") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        return run_crud_bench(argv[2], argv[3]);
    }

    print_usage();
    return 1;
}

int main(void)
{
    int argc = 0;
    char** argv = NULL;
    if (!make_utf8_argv(&argc, &argv)) {
        fprintf(stderr, "failed to parse UTF-8 command line\n");
        return 2;
    }
    int rc = run_main(argc, argv);
    free_utf8_argv(argc, argv);
    return rc;
}
