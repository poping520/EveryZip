/*
 * ezdb v4 path-tree index
 * =======================
 *
 * v4 intentionally breaks compatibility with v1/v2/v3. Instead of indexing every
 * byte gram in every full path, it stores a directory tree and builds separate
 * gram indexes for file names and directory components. Directory hits expand
 * to DFS file-id ranges, so a common directory term is stored once per matching
 * directory node rather than repeated for every child file.
 *
 * The public C API stays stable. The on-disk layout is:
 * header, file records, directory records, string pool, file index, directory
 * index, postings. Postings use adaptive array/range/bitset containers.
 */

#include "ezdb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EZDB_MAGIC "EZDB0004"
#define EZDB_VERSION 4u
#define EZDB_FLAG_DELETED 1u
#define EZDB_GRAM1 1u
#define EZDB_GRAM2 2u
#define EZDB_GRAM3 3u
#define EZDB_TOKEN_INLINE 0u
#define EZDB_TOKEN_HASHED 0x80000000u
#define EZDB_MAX_GRAM_TOKENS 3u
#define EZDB_STACK_TOKENS 512u
#define EZDB_STACK_KEYS (EZDB_STACK_TOKENS * EZDB_MAX_GRAM_TOKENS)

#define EZDB_POSTING_ARRAY 1u
#define EZDB_POSTING_RANGE 2u
#define EZDB_POSTING_BITSET 3u
#define EZDB_BITSET_DENSITY_DIVISOR 16u

enum {
    EZDB_OK = 0,
    EZDB_ERR_ARG = -1,
    EZDB_ERR_IO = -2,
    EZDB_ERR_FORMAT = -3,
    EZDB_ERR_MEMORY = -4,
    EZDB_ERR_NOT_FOUND = -5,
    EZDB_ERR_READ_ONLY = -6
};

typedef struct EzdbHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t file_count;
    uint64_t active_count;
    uint64_t dir_count;
    uint64_t file_records_offset;
    uint64_t file_records_size;
    uint64_t dir_records_offset;
    uint64_t dir_records_size;
    uint64_t strings_offset;
    uint64_t strings_size;
    uint64_t file_index_offset;
    uint64_t file_index_count;
    uint64_t dir_index_offset;
    uint64_t dir_index_count;
    uint64_t postings_offset;
    uint64_t postings_size;
    uint64_t reserved_offset;
    uint64_t reserved_size;
} EzdbHeader;

typedef struct EzdbDiskFile {
    uint32_t parent_dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t flags;
    uint64_t size;
    uint64_t modified_time;
} EzdbDiskFile;

typedef struct EzdbDiskDir {
    uint32_t parent_dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t first_file_id;
    uint32_t file_count;
} EzdbDiskDir;

typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
    uint64_t offset;
} EzdbDiskIndex;

typedef struct BuildDir {
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t parent;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t first_file;
    uint32_t old_first_file;
    uint32_t old_file_count;
    uint32_t first_file_id;
    uint32_t file_count;
} BuildDir;

typedef struct BuildFile {
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t old_dir;
    uint32_t parent_dir;
    uint32_t next_in_dir;
    uint64_t size;
    uint64_t modified_time;
} BuildFile;

typedef struct DirHashEntry {
    uint32_t parent;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t dir_id;
    uint32_t next;
} DirHashEntry;

typedef struct StringHashEntry {
    uint32_t offset;
    uint32_t len;
    uint32_t next;
} StringHashEntry;

typedef struct GramPair {
    uint32_t key;
    uint32_t id;
} GramPair;

typedef struct PostingBuildEntry {
    uint32_t key;
    uint32_t* ids;
    uint32_t count;
    uint32_t cap;
    uint32_t next;
} PostingBuildEntry;

typedef struct PostingBuilder {
    PostingBuildEntry* entries;
    uint32_t entry_count;
    uint32_t entry_cap;
    uint32_t* buckets;
    uint32_t bucket_count;
} PostingBuilder;

typedef struct QueryIndex {
    const EzdbDiskIndex* idx;
} QueryIndex;

struct Ezdb {
    FILE* fp;
    char* path;
    EzdbHeader header;
    EzdbDiskFile* files;
    EzdbDiskDir* dirs;
    char* strings;
    EzdbDiskIndex* file_index;
    EzdbDiskIndex* dir_index;
};

static uint64_t file_size_of(FILE* fp)
{
    long old_pos = ftell(fp);
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long size = ftell(fp);
    fseek(fp, old_pos, SEEK_SET);
    return size < 0 ? 0 : (uint64_t)size;
}

static double ezdb_now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

static char* ezdb_strdup_range(const char* text, size_t len)
{
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static int ensure_capacity(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (*capacity >= needed) return EZDB_OK;
    uint32_t next = *capacity ? *capacity : 1024;
    while (next < needed) {
        if (next > UINT32_MAX / 2u) return EZDB_ERR_MEMORY;
        next *= 2u;
    }
    void* new_data = realloc(*data, elem_size * (size_t)next);
    if (!new_data) return EZDB_ERR_MEMORY;
    *data = new_data;
    *capacity = next;
    return EZDB_OK;
}

static int ensure_capacity_small(void** data, size_t elem_size, uint32_t* capacity, uint32_t needed)
{
    if (*capacity >= needed) return EZDB_OK;
    uint32_t next = *capacity ? *capacity : 16u;
    while (next < needed) {
        if (next > UINT32_MAX / 2u) return EZDB_ERR_MEMORY;
        next *= 2u;
    }
    void* new_data = realloc(*data, elem_size * (size_t)next);
    if (!new_data) return EZDB_ERR_MEMORY;
    *data = new_data;
    *capacity = next;
    return EZDB_OK;
}

static unsigned char fold_ascii_byte(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static uint32_t fnv1a_bytes(const char* text, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 16777619u;
    }
    return hash;
}

static int utf8_token_len(const unsigned char* s, size_t remain)
{
    if (!remain) return 0;
    unsigned char ch = s[0];
    if (ch < 0x80u) return 1;
    if ((ch & 0xe0u) == 0xc0u && remain >= 2 && (s[1] & 0xc0u) == 0x80u) return 2;
    if ((ch & 0xf0u) == 0xe0u && remain >= 3 && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u) return 3;
    if ((ch & 0xf8u) == 0xf0u && remain >= 4 && (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u && (s[3] & 0xc0u) == 0x80u) return 4;
    return 1;
}

static int split_tokens(const char* text, uint32_t len, uint32_t** out_offsets, uint32_t** out_lens, uint32_t* out_count)
{
    uint32_t cap = 0;
    uint32_t count = 0;
    uint32_t* offsets = NULL;
    uint32_t* lens = NULL;
    uint32_t i = 0;
    while (i < len) {
        int token_len = utf8_token_len((const unsigned char*)text + i, (size_t)(len - i));
        if (count + 1 > cap) {
            uint32_t next = cap ? cap * 2u : 32u;
            uint32_t* new_offsets;
            uint32_t* new_lens;
            while (next < count + 1) {
                if (next > UINT32_MAX / 2u) {
                    free(offsets);
                    free(lens);
                    return EZDB_ERR_MEMORY;
                }
                next *= 2u;
            }
            new_offsets = (uint32_t*)realloc(offsets, sizeof(uint32_t) * (size_t)next);
            if (!new_offsets) {
                free(offsets);
                free(lens);
                return EZDB_ERR_MEMORY;
            }
            offsets = new_offsets;
            new_lens = (uint32_t*)realloc(lens, sizeof(uint32_t) * (size_t)next);
            if (!new_lens) {
                free(offsets);
                free(lens);
                return EZDB_ERR_MEMORY;
            }
            lens = new_lens;
            cap = next;
        }
        offsets[count] = i;
        lens[count] = (uint32_t)token_len;
        ++count;
        i += (uint32_t)token_len;
    }
    *out_offsets = offsets;
    *out_lens = lens;
    *out_count = count;
    return EZDB_OK;
}

static uint32_t make_gram_key_from_span(const char* text, uint32_t offset, uint32_t len, uint32_t token_count)
{
    const unsigned char* s = (const unsigned char*)text + offset;
    if (len <= 3u) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < len; ++i) value = (value << 8) | (uint32_t)fold_ascii_byte(s[i]);
        return (EZDB_TOKEN_INLINE << 31) | (token_count << 24) | value;
    }
    uint32_t hash = fnv1a_bytes(text + offset, len) & 0x00ffffffu;
    return EZDB_TOKEN_HASHED | (token_count << 24) | hash;
}

