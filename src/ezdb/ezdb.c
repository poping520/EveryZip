/*
 * ezdb v2 path index design
 * =========================
 *
 * ezdb v2 intentionally breaks compatibility with the first prototype. The new
 * priority is fast substring search for any path keyword, so every query goes
 * through an index instead of falling back to a full record scan for directory
 * terms such as "aaa" or "node_modules".
 *
 * The file still stores paths as dir + name to reuse common directory prefixes,
 * but the inverted index is built over the complete reconstructed path. The
 * index contains 1-byte, 2-byte, and 3-byte grams. Query length selects the gram
 * width directly: 1 for one byte, 2 for two bytes, and 3 for longer keywords.
 * Postings are sorted record ids encoded as delta varints. During search, ezdb
 * loads only the relevant postings, intersects them from the shortest list to
 * the longest, and finally verifies each candidate against the full path.
 *
 * Open keeps records, index entries, dir offsets, dir text bytes, and name text
 * bytes in memory. That raises steady-state memory compared with the v1 lazy
 * reader, but removes per-candidate fseek/malloc work from hot search. A result
 * path is allocated only when get/search needs to return it to the caller.
 *
 * Matching is ASCII case-insensitive. UTF-8 bytes are preserved and searchable
 * as byte substrings, but full Unicode case folding is intentionally left for a
 * later production pass.
 */

#include "ezdb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EZDB_MAGIC "EZDB0002"
#define EZDB_VERSION 2u
#define EZDB_FLAG_DELETED 1u
#define EZDB_GRAM1 1u
#define EZDB_GRAM2 2u
#define EZDB_GRAM3 3u

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
    uint64_t record_count;
    uint64_t active_count;
    uint64_t dir_count;
    uint64_t records_offset;
    uint64_t dirs_offset;
    uint64_t dirs_size;
    uint64_t names_offset;
    uint64_t names_size;
    uint64_t index_offset;
    uint64_t index_count;
    uint64_t postings_offset;
    uint64_t postings_size;
    uint64_t reserved_offset;
    uint64_t reserved_size;
} EzdbHeader;

typedef struct EzdbDiskRecord {
    uint32_t dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t flags;
    uint64_t size;
    uint64_t modified_time;
} EzdbDiskRecord;

typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint64_t offset;
} EzdbDiskIndex;

typedef struct DirEntry {
    char* text;
    uint32_t offset;
    uint32_t len;
    uint32_t next_hash;
} DirEntry;

typedef struct BuildRecord {
    char* path;
    uint32_t dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint64_t size;
    uint64_t modified_time;
} BuildRecord;

typedef struct GramPair {
    uint32_t key;
    uint32_t id;
} GramPair;

typedef struct QueryIndex {
    const EzdbDiskIndex* idx;
} QueryIndex;

struct Ezdb {
    FILE* fp;
    char* path;
    EzdbHeader header;
    EzdbDiskRecord* records;
    EzdbDiskIndex* index;
    uint32_t* dir_offsets;
    unsigned char* dirs_data;
    char* names_data;
};

static uint64_t file_size_of(FILE* fp)
{
    long old_pos = ftell(fp);
    if (fseek(fp, 0, SEEK_END) != 0) return 0;
    long size = ftell(fp);
    fseek(fp, old_pos, SEEK_SET);
    return size < 0 ? 0 : (uint64_t)size;
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

static unsigned char fold_ascii_byte(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static uint32_t make_gram_key(uint32_t kind, const unsigned char* s)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < kind; ++i) {
        value = (value << 8) | (uint32_t)fold_ascii_byte(s[i]);
    }
    return (kind << 24) | value;
}

static int parse_line(char* line, char** out_path, uint64_t* out_size, uint64_t* out_mtime)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

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

