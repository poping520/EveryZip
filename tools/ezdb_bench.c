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
        printf("Enter keyword or keyword limit. Type exit or quit to leave.\n");

        char line[4096];
        for (;;) {
            printf("ezdb> ");
            fflush(stdout);
            if (!read_console_utf8_line(line, sizeof(line))) break;
            char* keyword = NULL;
            uint32_t limit = default_limit;
            int parsed = parse_interactive_query(line, &keyword, &limit, default_limit);
            if (parsed < 0) break;
            if (parsed == 0) continue;
            rc = run_search_once(db, keyword, limit, "open_search");
            if (rc != 0) {
                ezdb_close(db);
                return 2;
            }
        }
        ezdb_close(db);
        return 0;
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