static uint32_t fnv1a_string(const char* text)
{
    return fnv1a_bytes(text, strlen(text));
}

static int parse_line(char* line, char** out_path, uint64_t* out_size, uint64_t* out_mtime)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
    char* last = strrchr(line, ',');
    if (!last) return 0;
    char* prev = last;
    while (prev > line) {
        --prev;
        if (*prev == ',') break;
    }
    if (*prev != ',') return 0;
    *prev = '\0';
    *last = '\0';
    char* size_text = prev + 1;
    char* time_text = last + 1;
    while (*size_text == ' ') ++size_text;
    while (*time_text == ' ') ++time_text;
    *out_path = line;
    *out_size = (uint64_t)_strtoui64(size_text, NULL, 10);
    *out_mtime = (uint64_t)_strtoui64(time_text, NULL, 10);
    return **out_path != '\0';
}

static int append_string(char** data, uint32_t* size, uint32_t* cap, const char* text, uint32_t len, uint32_t* out_offset)
{
    if (ensure_capacity((void**)data, 1, cap, *size + len + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
    *out_offset = *size;
    memcpy(*data + *size, text, len);
    (*data)[*size + len] = '\0';
    *size += len + 1u;
    return EZDB_OK;
}

static int init_u32_buckets(uint32_t** buckets, uint32_t count)
{
    *buckets = (uint32_t*)malloc(sizeof(uint32_t) * count);
    if (!*buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < count; ++i) (*buckets)[i] = UINT32_MAX;
    return EZDB_OK;
}

static int find_or_add_string(const char* text,
                              uint32_t len,
                              char** pool,
                              uint32_t* pool_size,
                              uint32_t* pool_cap,
                              StringHashEntry** entries,
                              uint32_t* entry_count,
                              uint32_t* entry_cap,
                              uint32_t** buckets,
                              uint32_t* bucket_count,
                              uint32_t* out_offset)
{
    if (!*buckets) {
        *bucket_count = 65536u;
        if (init_u32_buckets(buckets, *bucket_count) != EZDB_OK) return EZDB_ERR_MEMORY;
    }
    uint32_t hash = fnv1a_bytes(text, len);
    uint32_t bucket = hash & (*bucket_count - 1u);
    for (uint32_t i = (*buckets)[bucket]; i != UINT32_MAX; i = (*entries)[i].next) {
        if ((*entries)[i].len == len && memcmp(*pool + (*entries)[i].offset, text, len) == 0) {
            *out_offset = (*entries)[i].offset;
            return EZDB_OK;
        }
    }

    uint32_t offset = 0;
    if (append_string(pool, pool_size, pool_cap, text, len, &offset) != EZDB_OK) return EZDB_ERR_MEMORY;
    if (ensure_capacity((void**)entries, sizeof(StringHashEntry), entry_cap, *entry_count + 1) != EZDB_OK) return EZDB_ERR_MEMORY;
    StringHashEntry* e = &(*entries)[*entry_count];
    e->offset = offset;
    e->len = len;
    e->next = (*buckets)[bucket];
    (*buckets)[bucket] = *entry_count;
    *entry_count += 1;
    *out_offset = offset;
    return EZDB_OK;
}

static int dir_component_equals(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

static uint32_t find_or_add_dir(BuildDir** dirs,
                                uint32_t* dir_count,
                                uint32_t* dir_cap,
                                DirHashEntry** hash_entries,
                                uint32_t* hash_count,
                                uint32_t* hash_cap,
                                uint32_t** buckets,
                                uint32_t* bucket_count,
                                char** string_pool,
                                uint32_t* string_size,
                                uint32_t* string_cap,
                                StringHashEntry** string_entries,
                                uint32_t* string_entry_count,
                                uint32_t* string_entry_cap,
                                uint32_t** string_buckets,
                                uint32_t* string_bucket_count,
                                uint32_t parent,
                                const char* name,
                                uint32_t name_len)
{
    if (!*buckets) {
        *bucket_count = 262144u;
        if (init_u32_buckets(buckets, *bucket_count) != EZDB_OK) return UINT32_MAX;
    }
    uint32_t hash = fnv1a_bytes(name, name_len) ^ (parent * 16777619u);
    uint32_t bucket = hash & (*bucket_count - 1u);
    for (uint32_t i = (*buckets)[bucket]; i != UINT32_MAX; i = (*hash_entries)[i].next) {
        DirHashEntry* he = &(*hash_entries)[i];
        if (he->parent == parent && he->name_len == name_len &&
            memcmp(*string_pool + he->name_offset, name, name_len) == 0) {
            return he->dir_id;
        }
    }

    if (ensure_capacity((void**)dirs, sizeof(BuildDir), dir_cap, *dir_count + 1) != EZDB_OK) return UINT32_MAX;
    uint32_t name_offset = 0;
    if (find_or_add_string(name, name_len, string_pool, string_size, string_cap,
                           string_entries, string_entry_count, string_entry_cap,
                           string_buckets, string_bucket_count, &name_offset) != EZDB_OK) {
        return UINT32_MAX;
    }

    uint32_t id = *dir_count;
    BuildDir* dir = &(*dirs)[id];
    memset(dir, 0, sizeof(*dir));
    dir->name_offset = name_offset;
    dir->name_len = name_len;
    dir->parent = parent;
    dir->first_child = UINT32_MAX;
    dir->next_sibling = UINT32_MAX;
    dir->first_file = UINT32_MAX;
    dir->old_first_file = UINT32_MAX;
    if (id != parent && parent != UINT32_MAX) {
        dir->next_sibling = (*dirs)[parent].first_child;
        (*dirs)[parent].first_child = id;
    }
    *dir_count += 1;

    if (ensure_capacity((void**)hash_entries, sizeof(DirHashEntry), hash_cap, *hash_count + 1) != EZDB_OK) return UINT32_MAX;
    DirHashEntry* he = &(*hash_entries)[*hash_count];
    he->parent = parent;
    he->name_offset = name_offset;
    he->name_len = name_len;
    he->dir_id = id;
    he->next = (*buckets)[bucket];
    (*buckets)[bucket] = *hash_count;
    *hash_count += 1;
    return id;
}

static uint32_t get_or_create_path_dir(BuildDir** dirs,
                                       uint32_t* dir_count,
                                       uint32_t* dir_cap,
                                       DirHashEntry** hash_entries,
                                       uint32_t* hash_count,
                                       uint32_t* hash_cap,
                                       uint32_t** buckets,
                                       uint32_t* bucket_count,
                                       char** string_pool,
                                       uint32_t* string_size,
                                       uint32_t* string_cap,
                                       StringHashEntry** string_entries,
                                       uint32_t* string_entry_count,
                                       uint32_t* string_entry_cap,
                                       uint32_t** string_buckets,
                                       uint32_t* string_bucket_count,
                                       const char* path,
                                       uint32_t path_len)
{
    uint32_t parent = 0;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= path_len; ++i) {
        if (i == path_len || path[i] == '\\' || path[i] == '/') {
            if (i > start) {
                parent = find_or_add_dir(dirs, dir_count, dir_cap, hash_entries, hash_count, hash_cap,
                                         buckets, bucket_count, string_pool, string_size, string_cap,
                                         string_entries, string_entry_count, string_entry_cap,
                                         string_buckets, string_bucket_count, parent, path + start, i - start);
                if (parent == UINT32_MAX) return UINT32_MAX;
            }
            start = i + 1;
        }
    }
    return parent;
}

static int append_file(BuildFile** files,
                       uint32_t* file_count,
                       uint32_t* file_cap,
                       BuildDir* dirs,
                       uint32_t dir_id,
                       char** string_pool,
                       uint32_t* string_size,
                       uint32_t* string_cap,
                       StringHashEntry** string_entries,
                       uint32_t* string_entry_count,
                       uint32_t* string_entry_cap,
                       uint32_t** string_buckets,
                       uint32_t* string_bucket_count,
                       const char* name,
                       uint32_t name_len,
                       uint64_t size,
                       uint64_t modified_time)
{
    if (ensure_capacity((void**)files, sizeof(BuildFile), file_cap, *file_count + 1) != EZDB_OK) return EZDB_ERR_MEMORY;
    uint32_t name_offset = 0;
    if (find_or_add_string(name, name_len, string_pool, string_size, string_cap,
                           string_entries, string_entry_count, string_entry_cap,
                           string_buckets, string_bucket_count, &name_offset) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    uint32_t id = *file_count;
    BuildFile* f = &(*files)[id];
    memset(f, 0, sizeof(*f));
    f->name_offset = name_offset;
    f->name_len = name_len;
    f->old_dir = dir_id;
    f->parent_dir = dir_id;
    f->size = size;
    f->modified_time = modified_time;
    f->next_in_dir = dirs[dir_id].old_first_file;
    dirs[dir_id].old_first_file = id;
    dirs[dir_id].old_file_count += 1;
    *file_count += 1;
    return EZDB_OK;
}

static int u32_compare(const void* a, const void* b)
{
    uint32_t av = *(const uint32_t*)a;
    uint32_t bv = *(const uint32_t*)b;
    if (av == bv) return 0;
    return av < bv ? -1 : 1;
}

static int gram_pair_compare(const void* a, const void* b)
{
    const GramPair* pa = (const GramPair*)a;
    const GramPair* pb = (const GramPair*)b;
    if (pa->key != pb->key) return pa->key < pb->key ? -1 : 1;
    if (pa->id != pb->id) return pa->id < pb->id ? -1 : 1;
    return 0;
}

static int posting_entry_key_compare(const void* a, const void* b)
{
    const PostingBuildEntry* pa = *(const PostingBuildEntry* const*)a;
    const PostingBuildEntry* pb = *(const PostingBuildEntry* const*)b;
    if (pa->key == pb->key) return 0;
    return pa->key < pb->key ? -1 : 1;
}

static int index_compare(const void* a, const void* b)
{
    const EzdbDiskIndex* ia = (const EzdbDiskIndex*)a;
    const EzdbDiskIndex* ib = (const EzdbDiskIndex*)b;
    if (ia->key == ib->key) return 0;
    return ia->key < ib->key ? -1 : 1;
}

static int query_index_compare(const void* a, const void* b)
{
    const QueryIndex* qa = (const QueryIndex*)a;
    const QueryIndex* qb = (const QueryIndex*)b;
    if (qa->idx->count == qb->idx->count) return 0;
    return qa->idx->count < qb->idx->count ? -1 : 1;
}

static void posting_builder_free(PostingBuilder* builder)
{
    if (!builder) return;
    for (uint32_t i = 0; i < builder->entry_count; ++i) free(builder->entries[i].ids);
    free(builder->entries);
    free(builder->buckets);
    memset(builder, 0, sizeof(*builder));
}

static int posting_builder_init(PostingBuilder* builder, uint32_t bucket_count)
{
    memset(builder, 0, sizeof(*builder));
    builder->bucket_count = bucket_count;
    builder->buckets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)bucket_count);
    if (!builder->buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < bucket_count; ++i) builder->buckets[i] = UINT32_MAX;
    return EZDB_OK;
}

static int posting_builder_add(PostingBuilder* builder, uint32_t key, uint32_t id)
{
    uint32_t bucket = key & (builder->bucket_count - 1u);
    for (uint32_t i = builder->buckets[bucket]; i != UINT32_MAX; i = builder->entries[i].next) {
        PostingBuildEntry* entry = &builder->entries[i];
        if (entry->key == key) {
            if (entry->count && entry->ids[entry->count - 1u] == id) return EZDB_OK;
            if (ensure_capacity_small((void**)&entry->ids, sizeof(uint32_t), &entry->cap, entry->count + 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
            entry->ids[entry->count++] = id;
            return EZDB_OK;
        }
    }

    if (ensure_capacity((void**)&builder->entries, sizeof(PostingBuildEntry), &builder->entry_cap, builder->entry_count + 1u) != EZDB_OK) {
        return EZDB_ERR_MEMORY;
    }
    PostingBuildEntry* entry = &builder->entries[builder->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->key = key;
    entry->next = builder->buckets[bucket];
    builder->buckets[bucket] = builder->entry_count;
    builder->entry_count += 1u;
    if (ensure_capacity_small((void**)&entry->ids, sizeof(uint32_t), &entry->cap, 1u) != EZDB_OK) return EZDB_ERR_MEMORY;
    entry->ids[entry->count++] = id;
    return EZDB_OK;
}

static int add_text_grams(PostingBuilder* builder, const char* text, uint32_t id)
{
    uint32_t len = (uint32_t)strlen(text);
    if (!len) return EZDB_OK;
    uint32_t stack_offsets[EZDB_STACK_TOKENS];
    uint32_t stack_lens[EZDB_STACK_TOKENS];
    uint32_t stack_keys[EZDB_STACK_KEYS];
    uint32_t* offsets = stack_offsets;
    uint32_t* lens = stack_lens;
    uint32_t* keys = stack_keys;
    uint32_t token_cap = EZDB_STACK_TOKENS;
    uint32_t key_cap = EZDB_STACK_KEYS;
    uint32_t token_count = 0;
    uint32_t key_count = 0;
    uint32_t pos = 0;
    while (pos < len) {
        int token_len = utf8_token_len((const unsigned char*)text + pos, (size_t)(len - pos));
        if (token_count >= token_cap) {
            uint32_t next_cap = token_cap * 2u;
            uint32_t* new_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
            uint32_t* new_lens = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)next_cap);
            if (!new_offsets || !new_lens) {
                free(new_offsets);
                free(new_lens);
                if (offsets != stack_offsets) free(offsets);
                if (lens != stack_lens) free(lens);
                return EZDB_ERR_MEMORY;
            }
            memcpy(new_offsets, offsets, sizeof(uint32_t) * (size_t)token_count);
            memcpy(new_lens, lens, sizeof(uint32_t) * (size_t)token_count);
            if (offsets != stack_offsets) free(offsets);
            if (lens != stack_lens) free(lens);
            offsets = new_offsets;
            lens = new_lens;
            token_cap = next_cap;
        }
        offsets[token_count] = pos;
        lens[token_count] = (uint32_t)token_len;
        ++token_count;
        pos += (uint32_t)token_len;
    }
    if (token_count * EZDB_MAX_GRAM_TOKENS > key_cap) {
        key_cap = token_count * EZDB_MAX_GRAM_TOKENS;
        keys = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)key_cap);
        if (!keys) {
            if (offsets != stack_offsets) free(offsets);
            if (lens != stack_lens) free(lens);
            return EZDB_ERR_MEMORY;
        }
    }
    for (uint32_t kind = EZDB_GRAM1; kind <= EZDB_GRAM3; ++kind) {
        if (token_count < kind) continue;
        for (uint32_t i = 0; i + kind <= token_count; ++i) {
            uint32_t byte_start = offsets[i];
            uint32_t byte_end = offsets[i + kind - 1u] + lens[i + kind - 1u];
            keys[key_count++] = make_gram_key_from_span(text, byte_start, byte_end - byte_start, kind);
        }
    }
    if (offsets != stack_offsets) free(offsets);
    if (lens != stack_lens) free(lens);
    qsort(keys, key_count, sizeof(uint32_t), u32_compare);
    uint32_t last = UINT32_MAX;
    for (uint32_t i = 0; i < key_count; ++i) {
        if (keys[i] == last) continue;
        last = keys[i];
        if (posting_builder_add(builder, keys[i], id) != EZDB_OK) {
            if (keys != stack_keys) free(keys);
            return EZDB_ERR_MEMORY;
        }
    }
    if (keys != stack_keys) free(keys);
    return EZDB_OK;
}