static int split_path(const char* path, char** out_dir, char** out_name)
{
    const char* slash = strrchr(path, '\\');
    const char* fslash = strrchr(path, '/');
    if (!slash || (fslash && fslash > slash)) slash = fslash;

    if (!slash) {
        *out_dir = ezdb_strdup_range("", 0);
        *out_name = ezdb_strdup_range(path, strlen(path));
    } else {
        *out_dir = ezdb_strdup_range(path, (size_t)(slash - path));
        *out_name = ezdb_strdup_range(slash + 1, strlen(slash + 1));
    }
    if (!*out_dir || !*out_name) return EZDB_ERR_MEMORY;
    return EZDB_OK;
}

static uint32_t fnv1a_hash(const char* text)
{
    uint32_t hash = 2166136261u;
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static int init_dir_hash(uint32_t** buckets, uint32_t bucket_count)
{
    *buckets = (uint32_t*)malloc(sizeof(uint32_t) * bucket_count);
    if (!*buckets) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < bucket_count; ++i) (*buckets)[i] = UINT32_MAX;
    return EZDB_OK;
}

static int rebuild_dir_hash(DirEntry* dirs, uint32_t dir_count, uint32_t** buckets, uint32_t* bucket_count)
{
    uint32_t new_count = *bucket_count ? *bucket_count * 2u : 4096u;
    uint32_t* new_buckets = NULL;
    if (init_dir_hash(&new_buckets, new_count) != EZDB_OK) return EZDB_ERR_MEMORY;

    for (uint32_t i = 0; i < dir_count; ++i) {
        uint32_t bucket = fnv1a_hash(dirs[i].text) & (new_count - 1u);
        dirs[i].next_hash = new_buckets[bucket];
        new_buckets[bucket] = i;
    }

    free(*buckets);
    *buckets = new_buckets;
    *bucket_count = new_count;
    return EZDB_OK;
}

static uint32_t find_or_add_dir(DirEntry** dirs,
                                uint32_t* dir_count,
                                uint32_t* dir_cap,
                                uint32_t** dir_buckets,
                                uint32_t* dir_bucket_count,
                                char* dir)
{
    if (!*dir_buckets && rebuild_dir_hash(*dirs, *dir_count, dir_buckets, dir_bucket_count) != EZDB_OK) {
        free(dir);
        return UINT32_MAX;
    }

    uint32_t bucket = fnv1a_hash(dir) & (*dir_bucket_count - 1u);
    for (uint32_t i = (*dir_buckets)[bucket]; i != UINT32_MAX; i = (*dirs)[i].next_hash) {
        if (strcmp((*dirs)[i].text, dir) == 0) {
            free(dir);
            return i;
        }
    }

    if (ensure_capacity((void**)dirs, sizeof(DirEntry), dir_cap, *dir_count + 1) != EZDB_OK) {
        free(dir);
        return UINT32_MAX;
    }
    if (*dir_count + 1 > *dir_bucket_count * 2u &&
        rebuild_dir_hash(*dirs, *dir_count, dir_buckets, dir_bucket_count) != EZDB_OK) {
        free(dir);
        return UINT32_MAX;
    }

    bucket = fnv1a_hash(dir) & (*dir_bucket_count - 1u);
    uint32_t id = *dir_count;
    (*dirs)[id].text = dir;
    (*dirs)[id].offset = 0;
    (*dirs)[id].len = (uint32_t)strlen(dir);
    (*dirs)[id].next_hash = (*dir_buckets)[bucket];
    (*dir_buckets)[bucket] = id;
    *dir_count += 1;
    return id;
}

static int add_name(char** names, uint32_t* names_size, uint32_t* names_cap, const char* name, uint32_t* out_offset, uint32_t* out_len)
{
    uint32_t len = (uint32_t)strlen(name);
    if (ensure_capacity((void**)names, 1, names_cap, *names_size + len + 1) != EZDB_OK) return EZDB_ERR_MEMORY;
    *out_offset = *names_size;
    *out_len = len;
    memcpy(*names + *names_size, name, len + 1);
    *names_size += len + 1;
    return EZDB_OK;
}

