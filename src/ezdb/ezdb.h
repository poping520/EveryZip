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

typedef struct EzdbArchiveRecord {
    char drive_letter;
    uint64_t file_ref_number;
    int64_t usn;
    const char* file_path;
    uint64_t file_size;
    uint64_t modified_time;
} EzdbArchiveRecord;

typedef struct EzdbEntryRecord {
    uint32_t archive_id;
    const char* entry_path;
    const void* entry_raw_path;
    uint32_t entry_raw_path_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} EzdbEntryRecord;

typedef struct EzdbSearchResult {
    uint32_t id;
    char* path;
    uint64_t size;
    uint64_t modified_time;
} EzdbSearchResult;

typedef struct EzdbArchiveResult {
    uint32_t id;
    char drive_letter;
    uint64_t file_ref_number;
    int64_t usn;
    char* file_path;
    uint64_t file_size;
    uint64_t modified_time;
} EzdbArchiveResult;

typedef struct EzdbEntryResult {
    uint32_t id;
    uint32_t archive_id;
    char* archive_path;
    char* entry_path;
    void* entry_raw_path;
    uint32_t entry_raw_path_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} EzdbEntryResult;

typedef enum EzdbSearchKind {
    EZDB_RESULT_ARCHIVE = 1,
    EZDB_RESULT_ENTRY = 2
} EzdbSearchKind;

typedef struct EzdbSearchV2Result {
    EzdbSearchKind kind;
    uint32_t id;
    uint32_t archive_id;
    char drive_letter;
    uint64_t file_ref_number;
    int64_t usn;
    char* archive_path;
    char* entry_path;
    void* entry_raw_path;
    uint32_t entry_raw_path_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t file_size;
    uint64_t modified_time;
} EzdbSearchV2Result;

#define EZDB_SEARCH_ARCHIVE_PATH  0x01u
#define EZDB_SEARCH_ENTRY_PATH    0x02u
#define EZDB_SEARCH_COMBINED_PATH 0x04u
#define EZDB_SEARCH_ALL (EZDB_SEARCH_ARCHIVE_PATH | EZDB_SEARCH_ENTRY_PATH)

typedef struct EzdbStats {
    uint32_t record_count;
    uint32_t active_count;
    uint32_t entry_count;
    uint32_t active_entry_count;
    uint64_t file_size;
    uint64_t records_size;
    uint64_t dirs_size;
    uint64_t names_size;
    uint64_t archive_meta_size;
    uint64_t entry_records_size;
    uint64_t raw_blob_size;
    uint64_t index_size;
    uint64_t postings_size;
} EzdbStats;

typedef void (*EzdbSearchCallback)(const EzdbSearchResult* result, void* user_data);
typedef void (*EzdbSearchV2Callback)(const EzdbSearchV2Result* result, void* user_data);

int ezdb_build_from_text(const char* input_txt, const char* output_ezdb);
int ezdb_build_from_archives_tsv(const char* input_tsv, const char* output_ezdb);
int ezdb_build_snapshot(const EzdbArchiveRecord* archives,
                        uint32_t archive_count,
                        const EzdbEntryRecord* entries,
                        uint32_t entry_count,
                        const char* output_ezdb);
int ezdb_open(const char* path, Ezdb** out_db);
void ezdb_close(Ezdb* db);

uint32_t ezdb_count(Ezdb* db);
uint32_t ezdb_active_count(Ezdb* db);
uint32_t ezdb_archive_count(Ezdb* db);
uint32_t ezdb_active_archive_count(Ezdb* db);
uint32_t ezdb_entry_count(Ezdb* db);
uint32_t ezdb_active_entry_count(Ezdb* db);
uint64_t ezdb_file_size(Ezdb* db);
int ezdb_stats(Ezdb* db, EzdbStats* out_stats);

int ezdb_get_by_id(Ezdb* db, uint32_t id, EzdbSearchResult* out_result);
void ezdb_free_result(EzdbSearchResult* result);
int ezdb_get_archive(Ezdb* db, uint32_t id, EzdbArchiveResult* out_result);
int ezdb_get_entry(Ezdb* db, uint32_t id, EzdbEntryResult* out_result);
void ezdb_free_archive_result(EzdbArchiveResult* result);
void ezdb_free_entry_result(EzdbEntryResult* result);
void ezdb_free_search_v2_result(EzdbSearchV2Result* result);

int ezdb_search_path(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data);
int ezdb_search(Ezdb* db, const char* keyword, uint32_t scope, uint32_t limit, EzdbSearchV2Callback callback, void* user_data);

int ezdb_begin_write(Ezdb* db, uint32_t flags);
int ezdb_commit_write(Ezdb* db);
int ezdb_rollback_write(Ezdb* db);
int ezdb_insert_many(Ezdb* db, const EzdbFileRecord* records, uint32_t count, uint32_t* first_id);

int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id);
int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record);
int ezdb_delete(Ezdb* db, uint32_t id);
int ezdb_upsert_archive(Ezdb* db, const EzdbArchiveRecord* record, uint32_t* out_id);
int ezdb_delete_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number);

const char* ezdb_error_message(int code);

#ifdef __cplusplus
}
#endif