static int write_varuint(FILE* fp, uint32_t value, uint64_t* written)
{
    unsigned char bytes[5];
    int count = 0;
    do {
        bytes[count] = (unsigned char)(value & 0x7fu);
        value >>= 7u;
        if (value) bytes[count] |= 0x80u;
        ++count;
    } while (value);
    if (fwrite(bytes, 1, (size_t)count, fp) != (size_t)count) return EZDB_ERR_IO;
    *written += (uint64_t)count;
    return EZDB_OK;
}

static int read_varuint(FILE* fp, uint32_t* out)
{
    uint32_t value = 0;
    int shift = 0;
    for (int i = 0; i < 5; ++i) {
        int ch = fgetc(fp);
        if (ch == EOF) return EZDB_ERR_IO;
        value |= (uint32_t)(ch & 0x7f) << shift;
        if (!(ch & 0x80)) {
            *out = value;
            return EZDB_OK;
        }
        shift += 7;
    }
    return EZDB_ERR_FORMAT;
}

static int write_varuint64(FILE* fp, uint64_t value, uint64_t* written)
{
    unsigned char bytes[10];
    int count = 0;
    do {
        bytes[count] = (unsigned char)(value & 0x7fu);
        value >>= 7u;
        if (value) bytes[count] |= 0x80u;
        ++count;
    } while (value);
    if (fwrite(bytes, 1, (size_t)count, fp) != (size_t)count) return EZDB_ERR_IO;
    *written += (uint64_t)count;
    return EZDB_OK;
}

