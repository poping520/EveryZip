#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ezdb Ezdb;

typedef struct EzdbFileRecord {
    const char* path;
    uint64_t size;
    uint64_t modified_time;
} EzdbFileRecord;

typedef struct EzdbSearchResult {
    uint32_t id;
    char* path;
    uint64_t size;
    uint64_t modified_time;
} EzdbSearchResult;

typedef struct EzdbStats {
    uint32_t record_count;
    uint32_t active_count;
    uint64_t file_size;
    uint64_t records_size;
    uint64_t dirs_size;
    uint64_t names_size;
    uint64_t index_size;
    uint64_t postings_size;
} EzdbStats;

typedef void (*EzdbSearchCallback)(const EzdbSearchResult* result, void* user_data);

int ezdb_build_from_text(const char* input_txt, const char* output_ezdb);
int ezdb_open(const char* path, Ezdb** out_db);
void ezdb_close(Ezdb* db);

uint32_t ezdb_count(Ezdb* db);
uint32_t ezdb_active_count(Ezdb* db);
uint64_t ezdb_file_size(Ezdb* db);
int ezdb_stats(Ezdb* db, EzdbStats* out_stats);

int ezdb_get_by_id(Ezdb* db, uint32_t id, EzdbSearchResult* out_result);
void ezdb_free_result(EzdbSearchResult* result);

int ezdb_search_path(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data);

int ezdb_begin_write(Ezdb* db, uint32_t flags);
int ezdb_commit_write(Ezdb* db);
int ezdb_rollback_write(Ezdb* db);
int ezdb_insert_many(Ezdb* db, const EzdbFileRecord* records, uint32_t count, uint32_t* first_id);

int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id);
int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record);
int ezdb_delete(Ezdb* db, uint32_t id);

const char* ezdb_error_message(int code);

#ifdef __cplusplus
}
#endif