static int gram_pair_compare(const void* a, const void* b)
{
    const GramPair* pa = (const GramPair*)a;
    const GramPair* pb = (const GramPair*)b;
    if (pa->key != pb->key) return pa->key < pb->key ? -1 : 1;
    if (pa->id != pb->id) return pa->id < pb->id ? -1 : 1;
    return 0;
}

static int gram_u32_compare(const void* a, const void* b)
{
    uint32_t av = *(const uint32_t*)a;
    uint32_t bv = *(const uint32_t*)b;
    if (av == bv) return 0;
    return av < bv ? -1 : 1;
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

static int add_record_grams(GramPair** pairs, uint32_t* pair_count, uint32_t* pair_cap, const char* path, uint32_t id)
{
    size_t len = strlen(path);
    if (!len) return EZDB_OK;

    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    uint32_t key_cap = 0;

    for (uint32_t kind = EZDB_GRAM1; kind <= EZDB_GRAM3; ++kind) {
        if (len < kind) continue;
        for (size_t i = 0; i + kind <= len; ++i) {
            if (ensure_capacity((void**)&keys, sizeof(uint32_t), &key_cap, key_count + 1) != EZDB_OK) {
                free(keys);
                return EZDB_ERR_MEMORY;
            }
            keys[key_count++] = make_gram_key(kind, (const unsigned char*)path + i);
        }
    }

    qsort(keys, key_count, sizeof(uint32_t), gram_u32_compare);
    uint32_t last = UINT32_MAX;
    for (uint32_t i = 0; i < key_count; ++i) {
        if (keys[i] == last) continue;
        last = keys[i];
        if (ensure_capacity((void**)pairs, sizeof(GramPair), pair_cap, *pair_count + 1) != EZDB_OK) {
            free(keys);
            return EZDB_ERR_MEMORY;
        }
        (*pairs)[*pair_count].key = keys[i];
        (*pairs)[*pair_count].id = id;
        *pair_count += 1;
    }
    free(keys);
    return EZDB_OK;
}

static int build_query_keys(const char* keyword, uint32_t** out_keys, uint32_t* out_count)
{
    size_t len = strlen(keyword);
    if (!len) return EZDB_ERR_ARG;
    uint32_t kind = len >= EZDB_GRAM3 ? EZDB_GRAM3 : (uint32_t)len;
    uint32_t count = (uint32_t)(len - kind + 1u);
    uint32_t* keys = (uint32_t*)malloc(sizeof(uint32_t) * count);
    if (!keys) return EZDB_ERR_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        keys[i] = make_gram_key(kind, (const unsigned char*)keyword + i);
    }
    qsort(keys, count, sizeof(uint32_t), gram_u32_compare);

    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i && keys[i] == keys[i - 1]) continue;
        keys[n++] = keys[i];
    }
    *out_keys = keys;
    *out_count = n;
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

static const char* record_dir_text(Ezdb* db, const EzdbDiskRecord* rec, uint32_t* out_len)
{
    uint32_t offset = db->dir_offsets[rec->dir_id];
    uint32_t len = 0;
    memcpy(&len, db->dirs_data + offset, sizeof(len));
    if (out_len) *out_len = len;
    return (const char*)(db->dirs_data + offset + sizeof(uint32_t));
}

static const char* record_name_text(Ezdb* db, const EzdbDiskRecord* rec)
{
    return db->names_data + rec->name_offset;
}