static int read_varuint64(FILE* fp, uint64_t* out)
{
    uint64_t value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        int ch = fgetc(fp);
        if (ch == EOF) return EZDB_ERR_IO;
        value |= (uint64_t)(ch & 0x7f) << shift;
        if (!(ch & 0x80)) {
            *out = value;
            return EZDB_OK;
        }
        shift += 7;
    }
    return EZDB_ERR_FORMAT;
}

static int read_mem_varuint(const unsigned char** p, const unsigned char* end, uint32_t* out)
{
    uint32_t value = 0;
    int shift = 0;
    for (int i = 0; i < 5; ++i) {
        if (*p >= end) return EZDB_ERR_IO;
        unsigned char ch = *(*p)++;
        value |= (uint32_t)(ch & 0x7fu) << shift;
        if (!(ch & 0x80u)) {
            *out = value;
            return EZDB_OK;
        }
        shift += 7;
    }
    return EZDB_ERR_FORMAT;
}

static int read_mem_varuint64(const unsigned char** p, const unsigned char* end, uint64_t* out)
{
    uint64_t value = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (*p >= end) return EZDB_ERR_IO;
        unsigned char ch = *(*p)++;
        value |= (uint64_t)(ch & 0x7fu) << shift;
        if (!(ch & 0x80u)) {
            *out = value;
            return EZDB_OK;
        }
        shift += 7;
    }
    return EZDB_ERR_FORMAT;
}

static int write_file_records_compact(FILE* out, const BuildFile* files, uint32_t file_count, uint64_t* out_written)
{
    uint64_t written = 0;
    for (uint32_t i = 0; i < file_count; ++i) {
        int rc = write_varuint(out, files[i].parent_dir, &written);
        if (rc == EZDB_OK) rc = write_varuint(out, files[i].name_offset, &written);
        if (rc == EZDB_OK) rc = write_varuint(out, files[i].name_len, &written);
        if (rc == EZDB_OK) rc = write_varuint64(out, files[i].size, &written);
        if (rc == EZDB_OK) rc = write_varuint64(out, files[i].modified_time, &written);
        if (rc != EZDB_OK) return rc;
    }
    *out_written = written;
    return EZDB_OK;
}

static int read_file_records_compact(FILE* fp, uint64_t encoded_size, EzdbDiskFile* files, uint32_t file_count)
{
    unsigned char* data = (unsigned char*)malloc((size_t)encoded_size ? (size_t)encoded_size : 1u);
    if (!data) return EZDB_ERR_MEMORY;
    if (encoded_size && fread(data, 1, (size_t)encoded_size, fp) != (size_t)encoded_size) {
        free(data);
        return EZDB_ERR_IO;
    }
    const unsigned char* p = data;
    const unsigned char* end = data + encoded_size;
    for (uint32_t i = 0; i < file_count; ++i) {
        uint32_t parent_dir = 0, name_offset = 0, name_len = 0;
        uint64_t size = 0, modified_time = 0;
        int rc = read_mem_varuint(&p, end, &parent_dir);
        if (rc == EZDB_OK) rc = read_mem_varuint(&p, end, &name_offset);
        if (rc == EZDB_OK) rc = read_mem_varuint(&p, end, &name_len);
        if (rc == EZDB_OK) rc = read_mem_varuint64(&p, end, &size);
        if (rc == EZDB_OK) rc = read_mem_varuint64(&p, end, &modified_time);
        if (rc != EZDB_OK) {
            free(data);
            return rc;
        }
        files[i].parent_dir_id = parent_dir;
        files[i].name_offset = name_offset;
        files[i].name_len = name_len;
        files[i].flags = 0;
        files[i].size = size;
        files[i].modified_time = modified_time;
    }
    free(data);
    return EZDB_OK;
}

static int write_bytes(FILE* fp, const void* data, uint32_t size, uint64_t* written)
{
    if (size && fwrite(data, 1, size, fp) != size) return EZDB_ERR_IO;
    *written += size;
    return EZDB_OK;
}

static uint32_t varuint_size(uint32_t value)
{
    uint32_t count = 0;
    do {
        ++count;
        value >>= 7u;
    } while (value);
    return count;
}

static uint32_t estimate_array_size(const uint32_t* ids, uint32_t count)
{
    uint32_t bytes = 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t delta = i == 0 ? ids[i] : ids[i] - prev;
        bytes += varuint_size(delta);
        prev = ids[i];
    }
    return bytes;
}

static uint32_t count_ranges(const uint32_t* ids, uint32_t count)
{
    if (!count) return 0;
    uint32_t ranges = 1;
    for (uint32_t i = 1; i < count; ++i) {
        if (ids[i] != ids[i - 1u] + 1u) ++ranges;
    }
    return ranges;
}

static uint32_t estimate_range_size(const uint32_t* ids, uint32_t count)
{
    if (!count) return 0;
    uint32_t bytes = 0;
    uint32_t i = 0;
    uint32_t prev_start = 0;
    uint32_t range_index = 0;
    while (i < count) {
        uint32_t start = ids[i];
        uint32_t end = start;
        while (i + 1u < count && ids[i + 1u] == end + 1u) {
            ++i;
            ++end;
        }
        uint32_t len = end - start + 1u;
        bytes += varuint_size(range_index == 0 ? start : start - prev_start);
        bytes += varuint_size(len);
        prev_start = start;
        ++range_index;
        ++i;
    }
    return bytes;
}

static int write_array_container(FILE* out, const uint32_t* ids, uint32_t count, uint64_t* written)
{
    uint32_t prev = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t delta = i == 0 ? ids[i] : ids[i] - prev;
        int rc = write_varuint(out, delta, written);
        if (rc != EZDB_OK) return rc;
        prev = ids[i];
    }
    return EZDB_OK;
}

static int write_range_container(FILE* out, const uint32_t* ids, uint32_t count, uint64_t* written)
{
    uint32_t i = 0;
    uint32_t prev_start = 0;
    uint32_t range_index = 0;
    while (i < count) {
        uint32_t start = ids[i];
        uint32_t end = start;
        while (i + 1u < count && ids[i + 1u] == end + 1u) {
            ++i;
            ++end;
        }
        int rc = write_varuint(out, range_index == 0 ? start : start - prev_start, written);
        if (rc == EZDB_OK) rc = write_varuint(out, end - start + 1u, written);
        if (rc != EZDB_OK) return rc;
        prev_start = start;
        ++range_index;
        ++i;
    }
    return EZDB_OK;
}

static int write_bitset_container(FILE* out, const uint32_t* ids, uint32_t count, uint32_t universe_count, uint64_t* written)
{
    uint32_t byte_count = (universe_count + 7u) / 8u;
    unsigned char* bits = (unsigned char*)calloc(byte_count ? byte_count : 1u, 1);
    if (!bits) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i] < universe_count) bits[ids[i] >> 3u] |= (unsigned char)(1u << (ids[i] & 7u));
    }
    int rc = write_bytes(out, bits, byte_count, written);
    free(bits);
    return rc;
}

static int write_postings(FILE* out, PostingBuilder* builder, uint32_t universe_count, EzdbDiskIndex** out_index, uint32_t* out_index_count, uint64_t* out_written)
{
    *out_index = NULL;
    *out_index_count = 0;
    *out_written = 0;
    if (!builder->entry_count) return EZDB_OK;

    PostingBuildEntry** sorted = (PostingBuildEntry**)malloc(sizeof(PostingBuildEntry*) * (size_t)builder->entry_count);
    if (!sorted) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < builder->entry_count; ++i) sorted[i] = &builder->entries[i];
    qsort(sorted, builder->entry_count, sizeof(PostingBuildEntry*), posting_entry_key_compare);

    EzdbDiskIndex* indexes = NULL;
    uint32_t index_count = 0;
    uint32_t index_cap = 0;
    uint64_t written = 0;
    for (uint32_t entry_i = 0; entry_i < builder->entry_count; ++entry_i) {
        PostingBuildEntry* entry = sorted[entry_i];
        if (!entry->count) continue;
        qsort(entry->ids, entry->count, sizeof(uint32_t), u32_compare);
        uint32_t unique_count = 0;
        for (uint32_t i = 0; i < entry->count; ++i) {
            if (i && entry->ids[i] == entry->ids[i - 1u]) continue;
            entry->ids[unique_count++] = entry->ids[i];
        }
        entry->count = unique_count;

        uint32_t array_size = estimate_array_size(entry->ids, entry->count);
        uint32_t range_count = count_ranges(entry->ids, entry->count);
        uint32_t range_size = estimate_range_size(entry->ids, entry->count);
        uint32_t bitset_size = (universe_count + 7u) / 8u;
        uint32_t type = EZDB_POSTING_ARRAY;
        uint32_t encoded_size = array_size;
        if (range_count <= entry->count / 2u && range_size < encoded_size) {
            type = EZDB_POSTING_RANGE;
            encoded_size = range_size;
        }
        if (entry->count >= universe_count / EZDB_BITSET_DENSITY_DIVISOR && bitset_size < encoded_size) {
            type = EZDB_POSTING_BITSET;
            encoded_size = bitset_size;
        }

        uint64_t local_offset = written;
        int rc;
        if (type == EZDB_POSTING_RANGE) {
            rc = write_range_container(out, entry->ids, entry->count, &written);
        } else if (type == EZDB_POSTING_BITSET) {
            rc = write_bitset_container(out, entry->ids, entry->count, universe_count, &written);
        } else {
            rc = write_array_container(out, entry->ids, entry->count, &written);
        }
        if (rc != EZDB_OK) {
            free(sorted);
            free(indexes);
            return rc;
        }

        if (ensure_capacity((void**)&indexes, sizeof(EzdbDiskIndex), &index_cap, index_count + 1) != EZDB_OK) {
            free(sorted);
            free(indexes);
            return EZDB_ERR_MEMORY;
        }
        indexes[index_count].key = entry->key;
        indexes[index_count].count = entry->count;
        indexes[index_count].container_type = type;
        indexes[index_count].encoded_size = (uint32_t)(written - local_offset);
        indexes[index_count].offset = local_offset;
        ++index_count;
    }
    free(sorted);
    *out_index = indexes;
    *out_index_count = index_count;
    *out_written = written;
    return EZDB_OK;
}

static int build_query_keys(const char* keyword, uint32_t** out_keys, uint32_t* out_count)
{
    uint32_t len = (uint32_t)strlen(keyword);
    if (!len) return EZDB_ERR_ARG;
    uint32_t* offsets = NULL;
    uint32_t* lens = NULL;
    uint32_t token_count = 0;
    int rc = split_tokens(keyword, len, &offsets, &lens, &token_count);
    if (rc != EZDB_OK) return rc;
    uint32_t kind = token_count >= EZDB_GRAM3 ? EZDB_GRAM3 : token_count;
    uint32_t count = token_count - kind + 1u;
    uint32_t* keys = (uint32_t*)malloc(sizeof(uint32_t) * count);
    if (!keys) {
        free(offsets);
        free(lens);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t byte_start = offsets[i];
        uint32_t byte_end = offsets[i + kind - 1u] + lens[i + kind - 1u];
        keys[i] = make_gram_key_from_span(keyword, byte_start, byte_end - byte_start, kind);
    }
    free(offsets);
    free(lens);
    qsort(keys, count, sizeof(uint32_t), u32_compare);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i && keys[i] == keys[i - 1]) continue;
        keys[n++] = keys[i];
    }
    *out_keys = keys;
    *out_count = n;
    return EZDB_OK;
}

static uint32_t dfs_assign(BuildDir* dirs, BuildFile* old_files, BuildFile* new_files, uint32_t dir_id, uint32_t next_file)
{
    BuildDir* dir = &dirs[dir_id];
    dir->first_file_id = next_file;
    for (uint32_t old = dir->old_first_file; old != UINT32_MAX; old = old_files[old].next_in_dir) {
        new_files[next_file] = old_files[old];
        new_files[next_file].parent_dir = dir_id;
        ++next_file;
    }
    for (uint32_t child = dir->first_child; child != UINT32_MAX; child = dirs[child].next_sibling) {
        next_file = dfs_assign(dirs, old_files, new_files, child, next_file);
    }
    dir->file_count = next_file - dir->first_file_id;
    return next_file;
}

static const char* file_name(Ezdb* db, const EzdbDiskFile* f)
{
    return db->strings + f->name_offset;
}

static const char* dir_name(Ezdb* db, const EzdbDiskDir* d)
{
    return db->strings + d->name_offset;
}

static int build_dir_path(Ezdb* db, uint32_t dir_id, char** out, uint32_t* out_len)
{
    uint32_t stack_cap = 32;
    uint32_t stack_count = 0;
    uint32_t* stack = (uint32_t*)malloc(sizeof(uint32_t) * stack_cap);
    if (!stack) return EZDB_ERR_MEMORY;
    uint32_t cur = dir_id;
    while (cur != 0 && cur < db->header.dir_count) {
        if (ensure_capacity((void**)&stack, sizeof(uint32_t), &stack_cap, stack_count + 1) != EZDB_OK) {
            free(stack);
            return EZDB_ERR_MEMORY;
        }
        stack[stack_count++] = cur;
        cur = db->dirs[cur].parent_dir_id;
    }
    uint32_t len = 0;
    for (uint32_t i = 0; i < stack_count; ++i) {
        const EzdbDiskDir* d = &db->dirs[stack[i]];
        len += d->name_len;
        if (i + 1 < stack_count) ++len;
    }
    char* path = (char*)malloc((size_t)len + 1u);
    if (!path) {
        free(stack);
        return EZDB_ERR_MEMORY;
    }
    uint32_t pos = 0;
    for (uint32_t ri = stack_count; ri > 0; --ri) {
        uint32_t id = stack[ri - 1];
        const EzdbDiskDir* d = &db->dirs[id];
        if (pos) path[pos++] = '\\';
        memcpy(path + pos, dir_name(db, d), d->name_len);
        pos += d->name_len;
    }
    path[pos] = '\0';
    free(stack);
    *out = path;
    if (out_len) *out_len = len;
    return EZDB_OK;
}

static int build_result_path(Ezdb* db, uint32_t id, EzdbSearchResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    if (id >= db->header.file_count) return EZDB_ERR_NOT_FOUND;
    memset(out_result, 0, sizeof(*out_result));
    EzdbDiskFile* f = &db->files[id];
    if (f->flags & EZDB_FLAG_DELETED) return EZDB_ERR_NOT_FOUND;
    char* dir_path = NULL;
    uint32_t dir_len = 0;
    int rc = build_dir_path(db, f->parent_dir_id, &dir_path, &dir_len);
    if (rc != EZDB_OK) return rc;
    uint32_t path_len = dir_len + (dir_len ? 1u : 0u) + f->name_len;
    char* path = (char*)malloc((size_t)path_len + 1u);
    if (!path) {
        free(dir_path);
        return EZDB_ERR_MEMORY;
    }
    if (dir_len) {
        memcpy(path, dir_path, dir_len);
        path[dir_len] = '\\';
        memcpy(path + dir_len + 1u, file_name(db, f), f->name_len + 1u);
    } else {
        memcpy(path, file_name(db, f), f->name_len + 1u);
    }
    free(dir_path);
    out_result->id = id;
    out_result->path = path;
    out_result->size = f->size;
    out_result->modified_time = f->modified_time;
    return EZDB_OK;
}