static int build_result_path(Ezdb* db, uint32_t id, EzdbSearchResult* out_result)
{
    if (!db || !out_result) return EZDB_ERR_ARG;
    if (id >= db->header.record_count) return EZDB_ERR_NOT_FOUND;
    memset(out_result, 0, sizeof(*out_result));

    EzdbDiskRecord* rec = &db->records[id];
    if (rec->flags & EZDB_FLAG_DELETED) return EZDB_ERR_NOT_FOUND;

    uint32_t dir_len = 0;
    const char* dir = record_dir_text(db, rec, &dir_len);
    const char* name = record_name_text(db, rec);
    size_t path_len = (size_t)dir_len + (dir_len ? 1u : 0u) + rec->name_len;
    char* path = (char*)malloc(path_len + 1u);
    if (!path) return EZDB_ERR_MEMORY;

    if (dir_len) {
        memcpy(path, dir, dir_len);
        path[dir_len] = '\\';
        memcpy(path + dir_len + 1u, name, rec->name_len + 1u);
    } else {
        memcpy(path, name, rec->name_len + 1u);
    }

    out_result->id = id;
    out_result->path = path;
    out_result->size = rec->size;
    out_result->modified_time = rec->modified_time;
    return EZDB_OK;
}

static int contains_ascii_casefold_bytes(const char* text, size_t text_len, const char* needle, size_t needle_len)
{
    if (!needle_len) return 1;
    if (needle_len > text_len) return 0;
    for (size_t i = 0; i + needle_len <= text_len; ++i) {
        size_t j = 0;
        while (j < needle_len &&
               fold_ascii_byte((unsigned char)text[i + j]) == fold_ascii_byte((unsigned char)needle[j])) {
            ++j;
        }
        if (j == needle_len) return 1;
    }
    return 0;
}

static int record_contains_keyword(Ezdb* db, uint32_t id, const char* keyword, size_t key_len)
{
    const EzdbDiskRecord* rec = &db->records[id];
    if (rec->flags & EZDB_FLAG_DELETED) return 0;

    uint32_t dir_len = 0;
    const char* dir = record_dir_text(db, rec, &dir_len);
    const char* name = record_name_text(db, rec);

    if (contains_ascii_casefold_bytes(dir, dir_len, keyword, key_len)) return 1;
    if (contains_ascii_casefold_bytes(name, rec->name_len, keyword, key_len)) return 1;
    if (key_len == 1 && (*keyword == '\\' || *keyword == '/')) return dir_len > 0;

    if (dir_len && key_len <= (size_t)dir_len + 1u + rec->name_len) {
        size_t path_len = (size_t)dir_len + 1u + rec->name_len;
        char* path = (char*)malloc(path_len + 1u);
        if (!path) return 0;
        memcpy(path, dir, dir_len);
        path[dir_len] = '\\';
        memcpy(path + dir_len + 1u, name, rec->name_len + 1u);
        int matched = contains_ascii_casefold_bytes(path, path_len, keyword, key_len);
        free(path);
        return matched;
    }
    return 0;
}