static int contains_ascii_casefold_bytes(const char* text, size_t text_len, const char* needle, size_t needle_len)
{
    if (!needle_len) return 1;
    if (needle_len > text_len) return 0;
    for (size_t i = 0; i + needle_len <= text_len; ++i) {
        size_t j = 0;
        while (j < needle_len &&
               fold_ascii_byte((unsigned char)text[i + j]) == fold_ascii_byte((unsigned char)needle[j])) ++j;
        if (j == needle_len) return 1;
    }
    return 0;
}

static int record_contains_keyword(Ezdb* db, uint32_t id, const char* keyword, size_t key_len)
{
    const EzdbDiskFile* f = &db->files[id];
    if (f->flags & EZDB_FLAG_DELETED) return 0;
    if (contains_ascii_casefold_bytes(file_name(db, f), f->name_len, keyword, key_len)) return 1;
    char* path = NULL;
    EzdbSearchResult result;
    if (build_result_path(db, id, &result) != EZDB_OK) return 0;
    path = result.path;
    int matched = contains_ascii_casefold_bytes(path, strlen(path), keyword, key_len);
    ezdb_free_result(&result);
    return matched;
}

static EzdbDiskIndex* find_index(EzdbDiskIndex* index, uint64_t count, uint32_t key)
{
    EzdbDiskIndex search_key;
    search_key.key = key;
    search_key.count = 0;
    search_key.offset = 0;
    return (EzdbDiskIndex*)bsearch(&search_key, index, (size_t)count, sizeof(EzdbDiskIndex), index_compare);
}

static int load_postings(Ezdb* db, const EzdbDiskIndex* idx, uint32_t** out_ids)
{
    uint32_t* ids = (uint32_t*)malloc(sizeof(uint32_t) * idx->count);
    if (!ids && idx->count) return EZDB_ERR_MEMORY;
    if (fseek(db->fp, (long)(db->header.postings_offset + idx->offset), SEEK_SET) != 0) {
        free(ids);
        return EZDB_ERR_IO;
    }
    if (idx->container_type == EZDB_POSTING_ARRAY) {
        uint32_t current = 0;
        for (uint32_t i = 0; i < idx->count; ++i) {
            uint32_t delta = 0;
            int rc = read_varuint(db->fp, &delta);
            if (rc != EZDB_OK) {
                free(ids);
                return rc;
            }
            current = i == 0 ? delta : current + delta;
            ids[i] = current;
        }
    } else if (idx->container_type == EZDB_POSTING_RANGE) {
        uint32_t n = 0;
        uint32_t current_start = 0;
        while (n < idx->count) {
            uint32_t start_delta = 0, len = 0;
            int rc = read_varuint(db->fp, &start_delta);
            if (rc == EZDB_OK) rc = read_varuint(db->fp, &len);
            if (rc != EZDB_OK) {
                free(ids);
                return rc;
            }
            current_start = n == 0 ? start_delta : current_start + start_delta;
            for (uint32_t j = 0; j < len && n < idx->count; ++j) ids[n++] = current_start + j;
        }
    } else if (idx->container_type == EZDB_POSTING_BITSET) {
        uint32_t byte_count = idx->encoded_size;
        unsigned char* bits = (unsigned char*)malloc(byte_count ? byte_count : 1u);
        if (!bits) {
            free(ids);
            return EZDB_ERR_MEMORY;
        }
        if (byte_count && fread(bits, 1, byte_count, db->fp) != byte_count) {
            free(bits);
            free(ids);
            return EZDB_ERR_IO;
        }
        uint32_t n = 0;
        for (uint32_t byte_i = 0; byte_i < byte_count && n < idx->count; ++byte_i) {
            unsigned char byte = bits[byte_i];
            while (byte && n < idx->count) {
                unsigned bit = 0;
                while (bit < 8u && !(byte & (1u << bit))) ++bit;
                if (bit >= 8u) break;
                ids[n++] = byte_i * 8u + bit;
                byte &= (unsigned char)~(1u << bit);
            }
        }
        free(bits);
        if (n != idx->count) {
            free(ids);
            return EZDB_ERR_FORMAT;
        }
    } else {
        free(ids);
        return EZDB_ERR_FORMAT;
    }
    *out_ids = ids;
    return EZDB_OK;
}

static int load_intersected_postings(Ezdb* db, EzdbDiskIndex* index, uint64_t index_count, uint32_t* keys, uint32_t key_count, uint32_t** out_ids, uint32_t* out_count)
{
    *out_ids = NULL;
    *out_count = 0;
    QueryIndex* qis = (QueryIndex*)malloc(sizeof(QueryIndex) * key_count);
    if (!qis) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < key_count; ++i) {
        EzdbDiskIndex* idx = find_index(index, index_count, keys[i]);
        if (!idx) {
            free(qis);
            return EZDB_OK;
        }
        qis[i].idx = idx;
    }
    qsort(qis, key_count, sizeof(QueryIndex), query_index_compare);
    uint32_t* current = NULL;
    uint32_t current_count = 0;
    int rc = EZDB_OK;
    for (uint32_t i = 0; i < key_count && rc == EZDB_OK; ++i) {
        uint32_t* ids = NULL;
        rc = load_postings(db, qis[i].idx, &ids);
        if (rc != EZDB_OK) break;
        if (!current) {
            current = ids;
            current_count = qis[i].idx->count;
        } else {
            uint32_t max_next = current_count < qis[i].idx->count ? current_count : qis[i].idx->count;
            uint32_t* next = (uint32_t*)malloc(sizeof(uint32_t) * max_next);
            if (!next && max_next) {
                free(ids);
                rc = EZDB_ERR_MEMORY;
                break;
            }
            uint32_t a = 0, b = 0, n = 0;
            while (a < current_count && b < qis[i].idx->count) {
                if (current[a] == ids[b]) {
                    next[n++] = current[a];
                    ++a;
                    ++b;
                } else if (current[a] < ids[b]) {
                    ++a;
                } else {
                    ++b;
                }
            }
            free(current);
            free(ids);
            current = next;
            current_count = n;
            if (!current_count) break;
        }
    }
    free(qis);
    if (rc != EZDB_OK) {
        free(current);
        return rc;
    }
    *out_ids = current;
    *out_count = current_count;
    return EZDB_OK;
}

int ezdb_build_from_text(const char* input_txt, const char* output_ezdb)
{
    if (!input_txt || !output_ezdb) return EZDB_ERR_ARG;
    double total_start_ms = ezdb_now_ms();
    double parse_ms = 0.0;
    double dfs_ms = 0.0;
    double write_base_ms = 0.0;
    double file_index_ms = 0.0;
    double dir_index_ms = 0.0;
    FILE* in = fopen(input_txt, "rb");
    if (!in) return EZDB_ERR_IO;

    BuildDir* dirs = NULL;
    BuildFile* old_files = NULL;
    uint32_t dir_count = 0, dir_cap = 0, file_count = 0, file_cap = 0;
    DirHashEntry* dir_hash_entries = NULL;
    uint32_t dir_hash_count = 0, dir_hash_cap = 0, dir_bucket_count = 0;
    uint32_t* dir_buckets = NULL;
    char* string_pool = NULL;
    uint32_t string_size = 0, string_cap = 0;
    StringHashEntry* string_entries = NULL;
    uint32_t string_entry_count = 0, string_entry_cap = 0, string_bucket_count = 0;
    uint32_t* string_buckets = NULL;
    PostingBuilder file_builder;
    PostingBuilder dir_builder;
    int file_builder_ready = 0;
    int dir_builder_ready = 0;
    int rc = EZDB_OK;

    if (ensure_capacity((void**)&dirs, sizeof(BuildDir), &dir_cap, 1) != EZDB_OK) {
        fclose(in);
        return EZDB_ERR_MEMORY;
    }
    memset(&dirs[0], 0, sizeof(BuildDir));
    dirs[0].parent = 0;
    dirs[0].first_child = UINT32_MAX;
    dirs[0].next_sibling = UINT32_MAX;
    dirs[0].first_file = UINT32_MAX;
    dirs[0].old_first_file = UINT32_MAX;
    dir_count = 1;

    double stage_start_ms = ezdb_now_ms();
    char line[32768];
    while (fgets(line, sizeof(line), in)) {
        char* path = NULL;
        uint64_t size = 0, modified_time = 0;
        if (!parse_line(line, &path, &size, &modified_time)) continue;
        char* slash = strrchr(path, '\\');
        char* fslash = strrchr(path, '/');
        if (!slash || (fslash && fslash > slash)) slash = fslash;
        const char* name = slash ? slash + 1 : path;
        uint32_t name_len = (uint32_t)strlen(name);
        uint32_t dir_id = 0;
        if (slash) {
            dir_id = get_or_create_path_dir(&dirs, &dir_count, &dir_cap,
                                            &dir_hash_entries, &dir_hash_count, &dir_hash_cap,
                                            &dir_buckets, &dir_bucket_count,
                                            &string_pool, &string_size, &string_cap,
                                            &string_entries, &string_entry_count, &string_entry_cap,
                                            &string_buckets, &string_bucket_count,
                                            path, (uint32_t)(slash - path));
            if (dir_id == UINT32_MAX) {
                rc = EZDB_ERR_MEMORY;
                break;
            }
        }
        rc = append_file(&old_files, &file_count, &file_cap, dirs, dir_id,
                         &string_pool, &string_size, &string_cap,
                         &string_entries, &string_entry_count, &string_entry_cap,
                         &string_buckets, &string_bucket_count,
                         name, name_len, size, modified_time);
        if (rc != EZDB_OK) break;
    }
    fclose(in);
    parse_ms = ezdb_now_ms() - stage_start_ms;

    BuildFile* files = NULL;
    if (rc == EZDB_OK) {
        files = (BuildFile*)malloc(sizeof(BuildFile) * (size_t)file_count);
        if (!files && file_count) rc = EZDB_ERR_MEMORY;
    }
    if (rc == EZDB_OK) {
        stage_start_ms = ezdb_now_ms();
        uint32_t assigned = dfs_assign(dirs, old_files, files, 0, 0);
        if (assigned != file_count) rc = EZDB_ERR_FORMAT;
        dfs_ms = ezdb_now_ms() - stage_start_ms;
    }
    if (rc == EZDB_OK) {
        FILE* out = fopen(output_ezdb, "wb");
        if (!out) rc = EZDB_ERR_IO;
        if (out) {
            EzdbHeader header;
            memset(&header, 0, sizeof(header));
            memcpy(header.magic, EZDB_MAGIC, 8);
            header.version = EZDB_VERSION;
            header.header_size = sizeof(EzdbHeader);
            header.file_count = file_count;
            header.active_count = file_count;
            header.dir_count = dir_count;
            fwrite(&header, sizeof(header), 1, out);

            stage_start_ms = ezdb_now_ms();
            header.file_records_offset = (uint64_t)ftell(out);
            uint64_t file_records_written = 0;
            rc = write_file_records_compact(out, files, file_count, &file_records_written);
            header.file_records_size = (uint64_t)ftell(out) - header.file_records_offset;

            header.dir_records_offset = (uint64_t)ftell(out);
            for (uint32_t i = 0; i < dir_count && rc == EZDB_OK; ++i) {
                EzdbDiskDir d;
                d.parent_dir_id = dirs[i].parent;
                d.name_offset = dirs[i].name_offset;
                d.name_len = dirs[i].name_len;
                d.first_file_id = dirs[i].first_file_id;
                d.file_count = dirs[i].file_count;
                if (fwrite(&d, sizeof(d), 1, out) != 1) rc = EZDB_ERR_IO;
            }
            header.dir_records_size = (uint64_t)ftell(out) - header.dir_records_offset;

            header.strings_offset = (uint64_t)ftell(out);
            header.strings_size = string_size;
            if (string_size && fwrite(string_pool, 1, string_size, out) != string_size) rc = EZDB_ERR_IO;
            write_base_ms = ezdb_now_ms() - stage_start_ms;

            header.postings_offset = (uint64_t)ftell(out);
            EzdbDiskIndex* file_index = NULL;
            EzdbDiskIndex* dir_index = NULL;
            uint32_t file_index_count = 0, dir_index_count = 0;
            uint64_t file_postings_size = 0, dir_postings_size = 0;
            if (rc == EZDB_OK) {
                stage_start_ms = ezdb_now_ms();
                rc = posting_builder_init(&file_builder, 65536u);
                if (rc == EZDB_OK) file_builder_ready = 1;
            }
            if (rc == EZDB_OK) {
                for (uint32_t i = 0; i < file_count; ++i) {
                    rc = add_text_grams(&file_builder, string_pool + files[i].name_offset, i);
                    if (rc != EZDB_OK) break;
                }
            }
            if (rc == EZDB_OK) rc = write_postings(out, &file_builder, file_count, &file_index, &file_index_count, &file_postings_size);
            if (rc == EZDB_OK) file_index_ms = ezdb_now_ms() - stage_start_ms;
            if (file_builder_ready) {
                posting_builder_free(&file_builder);
                file_builder_ready = 0;
            }
            if (rc == EZDB_OK) {
                stage_start_ms = ezdb_now_ms();
                rc = posting_builder_init(&dir_builder, 65536u);
                if (rc == EZDB_OK) dir_builder_ready = 1;
            }
            if (rc == EZDB_OK) {
                for (uint32_t i = 1; i < dir_count; ++i) {
                    rc = add_text_grams(&dir_builder, string_pool + dirs[i].name_offset, i);
                    if (rc != EZDB_OK) break;
                }
            }
            if (rc == EZDB_OK) rc = write_postings(out, &dir_builder, dir_count, &dir_index, &dir_index_count, &dir_postings_size);
            if (rc == EZDB_OK) dir_index_ms = ezdb_now_ms() - stage_start_ms;
            if (dir_builder_ready) {
                posting_builder_free(&dir_builder);
                dir_builder_ready = 0;
            }
            header.postings_size = file_postings_size + dir_postings_size;

            header.file_index_offset = (uint64_t)ftell(out);
            header.file_index_count = file_index_count;
            if (rc == EZDB_OK && file_index_count && fwrite(file_index, sizeof(EzdbDiskIndex), file_index_count, out) != file_index_count) rc = EZDB_ERR_IO;

            for (uint32_t i = 0; i < dir_index_count; ++i) dir_index[i].offset += file_postings_size;
            header.dir_index_offset = (uint64_t)ftell(out);
            header.dir_index_count = dir_index_count;
            if (rc == EZDB_OK && dir_index_count && fwrite(dir_index, sizeof(EzdbDiskIndex), dir_index_count, out) != dir_index_count) rc = EZDB_ERR_IO;

            header.reserved_offset = (uint64_t)ftell(out);
            header.reserved_size = 0;
            if (rc == EZDB_OK && (fseek(out, 0, SEEK_SET) != 0 || fwrite(&header, sizeof(header), 1, out) != 1)) rc = EZDB_ERR_IO;
            free(file_index);
            free(dir_index);
            fclose(out);
        }
    }

    free(dirs);
    free(old_files);
    free(files);
    free(dir_hash_entries);
    free(dir_buckets);
    free(string_pool);
    free(string_entries);
    free(string_buckets);
    if (file_builder_ready) posting_builder_free(&file_builder);
    if (dir_builder_ready) posting_builder_free(&dir_builder);
    if (rc == EZDB_OK) {
        double total_ms = ezdb_now_ms() - total_start_ms;
        printf("build_parse_tree_ms: %.2f\n", parse_ms);
        printf("build_dfs_ms: %.2f\n", dfs_ms);
        printf("build_write_base_ms: %.2f\n", write_base_ms);
        printf("build_file_index_ms: %.2f\n", file_index_ms);
        printf("build_dir_index_ms: %.2f\n", dir_index_ms);
        printf("build_internal_total_ms: %.2f\n", total_ms);
    }
    return rc;
}