int ezdb_build_from_text(const char* input_txt, const char* output_ezdb)
{
    if (!input_txt || !output_ezdb) return EZDB_ERR_ARG;

    FILE* in = fopen(input_txt, "rb");
    if (!in) return EZDB_ERR_IO;

    BuildRecord* records = NULL;
    uint32_t record_count = 0;
    uint32_t record_cap = 0;
    DirEntry* dirs = NULL;
    uint32_t dir_count = 0;
    uint32_t dir_cap = 0;
    uint32_t* dir_buckets = NULL;
    uint32_t dir_bucket_count = 0;
    char* names = NULL;
    uint32_t names_size = 0;
    uint32_t names_cap = 0;
    GramPair* pairs = NULL;
    uint32_t pair_count = 0;
    uint32_t pair_cap = 0;
    int rc = EZDB_OK;

    char line[32768];
    while (fgets(line, sizeof(line), in)) {
        char* path = NULL;
        uint64_t size = 0;
        uint64_t modified_time = 0;
        if (!parse_line(line, &path, &size, &modified_time)) continue;

        char* path_copy = ezdb_strdup_range(path, strlen(path));
        char* dir = NULL;
        char* name = NULL;
        if (!path_copy || split_path(path, &dir, &name) != EZDB_OK) {
            rc = EZDB_ERR_MEMORY;
            free(path_copy);
            free(dir);
            free(name);
            break;
        }

        uint32_t dir_id = find_or_add_dir(&dirs, &dir_count, &dir_cap, &dir_buckets, &dir_bucket_count, dir);
        if (dir_id == UINT32_MAX) {
            rc = EZDB_ERR_MEMORY;
            free(path_copy);
            free(name);
            break;
        }

        if (ensure_capacity((void**)&records, sizeof(BuildRecord), &record_cap, record_count + 1) != EZDB_OK) {
            rc = EZDB_ERR_MEMORY;
            free(path_copy);
            free(name);
            break;
        }

        BuildRecord* rec = &records[record_count];
        rec->path = path_copy;
        rec->dir_id = dir_id;
        rec->size = size;
        rec->modified_time = modified_time;
        rc = add_name(&names, &names_size, &names_cap, name, &rec->name_offset, &rec->name_len);
        free(name);
        if (rc != EZDB_OK) break;

        rc = add_record_grams(&pairs, &pair_count, &pair_cap, path_copy, record_count);
        if (rc != EZDB_OK) break;
        ++record_count;
    }
    fclose(in);

    if (rc == EZDB_OK) {
        FILE* out = fopen(output_ezdb, "wb");
        if (!out) rc = EZDB_ERR_IO;
        if (out) {
            EzdbHeader header;
            memset(&header, 0, sizeof(header));
            memcpy(header.magic, EZDB_MAGIC, 8);
            header.version = EZDB_VERSION;
            header.header_size = sizeof(EzdbHeader);
            header.record_count = record_count;
            header.active_count = record_count;
            header.dir_count = dir_count;
            fwrite(&header, sizeof(header), 1, out);

            header.records_offset = (uint64_t)ftell(out);
            for (uint32_t i = 0; i < record_count && rc == EZDB_OK; ++i) {
                EzdbDiskRecord dr;
                dr.dir_id = records[i].dir_id;
                dr.name_offset = records[i].name_offset;
                dr.name_len = records[i].name_len;
                dr.flags = 0;
                dr.size = records[i].size;
                dr.modified_time = records[i].modified_time;
                if (fwrite(&dr, sizeof(dr), 1, out) != 1) rc = EZDB_ERR_IO;
            }

            header.dirs_offset = (uint64_t)ftell(out);
            for (uint32_t i = 0; i < dir_count && rc == EZDB_OK; ++i) {
                dirs[i].offset = (uint32_t)(ftell(out) - (long)header.dirs_offset);
                uint32_t len = dirs[i].len;
                if (fwrite(&len, sizeof(len), 1, out) != 1 ||
                    fwrite(dirs[i].text, 1, len + 1u, out) != len + 1u) {
                    rc = EZDB_ERR_IO;
                }
            }
            header.dirs_size = (uint64_t)ftell(out) - header.dirs_offset;

            header.names_offset = (uint64_t)ftell(out);
            header.names_size = names_size;
            if (names_size && fwrite(names, 1, names_size, out) != names_size) rc = EZDB_ERR_IO;

            EzdbDiskIndex* indexes = NULL;
            uint32_t index_count = 0;
            uint32_t index_cap = 0;
            if (rc == EZDB_OK && pair_count) {
                qsort(pairs, pair_count, sizeof(GramPair), gram_pair_compare);
                header.postings_offset = (uint64_t)ftell(out);
                uint32_t i = 0;
                uint64_t postings_written = 0;
                while (i < pair_count && rc == EZDB_OK) {
                    uint32_t key = pairs[i].key;
                    uint32_t count = 0;
                    uint32_t prev_id = 0;
                    uint64_t local_offset = postings_written;
                    while (i < pair_count && pairs[i].key == key) {
                        uint32_t id = pairs[i].id;
                        uint32_t delta = count == 0 ? id : id - prev_id;
                        rc = write_varuint(out, delta, &postings_written);
                        if (rc != EZDB_OK) break;
                        prev_id = id;
                        ++count;
                        ++i;
                    }
                    if (rc != EZDB_OK) break;
                    if (ensure_capacity((void**)&indexes, sizeof(EzdbDiskIndex), &index_cap, index_count + 1) != EZDB_OK) {
                        rc = EZDB_ERR_MEMORY;
                        break;
                    }
                    indexes[index_count].key = key;
                    indexes[index_count].count = count;
                    indexes[index_count].offset = local_offset;
                    ++index_count;
                }
                header.postings_size = postings_written;
            }

            if (rc == EZDB_OK) {
                header.index_offset = (uint64_t)ftell(out);
                header.index_count = index_count;
                if (index_count && fwrite(indexes, sizeof(EzdbDiskIndex), index_count, out) != index_count) rc = EZDB_ERR_IO;
            }
            if (rc == EZDB_OK) {
                header.reserved_offset = (uint64_t)ftell(out);
                header.reserved_size = 0;
                if (fseek(out, 0, SEEK_SET) != 0 || fwrite(&header, sizeof(header), 1, out) != 1) rc = EZDB_ERR_IO;
            }
            free(indexes);
            fclose(out);
        }
    }

    for (uint32_t i = 0; i < record_count; ++i) free(records[i].path);
    for (uint32_t i = 0; i < dir_count; ++i) free(dirs[i].text);
    free(records);
    free(dirs);
    free(dir_buckets);
    free(names);
    free(pairs);
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

    if (db->header.record_count > UINT32_MAX || db->header.dir_count > UINT32_MAX ||
        db->header.index_count > UINT32_MAX || db->header.dirs_size > UINT32_MAX ||
        db->header.names_size > UINT32_MAX) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }

    db->records = (EzdbDiskRecord*)malloc(sizeof(EzdbDiskRecord) * (size_t)db->header.record_count);
    db->index = (EzdbDiskIndex*)malloc(sizeof(EzdbDiskIndex) * (size_t)db->header.index_count);
    db->dirs_data = (unsigned char*)malloc((size_t)db->header.dirs_size);
    db->names_data = (char*)malloc((size_t)db->header.names_size + 1u);
    if ((!db->records && db->header.record_count) ||
        (!db->index && db->header.index_count) ||
        (!db->dirs_data && db->header.dirs_size) ||
        (!db->names_data && db->header.names_size)) {
        ezdb_close(db);
        return EZDB_ERR_MEMORY;
    }

    if (fseek(fp, (long)db->header.records_offset, SEEK_SET) != 0 ||
        fread(db->records, sizeof(EzdbDiskRecord), (size_t)db->header.record_count, fp) != (size_t)db->header.record_count ||
        fseek(fp, (long)db->header.index_offset, SEEK_SET) != 0 ||
        fread(db->index, sizeof(EzdbDiskIndex), (size_t)db->header.index_count, fp) != (size_t)db->header.index_count ||
        fseek(fp, (long)db->header.dirs_offset, SEEK_SET) != 0 ||
        fread(db->dirs_data, 1, (size_t)db->header.dirs_size, fp) != (size_t)db->header.dirs_size ||
        fseek(fp, (long)db->header.names_offset, SEEK_SET) != 0 ||
        fread(db->names_data, 1, (size_t)db->header.names_size, fp) != (size_t)db->header.names_size) {
        ezdb_close(db);
        return EZDB_ERR_IO;
    }
    db->names_data[db->header.names_size] = '\0';

    db->dir_offsets = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)db->header.dir_count);
    if (!db->dir_offsets && db->header.dir_count) {
        ezdb_close(db);
        return EZDB_ERR_MEMORY;
    }
    uint32_t dir_count = 0;
    uint64_t pos = 0;
    while (pos < db->header.dirs_size) {
        if (dir_count >= db->header.dir_count || pos + sizeof(uint32_t) > db->header.dirs_size) {
            ezdb_close(db);
            return EZDB_ERR_FORMAT;
        }
        db->dir_offsets[dir_count++] = (uint32_t)pos;
        uint32_t len = 0;
        memcpy(&len, db->dirs_data + pos, sizeof(len));
        pos += sizeof(uint32_t) + (uint64_t)len + 1u;
    }
    if (dir_count != db->header.dir_count) {
        ezdb_close(db);
        return EZDB_ERR_FORMAT;
    }

    *out_db = db;
    return EZDB_OK;
}