int ezdb_open(const char* path, Ezdb** out_db)
{
    if (!path || !out_db) return EZDB_ERR_ARG;
    *out_db = NULL;
    FILE* fp = fopen(path, "rb");
    if (!fp) return EZDB_ERR_IO;
    Ezdb* db = (Ezdb*)calloc(1, sizeof(Ezdb));
    if (!db) {
        fclose(fp);
        return EZDB_ERR_MEMORY;
    }
    db->fp = fp;
    db->path = ezdb_strdup_range(path, strlen(path));
    if (fread(&db->header, sizeof(db->header), 1, fp) != 1 ||
        memcmp(db->header.magic, EZDB_MAGIC, 8) != 0 ||
        db->header.version != EZDB_VERSION ||
        db->header.header_size != sizeof(EzdbHeader)) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    if (db->header.file_count > UINT32_MAX || db->header.dir_count > UINT32_MAX ||
        db->header.file_index_count > UINT32_MAX || db->header.dir_index_count > UINT32_MAX ||
        db->header.strings_size > UINT32_MAX) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }
    db->files = (EzdbDiskFile*)malloc(sizeof(EzdbDiskFile) * (size_t)db->header.file_count);
    db->dirs = (EzdbDiskDir*)malloc((size_t)db->header.dir_records_size);
    db->strings = (char*)malloc((size_t)db->header.strings_size + 1u);
    db->file_index = (EzdbDiskIndex*)malloc(sizeof(EzdbDiskIndex) * (size_t)db->header.file_index_count);
    db->dir_index = (EzdbDiskIndex*)malloc(sizeof(EzdbDiskIndex) * (size_t)db->header.dir_index_count);
    if ((!db->files && db->header.file_count) || (!db->dirs && db->header.dir_records_size) ||
        (!db->strings && db->header.strings_size) || (!db->file_index && db->header.file_index_count) ||
        (!db->dir_index && db->header.dir_index_count)) {
        ezdb_close(db);
        return EZDB_ERR_MEMORY;
    }
    if (fseek(fp, (long)db->header.file_records_offset, SEEK_SET) != 0 ||
        read_file_records_compact(fp, db->header.file_records_size, db->files, (uint32_t)db->header.file_count) != EZDB_OK ||
        fseek(fp, (long)db->header.dir_records_offset, SEEK_SET) != 0 ||
        fread(db->dirs, 1, (size_t)db->header.dir_records_size, fp) != (size_t)db->header.dir_records_size ||
        fseek(fp, (long)db->header.strings_offset, SEEK_SET) != 0 ||
        fread(db->strings, 1, (size_t)db->header.strings_size, fp) != (size_t)db->header.strings_size ||
        fseek(fp, (long)db->header.file_index_offset, SEEK_SET) != 0 ||
        fread(db->file_index, sizeof(EzdbDiskIndex), (size_t)db->header.file_index_count, fp) != (size_t)db->header.file_index_count ||
        fseek(fp, (long)db->header.dir_index_offset, SEEK_SET) != 0 ||
        fread(db->dir_index, sizeof(EzdbDiskIndex), (size_t)db->header.dir_index_count, fp) != (size_t)db->header.dir_index_count) {
        ezdb_close(db);
        return EZDB_ERR_IO;
    }
    db->strings[db->header.strings_size] = '\0';
    *out_db = db;
    return EZDB_OK;
}

void ezdb_close(Ezdb* db)
{
    if (!db) return;
    if (db->fp) fclose(db->fp);
    free(db->path);
    free(db->files);
    free(db->dirs);
    free(db->strings);
    free(db->file_index);
    free(db->dir_index);
    free(db);
}

uint32_t ezdb_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.file_count : 0;
}

uint32_t ezdb_active_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.active_count : 0;
}

uint64_t ezdb_file_size(Ezdb* db)
{
    return db && db->fp ? file_size_of(db->fp) : 0;
}

int ezdb_stats(Ezdb* db, EzdbStats* out_stats)
{
    if (!db || !out_stats) return EZDB_ERR_ARG;
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->record_count = (uint32_t)db->header.file_count;
    out_stats->active_count = (uint32_t)db->header.active_count;
    out_stats->file_size = ezdb_file_size(db);
    out_stats->records_size = db->header.file_records_size;
    out_stats->dirs_size = db->header.dir_records_size;
    out_stats->names_size = db->header.strings_size;
    out_stats->index_size = db->header.file_index_count * sizeof(EzdbDiskIndex) +
                            db->header.dir_index_count * sizeof(EzdbDiskIndex);
    out_stats->postings_size = db->header.postings_size;
    return EZDB_OK;
}

int ezdb_get_by_id(Ezdb* db, uint32_t id, EzdbSearchResult* out_result)
{
    return build_result_path(db, id, out_result);
}

void ezdb_free_result(EzdbSearchResult* result)
{
    if (!result) return;
    free(result->path);
    memset(result, 0, sizeof(*result));
}

int ezdb_search_path(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data)
{
    if (!db || !keyword || !callback) return EZDB_ERR_ARG;
    size_t key_len = strlen(keyword);
    if (!key_len) return EZDB_OK;
    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    int rc = build_query_keys(keyword, &keys, &key_count);
    if (rc != EZDB_OK) return rc;

    uint32_t* file_ids = NULL;
    uint32_t file_count = 0;
    uint32_t* dir_ids = NULL;
    uint32_t dir_count = 0;
    rc = load_intersected_postings(db, db->file_index, db->header.file_index_count, keys, key_count, &file_ids, &file_count);
    if (rc == EZDB_OK) rc = load_intersected_postings(db, db->dir_index, db->header.dir_index_count, keys, key_count, &dir_ids, &dir_count);
    free(keys);
    if (rc != EZDB_OK) {
        free(file_ids);
        free(dir_ids);
        return rc;
    }

    unsigned char* seen = (unsigned char*)calloc((size_t)db->header.file_count, 1);
    if (!seen && db->header.file_count) {
        free(file_ids);
        free(dir_ids);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < file_count; ++i) {
        if (file_ids[i] < db->header.file_count) seen[file_ids[i]] = 1;
    }
    for (uint32_t i = 0; i < dir_count; ++i) {
        uint32_t dir_id = dir_ids[i];
        if (dir_id >= db->header.dir_count) continue;
        EzdbDiskDir* d = &db->dirs[dir_id];
        uint32_t end = d->first_file_id + d->file_count;
        if (end > db->header.file_count) end = (uint32_t)db->header.file_count;
        for (uint32_t id = d->first_file_id; id < end; ++id) seen[id] = 1;
    }

    uint32_t emitted = 0;
    for (uint32_t id = 0; id < db->header.file_count; ++id) {
        if (limit && emitted >= limit) break;
        if (!seen[id]) continue;
        if (key_len > EZDB_GRAM3 && !record_contains_keyword(db, id, keyword, key_len)) continue;
        EzdbSearchResult result;
        rc = build_result_path(db, id, &result);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        callback(&result, user_data);
        ++emitted;
        ezdb_free_result(&result);
    }
    free(seen);
    free(file_ids);
    free(dir_ids);
    return rc;
}

int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id)
{
    (void)db;
    (void)record;
    (void)out_id;
    return EZDB_ERR_READ_ONLY;
}

int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record)
{
    (void)db;
    (void)id;
    (void)record;
    return EZDB_ERR_READ_ONLY;
}

int ezdb_delete(Ezdb* db, uint32_t id)
{
    (void)db;
    (void)id;
    return EZDB_ERR_READ_ONLY;
}

const char* ezdb_error_message(int code)
{
    switch (code) {
    case EZDB_OK: return "ok";
    case EZDB_ERR_ARG: return "invalid argument";
    case EZDB_ERR_IO: return "I/O error";
    case EZDB_ERR_FORMAT: return "invalid ezdb format";
    case EZDB_ERR_MEMORY: return "out of memory";
    case EZDB_ERR_NOT_FOUND: return "not found";
    case EZDB_ERR_READ_ONLY: return "mutation is not implemented in this v3 build";
    default: return "unknown error";
    }
}