void ezdb_close(Ezdb* db)
{
    if (!db) return;
    if (db->fp) fclose(db->fp);
    free(db->path);
    free(db->records);
    free(db->index);
    free(db->dir_offsets);
    free(db->dirs_data);
    free(db->names_data);
    free(db);
}

uint32_t ezdb_count(Ezdb* db)
{
    return db ? (uint32_t)db->header.record_count : 0;
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
    out_stats->record_count = (uint32_t)db->header.record_count;
    out_stats->active_count = (uint32_t)db->header.active_count;
    out_stats->file_size = ezdb_file_size(db);
    out_stats->records_size = db->header.record_count * sizeof(EzdbDiskRecord);
    out_stats->dirs_size = db->header.dirs_size;
    out_stats->names_size = db->header.names_size;
    out_stats->index_size = db->header.index_count * sizeof(EzdbDiskIndex);
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

static EzdbDiskIndex* find_index(Ezdb* db, uint32_t key)
{
    EzdbDiskIndex search_key;
    search_key.key = key;
    search_key.count = 0;
    search_key.offset = 0;
    return (EzdbDiskIndex*)bsearch(&search_key, db->index, (size_t)db->header.index_count, sizeof(EzdbDiskIndex), index_compare);
}

static int load_postings(Ezdb* db, const EzdbDiskIndex* idx, uint32_t** out_ids)
{
    uint32_t* ids = (uint32_t*)malloc(sizeof(uint32_t) * idx->count);
    if (!ids && idx->count) return EZDB_ERR_MEMORY;
    if (fseek(db->fp, (long)(db->header.postings_offset + idx->offset), SEEK_SET) != 0) {
        free(ids);
        return EZDB_ERR_IO;
    }
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
    *out_ids = ids;
    return EZDB_OK;
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

    QueryIndex* query_indexes = (QueryIndex*)malloc(sizeof(QueryIndex) * key_count);
    if (!query_indexes) {
        free(keys);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < key_count; ++i) {
        EzdbDiskIndex* idx = find_index(db, keys[i]);
        if (!idx) {
            free(query_indexes);
            free(keys);
            return EZDB_OK;
        }
        query_indexes[i].idx = idx;
    }
    free(keys);
    qsort(query_indexes, key_count, sizeof(QueryIndex), query_index_compare);

    uint32_t* current = NULL;
    uint32_t current_count = 0;
    for (uint32_t i = 0; i < key_count && rc == EZDB_OK; ++i) {
        uint32_t* ids = NULL;
        rc = load_postings(db, query_indexes[i].idx, &ids);
        if (rc != EZDB_OK) break;
        if (!current) {
            current = ids;
            current_count = query_indexes[i].idx->count;
        } else {
            uint32_t max_next = current_count < query_indexes[i].idx->count ? current_count : query_indexes[i].idx->count;
            uint32_t* next = (uint32_t*)malloc(sizeof(uint32_t) * max_next);
            if (!next && max_next) {
                free(ids);
                rc = EZDB_ERR_MEMORY;
                break;
            }
            uint32_t a = 0, b = 0, n = 0;
            while (a < current_count && b < query_indexes[i].idx->count) {
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
    free(query_indexes);
    if (rc != EZDB_OK) {
        free(current);
        return rc;
    }

    uint32_t emitted = 0;
    for (uint32_t i = 0; i < current_count; ++i) {
        if (limit && emitted >= limit) break;
        uint32_t id = current[i];
        if (!record_contains_keyword(db, id, keyword, key_len)) continue;
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
    free(current);
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
    case EZDB_ERR_READ_ONLY: return "mutation is not implemented in this v2 build";
    default: return "unknown error";
    }
}
