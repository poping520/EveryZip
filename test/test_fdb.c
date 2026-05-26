/*
 * FDB2 prototype format notes
 * ===========================
 *
 * Goal
 * ----
 * FDB2 is an experimental Windows-only path database format. It is designed for
 * a very specific workload: import many absolute file paths, keep the index file
 * smaller than the source text list, load it quickly, search interactively with
 * low latency, and keep process memory much lower than a fully materialized
 * inverted index.
 *
 * The current prototype deliberately favors compact sequential blocks over many
 * small records. A loaded database owns only a few large arrays, which keeps
 * allocation overhead and pointer chasing low:
 *
 *   1. Fdb2Header
 *      Fixed-size metadata with counts and file offsets for every block.
 *
 *   2. String Data Block
 *      All unique path segments are stored once as NUL-terminated UTF-8 strings.
 *      There is no on-disk string offset table in FDB2 v1. During load, the code
 *      reads the whole string block and walks NUL terminators linearly to rebuild
 *      FdbString.text pointers. This saves about string_count * 4 bytes on disk
 *      compared with a uint32 offset table and does not add permanent memory.
 *
 *   3. Path Segment Counts Block
 *      One uint8_t per path. Windows paths normally have very few segments, so a
 *      byte is enough for this prototype and saves three bytes per path compared
 *      with uint32_t. The writer asserts segment_count <= 255.
 *
 *   4. Path Segment Id Stream
 *      Path contents are stored as a flat stream of string_id values encoded with
 *      unsigned varints. Each path points into one contiguous uint32_t array after
 *      load. This keeps the file compact while still giving O(1) in-memory access
 *      to every segment id after loading.
 *
 *   5. Deleted Bitmap
 *      Logical deletion is stored as one bit per path instead of one byte. This
 *      saves roughly path_count * 7 / 8 bytes on disk. The loaded representation
 *      still expands to FdbPath.deleted for simple demo code.
 *
 * Search strategy
 * ---------------
 * FDB2 v1 intentionally does not persist or load a large inverted index. For hot
 * search, it scans the de-duplicated string pool once, marks matching string ids
 * in a temporary bitset, then scans the path table and returns paths containing
 * at least one matching segment. This keeps steady-state memory low and avoids
 * the previous high-memory index. Because many paths share the same directory and
 * filename segments, scanning unique segments is much cheaper than scanning the
 * original text file line by line.
 *
 * Matching is ASCII case-insensitive in this prototype and supports substring
 * matches inside each path segment, so a query such as "device" matches
 * "deviceinfo.txt" and "device_notes.md". UTF-8 bytes are preserved exactly in
 * storage and path reconstruction; full Unicode case folding is left for a later
 * production layer.
 *
 * Load strategy
 * -------------
 * Loading is mostly sequential:
 *
 *   - read Fdb2Header
 *   - read String Data Block and rebuild string pointers by walking NUL bytes
 *   - read uint8_t path segment counts
 *   - decode the varint segment-id stream into one contiguous uint32_t array
 *   - read deleted bits and expand them into FdbPath.deleted
 *
 * There is no per-string or per-path malloc during FDB2 load. The important live
 * memory is the string table, string text block, path table, and flat path id
 * block. This is why FDB2 can load the current all_files data set at roughly
 * 65 MB private memory instead of hundreds of MB.
 *
 * Current measured sample
 * -----------------------
 * On scripts/all_files.txt from the test machine:
 *
 *   - source txt:  91,734,009 bytes
 *   - FDB1:        39,702,601 bytes
 *   - FDB2:        38,989,367 bytes
 *   - data set:    814,600 paths, 421,681 unique strings
 *   - open2:       about 2.3s to 2.8s load, about 65 MB private memory
 *   - search:      typical hot search is around 160ms to 190ms for tested terms
 *
 * Tradeoffs and future work
 * -------------------------
 * FDB2 v1 is still a test demo rather than a final production format. It has no
 * crash-safe writer, no concurrent updates, no mmap reader, no compressed string
 * block, and no Unicode-aware tokenizer. Possible future improvements include
 * frequency-ordered string ids for smaller varints, mmap-backed lazy loading,
 * optional compressed blocks, and a compact secondary index for very frequent
 * interactive queries.
 *
 * FDB2 原型格式说明（中文）
 * ========================
 *
 * 目标
 * ----
 * FDB2 是一个 Windows 专用的实验性路径数据库格式，面向“导入大量绝对文件
 * 路径，然后快速交互搜索”的场景。它的核心目标是：索引文件要比原始 txt 更小，
 * 加载要快，搜索延迟要低，进程内存不能因为完整倒排索引而膨胀到不可接受。
 *
 * 当前原型选择“少量连续大块”而不是“大量小记录”。数据库加载后只持有几个
 * 大数组，从而减少 malloc 次数、结构体碎片和随机指针跳转：
 *
 *   1. Fdb2Header
 *      固定大小的头部，记录每个数据块的数量信息和文件偏移。
 *
 *   2. String Data Block
 *      所有去重后的路径段只保存一次，按 NUL 结尾的 UTF-8 字符串连续存放。
 *      FDB2 v1 不在磁盘保存 string offset 表。加载时一次读入整个字符串块，
 *      然后线性扫描 NUL 字节来重建 FdbString.text 指针。相比 uint32 offset
 *      表，这能节省约 string_count * 4 字节的硬盘空间，并且不增加常驻内存。
 *
 *   3. Path Segment Counts Block
 *      每条路径只用一个 uint8_t 保存段数量。Windows 路径通常段数很少，因此
 *      demo 中一个字节足够；相比 uint32_t，每条路径可节省 3 字节。写入时会
 *      断言 segment_count <= 255。
 *
 *   4. Path Segment Id Stream
 *      每条路径的内容不直接保存字符串，而是保存 string_id。所有 string_id 被
 *      展平成一条连续流，并使用无符号 varint 编码写入磁盘。加载后解码到一个
 *      连续 uint32_t 数组，每条路径只保存指向该数组内部的指针，因此既压缩了
 *      文件体积，又保留了加载后的 O(1) 段访问能力。
 *
 *   5. Deleted Bitmap
 *      逻辑删除标记在磁盘中按 bit 保存，一条路径只占 1 bit，而不是 1 byte。
 *      这大约能节省 path_count * 7 / 8 字节。加载后为了 demo 代码简单，仍然
 *      展开成 FdbPath.deleted 字段。
 *
 * 搜索策略
 * --------
 * FDB2 v1 故意不持久化、也不加载庞大的倒排索引。热搜索时，它先扫描去重后的
 * 字符串池，把命中关键词的 string_id 标记到临时 bitset 中；随后扫描路径表，
 * 只要某条路径包含任意已命中的 string_id，就把这条路径作为搜索结果返回。
 *
 * 这样做的好处是常驻内存低，不需要为所有 token 保存巨大的 path_id 列表。
 * 因为大量路径共享目录名、文件名片段，扫描“去重后的路径段”通常远比逐行扫描
 * 原始 txt 文件便宜。
 *
 * 当前匹配规则是 ASCII 大小写不敏感，并支持路径段内部的子串匹配。因此搜索
 * "device" 可以命中 "deviceinfo.txt" 和 "device_notes.md"。UTF-8 字节在存储
 * 和路径还原时保持原样；完整 Unicode 大小写折叠和分词留给后续生产层处理。
 *
 * 加载策略
 * --------
 * FDB2 加载过程基本是顺序读：
 *
 *   - 读取 Fdb2Header
 *   - 读取 String Data Block，并通过扫描 NUL 字节重建字符串指针
 *   - 读取 uint8_t 路径段数量表
 *   - 解码 varint segment-id 流到一个连续 uint32_t 数组
 *   - 读取 deleted bitmap，并展开到 FdbPath.deleted
 *
 * FDB2 加载期间没有按字符串、按路径逐个 malloc。主要常驻内存只有字符串表、
 * 字符串文本块、路径表和平铺的路径段 id 数组。这也是它能在当前 all_files
 * 数据集上把 private memory 控制在约 65 MB 的原因。
 *
 * 当前实测样本
 * ------------
 * 基于测试机器上的 scripts/all_files.txt：
 *
 *   - source txt:  91,734,009 bytes
 *   - FDB1:        39,702,601 bytes
 *   - FDB2:        38,989,367 bytes
 *   - 数据集:      814,600 条路径，421,681 个唯一字符串
 *   - open2:       加载约 2.3s 到 2.8s，private memory 约 65 MB
 *   - search:      已测试关键词的典型热搜索约 160ms 到 190ms
 *
 * 取舍与后续方向
 * --------------
 * FDB2 v1 仍然是测试 demo，不是最终生产格式。当前还没有崩溃安全写入、并发
 * 更新、mmap 读取器、压缩字符串块，也没有 Unicode 感知 tokenizer。后续可以
 * 继续尝试：按出现频率重新排列 string_id 以缩短 varint、基于 mmap 的懒加载、
 * 可选压缩块，以及面向高频交互查询的紧凑二级索引。
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <psapi.h>

#define FDB_MAGIC 0x31424446u /* FDB1 */
#define FDB_VERSION 1u
#define FDB2_MAGIC 0x32424446u /* FDB2 */
#define FDB2_VERSION 1u
#define FDB_INVALID_ID UINT32_MAX
#define FDB_STRING_BUCKETS 262144u
#define FDB_INDEX_BUCKETS 524288u

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { printf("  [RUN ] %s\n", #name); name(); printf("  [PASS] %s\n", #name); } while (0)
#define ASSERT_TRUE(expr) do { if (!(expr)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); assert(0); } } while (0)
#define ASSERT_EQ_U32(a, b) ASSERT_TRUE((uint32_t)(a) == (uint32_t)(b))
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

typedef struct FdbHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t string_count;
    uint32_t path_count;
    uint32_t index_count;
    uint64_t strings_offset;
    uint64_t paths_offset;
    uint64_t index_offset;
} FdbHeader;

typedef struct Fdb2Header {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t string_count;
    uint32_t path_count;
    uint32_t segment_ref_count;
    uint64_t string_data_size;
    uint64_t string_offsets_offset;
    uint64_t string_data_offset;
    uint64_t path_segment_counts_offset;
    uint64_t path_segment_ids_offset;
    uint64_t deleted_flags_offset;
} Fdb2Header;

typedef struct FdbString {
    char* text;
    uint32_t next;
} FdbString;

typedef struct FdbPath {
    uint32_t* string_ids;
    uint32_t segment_count;
    uint8_t deleted;
} FdbPath;

typedef struct FdbIndexEntry {
    char* token;
    uint32_t* path_ids;
    uint32_t path_count;
    uint32_t path_capacity;
    uint8_t* path_id_bytes;
    uint32_t path_id_byte_count;
    uint32_t next;
} FdbIndexEntry;

typedef struct Fdb {
    FdbString* strings;
    uint32_t string_count;
    uint32_t string_capacity;
    FdbPath* paths;
    uint32_t path_count;
    uint32_t path_capacity;
    FdbIndexEntry* index;
    uint32_t index_count;
    uint32_t index_capacity;
    uint32_t* string_buckets;
    uint32_t* index_buckets;
    char* string_text_block;
    uint32_t* path_string_id_block;
    uint32_t* index_path_id_block;
} Fdb;

typedef struct FdbIdList {
    uint32_t* ids;
    uint32_t count;
} FdbIdList;

typedef struct FdbDecodedIndexEntry {
    const FdbIndexEntry* entry;
    uint32_t* ids;
} FdbDecodedIndexEntry;

static void fdb_compact_index_storage(Fdb* db);
static void fdb_compress_index_storage(Fdb* db);
static uint32_t fdb_varuint_size(uint32_t value);
static void fdb_write_varuint_to_mem(uint8_t** out, uint32_t value);
static uint32_t fdb_read_varuint_from_mem(const uint8_t** in);

static char* fdb_strdup_range(const char* text, size_t len)
{
    char* out = (char*)malloc(len + 1);
    ASSERT_TRUE(out != NULL);
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static uint32_t fdb_hash_string(const char* text)
{
    uint32_t hash = 5381u;
    while (*text) {
        hash = ((hash << 5) + hash) ^ (unsigned char)*text++;
    }
    return hash;
}

static void fdb_init_buckets(uint32_t** buckets, uint32_t count)
{
    if (*buckets) return;
    *buckets = (uint32_t*)malloc(sizeof(uint32_t) * count);
    ASSERT_TRUE(*buckets != NULL);
    for (uint32_t i = 0; i < count; ++i) (*buckets)[i] = FDB_INVALID_ID;
}

static char fdb_ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static char* fdb_to_lower_copy(const char* text)
{
    const size_t len = strlen(text);
    char* out = fdb_strdup_range(text, len);
    for (size_t i = 0; i < len; ++i) out[i] = fdb_ascii_lower(out[i]);
    return out;
}

static int fdb_contains_ascii_case_insensitive(const char* text, const char* lower_keyword, size_t keyword_len)
{
    if (keyword_len == 0) return 1;
    for (const char* p = text; *p; ++p) {
        size_t i = 0;
        while (i < keyword_len && p[i] &&
               fdb_ascii_lower(p[i]) == lower_keyword[i]) {
            i++;
        }
        if (i == keyword_len) return 1;
    }
    return 0;
}

static void* fdb_realloc_array(void* ptr, size_t count, size_t item_size)
{
    void* out = realloc(ptr, count * item_size);
    ASSERT_TRUE(out != NULL);
    return out;
}

static void fdb_init(Fdb* db)
{
    memset(db, 0, sizeof(*db));
}

static Fdb* fdb_open(void)
{
    Fdb* db = (Fdb*)calloc(1, sizeof(Fdb));
    ASSERT_TRUE(db != NULL);
    return db;
}

static void fdb_clear(Fdb* db)
{
    if (!db) return;

    for (uint32_t i = 0; i < db->string_count; ++i) {
        if (!db->string_text_block) free(db->strings[i].text);
    }
    free(db->strings);
    free(db->string_buckets);
    free(db->string_text_block);

    for (uint32_t i = 0; i < db->path_count; ++i) {
        if (!db->path_string_id_block) free(db->paths[i].string_ids);
    }
    free(db->paths);
    free(db->path_string_id_block);

    for (uint32_t i = 0; i < db->index_count; ++i) {
        free(db->index[i].token);
        if (!db->index_path_id_block) free(db->index[i].path_ids);
        free(db->index[i].path_id_bytes);
    }
    free(db->index);
    free(db->index_buckets);
    free(db->index_path_id_block);

    fdb_init(db);
}

static void fdb_close(Fdb* db)
{
    if (!db) return;
    fdb_clear(db);
    free(db);
}

static uint32_t fdb_get_or_add_string(Fdb* db, const char* text)
{
    fdb_init_buckets(&db->string_buckets, FDB_STRING_BUCKETS);
    const uint32_t bucket = fdb_hash_string(text) % FDB_STRING_BUCKETS;
    for (uint32_t i = db->string_buckets[bucket]; i != FDB_INVALID_ID; i = db->strings[i].next) {
        if (strcmp(db->strings[i].text, text) == 0) return i;
    }

    if (db->string_count == db->string_capacity) {
        db->string_capacity = db->string_capacity ? db->string_capacity * 2 : 16;
        db->strings = (FdbString*)fdb_realloc_array(db->strings, db->string_capacity, sizeof(FdbString));
    }

    const uint32_t id = db->string_count++;
    db->strings[id].text = fdb_strdup_range(text, strlen(text));
    db->strings[id].next = db->string_buckets[bucket];
    db->string_buckets[bucket] = id;
    return id;
}

static FdbIndexEntry* fdb_find_index(Fdb* db, const char* token)
{
    fdb_init_buckets(&db->index_buckets, FDB_INDEX_BUCKETS);
    const uint32_t bucket = fdb_hash_string(token) % FDB_INDEX_BUCKETS;
    for (uint32_t i = db->index_buckets[bucket]; i != FDB_INVALID_ID; i = db->index[i].next) {
        if (strcmp(db->index[i].token, token) == 0) return &db->index[i];
    }
    return NULL;
}

static FdbIndexEntry* fdb_get_or_add_index(Fdb* db, const char* token)
{
    FdbIndexEntry* existing = fdb_find_index(db, token);
    if (existing) return existing;

    if (db->index_count == db->index_capacity) {
        db->index_capacity = db->index_capacity ? db->index_capacity * 2 : 32;
        db->index = (FdbIndexEntry*)fdb_realloc_array(db->index, db->index_capacity, sizeof(FdbIndexEntry));
    }

    FdbIndexEntry* entry = &db->index[db->index_count++];
    entry->token = fdb_strdup_range(token, strlen(token));
    entry->path_ids = NULL;
    entry->path_count = 0;
    entry->path_capacity = 0;
    entry->path_id_bytes = NULL;
    entry->path_id_byte_count = 0;
    const uint32_t bucket = fdb_hash_string(token) % FDB_INDEX_BUCKETS;
    entry->next = db->index_buckets[bucket];
    db->index_buckets[bucket] = (uint32_t)(entry - db->index);
    return entry;
}

static void fdb_index_add_path(FdbIndexEntry* entry, uint32_t path_id)
{
    if (entry->path_count > 0 && entry->path_ids[entry->path_count - 1] == path_id) {
        return;
    }

    if (entry->path_count == entry->path_capacity) {
        entry->path_capacity = entry->path_capacity ? entry->path_capacity * 2 : 4;
        entry->path_ids = (uint32_t*)fdb_realloc_array(entry->path_ids, entry->path_capacity, sizeof(uint32_t));
    }
    entry->path_ids[entry->path_count++] = path_id;
}

static void fdb_add_token(Fdb* db, uint32_t path_id, const char* lower_text, size_t start, size_t len)
{
    if (len == 0) return;
    char small_token[4];
    char* heap_token = NULL;
    char* token = small_token;
    if (len < sizeof(small_token)) {
        memcpy(small_token, lower_text + start, len);
        small_token[len] = '\0';
    } else {
        heap_token = fdb_strdup_range(lower_text + start, len);
        token = heap_token;
    }
    FdbIndexEntry* entry = fdb_get_or_add_index(db, token);
    fdb_index_add_path(entry, path_id);
    free(heap_token);
}

static void fdb_index_segment(Fdb* db, uint32_t path_id, const char* segment)
{
    char* lower = fdb_to_lower_copy(segment);
    const size_t len = strlen(lower);

    fdb_add_token(db, path_id, lower, 0, len);

    if (len <= 3) {
        for (size_t start = 0; start < len; ++start) {
            for (size_t end = start + 1; end <= len; ++end) {
                fdb_add_token(db, path_id, lower, start, end - start);
            }
        }
    } else {
        for (size_t start = 0; start + 3 <= len; ++start) {
            fdb_add_token(db, path_id, lower, start, 3);
        }
    }

    free(lower);
}

static void fdb_index_path(Fdb* db, uint32_t path_id)
{
    FdbPath* path = &db->paths[path_id];
    for (uint32_t i = 0; i < path->segment_count; ++i) {
        fdb_index_segment(db, path_id, db->strings[path->string_ids[i]].text);
    }
}

static uint32_t fdb_import_path(Fdb* db, const char* path_text)
{
    uint32_t* ids = NULL;
    uint32_t count = 0;
    const char* segment_start = path_text;

    for (const char* p = path_text;; ++p) {
        if (*p == '\\' || *p == '/' || *p == '\0') {
            const size_t len = (size_t)(p - segment_start);
            if (len > 0) {
                char* segment = fdb_strdup_range(segment_start, len);
                const uint32_t string_id = fdb_get_or_add_string(db, segment);
                ids = (uint32_t*)fdb_realloc_array(ids, count + 1, sizeof(uint32_t));
                ids[count++] = string_id;
                free(segment);
            }
            if (*p == '\0') break;
            segment_start = p + 1;
        }
    }

    if (count == 0) {
        free(ids);
        return FDB_INVALID_ID;
    }

    if (db->path_count == db->path_capacity) {
        db->path_capacity = db->path_capacity ? db->path_capacity * 2 : 16;
        db->paths = (FdbPath*)fdb_realloc_array(db->paths, db->path_capacity, sizeof(FdbPath));
    }

    const uint32_t path_id = db->path_count++;
    db->paths[path_id].string_ids = ids;
    db->paths[path_id].segment_count = count;
    db->paths[path_id].deleted = 0;
    fdb_index_path(db, path_id);
    return path_id;
}

static char* fdb_trim_line(char* line)
{
    unsigned char* bytes = (unsigned char*)line;
    if (bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        line += 3;
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    return line;
}

static int fdb_import_path_list_file(Fdb* db, const char* txt_path, uint32_t* out_imported)
{
    FILE* file = fopen(txt_path, "rb");
    if (!file) return 0;

    char buffer[32768];
    uint32_t imported = 0;
    while (fgets(buffer, (int)sizeof(buffer), file)) {
        char* line = fdb_trim_line(buffer);
        if (line[0] == '\0') continue;
        if (fdb_import_path(db, line) != FDB_INVALID_ID) imported++;
        if (imported > 0 && imported % 100000u == 0) {
            printf("Imported %u paths...\n", imported);
        }
    }

    fclose(file);
    if (out_imported) *out_imported = imported;
    return 1;
}

static char* fdb_build_path(const Fdb* db, uint32_t path_id)
{
    ASSERT_TRUE(path_id < db->path_count);
    const FdbPath* path = &db->paths[path_id];
    size_t total = 1;

    for (uint32_t i = 0; i < path->segment_count; ++i) {
        total += strlen(db->strings[path->string_ids[i]].text);
        if (i + 1 < path->segment_count) total += 1;
    }

    char* out = (char*)malloc(total);
    ASSERT_TRUE(out != NULL);
    out[0] = '\0';

    for (uint32_t i = 0; i < path->segment_count; ++i) {
        if (i > 0) strcat(out, "\\");
        strcat(out, db->strings[path->string_ids[i]].text);
    }

    return out;
}

static uint32_t fdb_list_paths(const Fdb* db, char*** out_paths)
{
    uint32_t count = 0;
    char** paths = NULL;

    for (uint32_t i = 0; i < db->path_count; ++i) {
        if (db->paths[i].deleted) continue;
        paths = (char**)fdb_realloc_array(paths, count + 1, sizeof(char*));
        paths[count++] = fdb_build_path(db, i);
    }

    *out_paths = paths;
    return count;
}

static const FdbIndexEntry* fdb_find_index_const(const Fdb* db, const char* token)
{
    if (!db->index_buckets) return NULL;
    const uint32_t bucket = fdb_hash_string(token) % FDB_INDEX_BUCKETS;
    for (uint32_t i = db->index_buckets[bucket]; i != FDB_INVALID_ID; i = db->index[i].next) {
        if (strcmp(db->index[i].token, token) == 0) return &db->index[i];
    }
    return NULL;
}

static int fdb_id_list_contains(const FdbIdList* list, uint32_t id)
{
    for (uint32_t i = 0; i < list->count; ++i) {
        if (list->ids[i] == id) return 1;
    }
    return 0;
}

static void fdb_id_list_add(FdbIdList* list, uint32_t id)
{
    if (fdb_id_list_contains(list, id)) return;
    list->ids = (uint32_t*)fdb_realloc_array(list->ids, list->count + 1, sizeof(uint32_t));
    list->ids[list->count++] = id;
}

static int fdb_index_entry_contains_path(const FdbIndexEntry* entry, uint32_t path_id)
{
    if (entry->path_ids == NULL && entry->path_id_bytes != NULL) {
        const uint8_t* in = entry->path_id_bytes;
        uint32_t prev = 0;
        for (uint32_t i = 0; i < entry->path_count; ++i) {
            const uint32_t delta = fdb_read_varuint_from_mem(&in);
            const uint32_t value = i == 0 ? delta : prev + delta;
            if (value == path_id) return 1;
            if (value > path_id) return 0;
            prev = value;
        }
        return 0;
    }

    uint32_t left = 0;
    uint32_t right = entry->path_count;
    while (left < right) {
        const uint32_t mid = left + (right - left) / 2;
        const uint32_t value = entry->path_ids[mid];
        if (value == path_id) return 1;
        if (value < path_id) left = mid + 1;
        else right = mid;
    }
    return 0;
}

static uint32_t* fdb_decode_index_entry_paths(const FdbIndexEntry* entry)
{
    uint32_t* ids = (uint32_t*)malloc(sizeof(uint32_t) * entry->path_count);
    ASSERT_TRUE(ids != NULL);

    if (entry->path_ids) {
        memcpy(ids, entry->path_ids, sizeof(uint32_t) * entry->path_count);
        return ids;
    }

    const uint8_t* in = entry->path_id_bytes;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < entry->path_count; ++i) {
        const uint32_t delta = fdb_read_varuint_from_mem(&in);
        const uint32_t id = i == 0 ? delta : prev + delta;
        ids[i] = id;
        prev = id;
    }
    return ids;
}

static int fdb_decoded_entry_contains_path(const FdbDecodedIndexEntry* decoded, uint32_t path_id)
{
    uint32_t left = 0;
    uint32_t right = decoded->entry->path_count;
    while (left < right) {
        const uint32_t mid = left + (right - left) / 2;
        const uint32_t value = decoded->ids[mid];
        if (value == path_id) return 1;
        if (value < path_id) left = mid + 1;
        else right = mid;
    }
    return 0;
}

static int fdb_path_contains_lower_keyword(const Fdb* db, uint32_t path_id, const char* lower_keyword)
{
    char* path = fdb_build_path(db, path_id);
    char* lower_path = fdb_to_lower_copy(path);
    const int found = strstr(lower_path, lower_keyword) != NULL;
    free(lower_path);
    free(path);
    return found;
}

static uint32_t fdb_search(const Fdb* db, const char* keyword, char*** out_paths)
{
    char* lower = fdb_to_lower_copy(keyword);
    FdbIdList candidates = {0};
    uint32_t count = 0;
    char** paths = NULL;
    const size_t len = strlen(lower);

    if (db->index_count == 0) {
        const uint32_t bitset_bytes = (db->string_count + 7u) / 8u;
        uint8_t* matched_strings = (uint8_t*)calloc(bitset_bytes ? bitset_bytes : 1, 1);
        ASSERT_TRUE(matched_strings != NULL);

        for (uint32_t i = 0; i < db->string_count; ++i) {
            if (fdb_contains_ascii_case_insensitive(db->strings[i].text, lower, len)) {
                matched_strings[i / 8u] |= (uint8_t)(1u << (i % 8u));
            }
        }

        for (uint32_t i = 0; i < db->path_count; ++i) {
            int matched = 0;
            if (db->paths[i].deleted) continue;
            for (uint32_t j = 0; j < db->paths[i].segment_count; ++j) {
                const uint32_t string_id = db->paths[i].string_ids[j];
                if (matched_strings[string_id / 8u] & (uint8_t)(1u << (string_id % 8u))) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) continue;
            paths = (char**)fdb_realloc_array(paths, count + 1, sizeof(char*));
            paths[count++] = fdb_build_path(db, i);
        }
        free(matched_strings);
        free(lower);
        *out_paths = paths;
        return count;
    }

    if (len <= 3) {
        const FdbIndexEntry* entry = fdb_find_index_const(db, lower);
        if (entry) {
            uint32_t* ids = fdb_decode_index_entry_paths(entry);
            for (uint32_t i = 0; i < entry->path_count; ++i) {
                fdb_id_list_add(&candidates, ids[i]);
            }
            free(ids);
        }
    } else {
        FdbDecodedIndexEntry entries[256];
        uint32_t entry_count = 0;
        ASSERT_TRUE(len - 2 < 256);

        for (size_t start = 0; start + 3 <= len; ++start) {
            char* token = fdb_strdup_range(lower + start, 3);
            const FdbIndexEntry* entry = fdb_find_index_const(db, token);
            free(token);
            if (!entry) {
                free(candidates.ids);
                free(lower);
                *out_paths = NULL;
                return 0;
            }

            int duplicate = 0;
            for (uint32_t i = 0; i < entry_count; ++i) {
                if (entries[i].entry == entry) {
                    duplicate = 1;
                    break;
                }
            }
            if (!duplicate) {
                entries[entry_count].entry = entry;
                entries[entry_count].ids = fdb_decode_index_entry_paths(entry);
                entry_count++;
            }
        }

        uint32_t base = 0;
        for (uint32_t i = 1; i < entry_count; ++i) {
            if (entries[i].entry->path_count < entries[base].entry->path_count) base = i;
        }

        for (uint32_t i = 0; i < entries[base].entry->path_count; ++i) {
            const uint32_t path_id = entries[base].ids[i];
            int ok = 1;
            for (uint32_t j = 0; j < entry_count; ++j) {
                if (j == base) continue;
                if (!fdb_decoded_entry_contains_path(&entries[j], path_id)) {
                    ok = 0;
                    break;
                }
            }
            if (ok) fdb_id_list_add(&candidates, path_id);
        }

        for (uint32_t i = 0; i < entry_count; ++i) free(entries[i].ids);
    }

    for (uint32_t i = 0; i < candidates.count; ++i) {
        const uint32_t path_id = candidates.ids[i];
        if (path_id >= db->path_count || db->paths[path_id].deleted) continue;
        if (!fdb_path_contains_lower_keyword(db, path_id, lower)) continue;
        paths = (char**)fdb_realloc_array(paths, count + 1, sizeof(char*));
        paths[count++] = fdb_build_path(db, path_id);
    }

    free(candidates.ids);
    free(lower);
    *out_paths = paths;
    return count;
}

static void fdb_build_memory_index(Fdb* db)
{
    for (uint32_t i = 0; i < db->index_count; ++i) {
        free(db->index[i].token);
        if (!db->index_path_id_block) free(db->index[i].path_ids);
    }
    free(db->index);
    free(db->index_buckets);
    free(db->index_path_id_block);
    db->index = NULL;
    db->index_count = 0;
    db->index_capacity = 0;
    db->index_buckets = NULL;
    db->index_path_id_block = NULL;

    for (uint32_t i = 0; i < db->path_count; ++i) {
        if (!db->paths[i].deleted) fdb_index_path(db, i);
    }
    fdb_compact_index_storage(db);
}

static double elapsed_ms(clock_t start, clock_t end)
{
    return ((double)(end - start) * 1000.0) / (double)CLOCKS_PER_SEC;
}

static void print_process_memory_usage(void)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
        printf("Memory: working set %.2f MB, private %.2f MB\n",
               (double)counters.WorkingSetSize / (1024.0 * 1024.0),
               (double)counters.PrivateUsage / (1024.0 * 1024.0));
    }
}

static int fdb_delete_path(Fdb* db, const char* path_text)
{
    for (uint32_t i = 0; i < db->path_count; ++i) {
        char* built = fdb_build_path(db, i);
        const int match = strcmp(built, path_text) == 0;
        free(built);
        if (match) {
            db->paths[i].deleted = 1;
            return 1;
        }
    }
    return 0;
}

static void fdb_compact_path_storage(Fdb* db)
{
    if (db->path_string_id_block) return;

    uint32_t total = 0;
    for (uint32_t i = 0; i < db->path_count; ++i) total += db->paths[i].segment_count;
    if (total == 0) return;

    uint32_t* block = (uint32_t*)malloc(sizeof(uint32_t) * total);
    ASSERT_TRUE(block != NULL);

    uint32_t offset = 0;
    for (uint32_t i = 0; i < db->path_count; ++i) {
        FdbPath* path = &db->paths[i];
        memcpy(block + offset, path->string_ids, sizeof(uint32_t) * path->segment_count);
        free(path->string_ids);
        path->string_ids = block + offset;
        offset += path->segment_count;
    }

    db->path_string_id_block = block;
}

static void fdb_compact_string_storage(Fdb* db)
{
    if (db->string_text_block) return;

    size_t total = 0;
    for (uint32_t i = 0; i < db->string_count; ++i) total += strlen(db->strings[i].text) + 1;
    if (total == 0) return;

    char* block = (char*)malloc(total);
    ASSERT_TRUE(block != NULL);

    size_t offset = 0;
    for (uint32_t i = 0; i < db->string_count; ++i) {
        const size_t len = strlen(db->strings[i].text) + 1;
        memcpy(block + offset, db->strings[i].text, len);
        free(db->strings[i].text);
        db->strings[i].text = block + offset;
        offset += len;
    }

    db->string_text_block = block;
}

static void fdb_compact_index_storage(Fdb* db)
{
    if (db->index_path_id_block) return;

    uint32_t total = 0;
    for (uint32_t i = 0; i < db->index_count; ++i) total += db->index[i].path_count;
    if (total == 0) return;

    uint32_t* block = (uint32_t*)malloc(sizeof(uint32_t) * total);
    ASSERT_TRUE(block != NULL);

    uint32_t offset = 0;
    for (uint32_t i = 0; i < db->index_count; ++i) {
        FdbIndexEntry* entry = &db->index[i];
        memcpy(block + offset, entry->path_ids, sizeof(uint32_t) * entry->path_count);
        free(entry->path_ids);
        entry->path_ids = block + offset;
        entry->path_capacity = entry->path_count;
        offset += entry->path_count;
    }

    db->index_path_id_block = block;
}

static void fdb_compress_index_storage(Fdb* db)
{
    for (uint32_t i = 0; i < db->index_count; ++i) {
        FdbIndexEntry* entry = &db->index[i];
        uint32_t bytes = 0;
        uint32_t prev = 0;
        for (uint32_t j = 0; j < entry->path_count; ++j) {
            const uint32_t id = entry->path_ids[j];
            bytes += fdb_varuint_size(j == 0 ? id : id - prev);
            prev = id;
        }

        entry->path_id_bytes = (uint8_t*)malloc(bytes ? bytes : 1);
        ASSERT_TRUE(entry->path_id_bytes != NULL);
        entry->path_id_byte_count = bytes;

        uint8_t* out = entry->path_id_bytes;
        prev = 0;
        for (uint32_t j = 0; j < entry->path_count; ++j) {
            const uint32_t id = entry->path_ids[j];
            fdb_write_varuint_to_mem(&out, j == 0 ? id : id - prev);
            prev = id;
        }
    }

    if (db->index_path_id_block) {
        free(db->index_path_id_block);
        db->index_path_id_block = NULL;
        for (uint32_t i = 0; i < db->index_count; ++i) {
            db->index[i].path_ids = NULL;
            db->index[i].path_capacity = 0;
        }
    } else {
        for (uint32_t i = 0; i < db->index_count; ++i) {
            free(db->index[i].path_ids);
            db->index[i].path_ids = NULL;
            db->index[i].path_capacity = 0;
        }
    }
}

static void fdb_free_path_list(char** paths, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) free(paths[i]);
    free(paths);
}

static void fdb_write_exact(FILE* file, const void* data, size_t size)
{
    ASSERT_TRUE(fwrite(data, 1, size, file) == size);
}

static void fdb_read_exact(FILE* file, void* data, size_t size)
{
    ASSERT_TRUE(fread(data, 1, size, file) == size);
}

static void fdb_write_varuint(FILE* file, uint32_t value)
{
    while (value >= 0x80u) {
        const uint8_t byte = (uint8_t)((value & 0x7Fu) | 0x80u);
        fdb_write_exact(file, &byte, sizeof(byte));
        value >>= 7;
    }
    {
        const uint8_t byte = (uint8_t)value;
        fdb_write_exact(file, &byte, sizeof(byte));
    }
}

static uint32_t fdb_read_varuint(FILE* file)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    for (;;) {
        uint8_t byte = 0;
        fdb_read_exact(file, &byte, sizeof(byte));
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
        ASSERT_TRUE(shift < 32);
    }
    return value;
}

static uint32_t fdb_varuint_size(uint32_t value)
{
    uint32_t size = 1;
    while (value >= 0x80u) {
        value >>= 7;
        size++;
    }
    return size;
}

static void fdb_write_varuint_to_mem(uint8_t** out, uint32_t value)
{
    while (value >= 0x80u) {
        *(*out)++ = (uint8_t)((value & 0x7Fu) | 0x80u);
        value >>= 7;
    }
    *(*out)++ = (uint8_t)value;
}

static uint32_t fdb_read_varuint_from_mem(const uint8_t** in)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    for (;;) {
        const uint8_t byte = *(*in)++;
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
    }
    return value;
}

static int fdb_save(const Fdb* db, const char* file_path)
{
    FILE* file = fopen(file_path, "wb");
    if (!file) return 0;

    FdbHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = FDB_MAGIC;
    header.version = FDB_VERSION;
    header.string_count = db->string_count;
    header.path_count = db->path_count;
    header.index_count = 0;

    fdb_write_exact(file, &header, sizeof(header));

    header.strings_offset = (uint64_t)ftell(file);
    for (uint32_t i = 0; i < db->string_count; ++i) {
        const uint32_t len = (uint32_t)strlen(db->strings[i].text);
        fdb_write_varuint(file, len);
        fdb_write_exact(file, db->strings[i].text, len);
    }

    header.paths_offset = (uint64_t)ftell(file);
    for (uint32_t i = 0; i < db->path_count; ++i) {
        const FdbPath* path = &db->paths[i];
        fdb_write_exact(file, &path->deleted, sizeof(path->deleted));
        fdb_write_varuint(file, path->segment_count);
        for (uint32_t j = 0; j < path->segment_count; ++j) {
            fdb_write_varuint(file, path->string_ids[j]);
        }
    }

    header.index_offset = (uint64_t)ftell(file);

    ASSERT_TRUE(fseek(file, 0, SEEK_SET) == 0);
    fdb_write_exact(file, &header, sizeof(header));
    fclose(file);
    return 1;
}

static int fdb2_save(const Fdb* db, const char* file_path)
{
    FILE* file = fopen(file_path, "wb");
    if (!file) return 0;

    Fdb2Header header;
    memset(&header, 0, sizeof(header));
    header.magic = FDB2_MAGIC;
    header.version = FDB2_VERSION;
    header.string_count = db->string_count;
    header.path_count = db->path_count;

    for (uint32_t i = 0; i < db->path_count; ++i) {
        header.segment_ref_count += db->paths[i].segment_count;
    }
    for (uint32_t i = 0; i < db->string_count; ++i) {
        header.string_data_size += strlen(db->strings[i].text) + 1;
    }

    fdb_write_exact(file, &header, sizeof(header));

    header.string_offsets_offset = 0;

    header.string_data_offset = (uint64_t)ftell(file);
    for (uint32_t i = 0; i < db->string_count; ++i) {
        fdb_write_exact(file, db->strings[i].text, strlen(db->strings[i].text) + 1);
    }

    header.path_segment_counts_offset = (uint64_t)ftell(file);
    for (uint32_t i = 0; i < db->path_count; ++i) {
        ASSERT_TRUE(db->paths[i].segment_count <= 255u);
        {
            const uint8_t segment_count = (uint8_t)db->paths[i].segment_count;
            fdb_write_exact(file, &segment_count, sizeof(segment_count));
        }
    }

    header.path_segment_ids_offset = (uint64_t)ftell(file);
    for (uint32_t i = 0; i < db->path_count; ++i) {
        for (uint32_t j = 0; j < db->paths[i].segment_count; ++j) {
            fdb_write_varuint(file, db->paths[i].string_ids[j]);
        }
    }

    header.deleted_flags_offset = (uint64_t)ftell(file);
    for (uint32_t i = 0; i < db->path_count; i += 8) {
        uint8_t bits = 0;
        for (uint32_t bit = 0; bit < 8 && i + bit < db->path_count; ++bit) {
            if (db->paths[i + bit].deleted) bits |= (uint8_t)(1u << bit);
        }
        fdb_write_exact(file, &bits, sizeof(bits));
    }

    ASSERT_TRUE(fseek(file, 0, SEEK_SET) == 0);
    fdb_write_exact(file, &header, sizeof(header));
    fclose(file);
    return 1;
}

static int fdb_load(Fdb* db, const char* file_path)
{
    FILE* file = fopen(file_path, "rb");
    if (!file) return 0;

    fdb_clear(db);

    FdbHeader header;
    fdb_read_exact(file, &header, sizeof(header));
    ASSERT_EQ_U32(header.magic, FDB_MAGIC);
    ASSERT_EQ_U32(header.version, FDB_VERSION);

    ASSERT_TRUE(fseek(file, (long)header.strings_offset, SEEK_SET) == 0);
    for (uint32_t i = 0; i < header.string_count; ++i) {
        uint32_t len = fdb_read_varuint(file);
        char* text = (char*)malloc((size_t)len + 1);
        ASSERT_TRUE(text != NULL);
        fdb_read_exact(file, text, len);
        text[len] = '\0';

        if (db->string_count == db->string_capacity) {
            db->string_capacity = db->string_capacity ? db->string_capacity * 2 : 16;
            db->strings = (FdbString*)fdb_realloc_array(db->strings, db->string_capacity, sizeof(FdbString));
        }
        const uint32_t id = db->string_count++;
        db->strings[id].text = text;
        db->strings[id].next = FDB_INVALID_ID;
    }

    ASSERT_TRUE(fseek(file, (long)header.paths_offset, SEEK_SET) == 0);
    for (uint32_t i = 0; i < header.path_count; ++i) {
        if (db->path_count == db->path_capacity) {
            db->path_capacity = db->path_capacity ? db->path_capacity * 2 : 16;
            db->paths = (FdbPath*)fdb_realloc_array(db->paths, db->path_capacity, sizeof(FdbPath));
        }

        FdbPath* path = &db->paths[db->path_count++];
        fdb_read_exact(file, &path->deleted, sizeof(path->deleted));
        path->segment_count = fdb_read_varuint(file);
        path->string_ids = (uint32_t*)malloc(sizeof(uint32_t) * path->segment_count);
        ASSERT_TRUE(path->string_ids != NULL);
        for (uint32_t j = 0; j < path->segment_count; ++j) {
            path->string_ids[j] = fdb_read_varuint(file);
        }
    }

    fclose(file);

    fdb_init_buckets(&db->string_buckets, FDB_STRING_BUCKETS);
    for (uint32_t i = 0; i < db->string_count; ++i) {
        const uint32_t bucket = fdb_hash_string(db->strings[i].text) % FDB_STRING_BUCKETS;
        db->strings[i].next = db->string_buckets[bucket];
        db->string_buckets[bucket] = i;
    }

    fdb_compact_string_storage(db);
    fdb_compact_path_storage(db);
    return 1;
}

static int fdb2_load(Fdb* db, const char* file_path)
{
    FILE* file = fopen(file_path, "rb");
    if (!file) return 0;

    fdb_clear(db);

    Fdb2Header header;
    fdb_read_exact(file, &header, sizeof(header));
    ASSERT_EQ_U32(header.magic, FDB2_MAGIC);
    ASSERT_EQ_U32(header.version, FDB2_VERSION);

    db->strings = (FdbString*)calloc(header.string_count ? header.string_count : 1, sizeof(FdbString));
    ASSERT_TRUE(db->strings != NULL);
    db->string_count = header.string_count;
    db->string_capacity = header.string_count;

    db->string_text_block = (char*)malloc((size_t)header.string_data_size);
    ASSERT_TRUE(db->string_text_block != NULL);

    ASSERT_TRUE(fseek(file, (long)header.string_data_offset, SEEK_SET) == 0);
    fdb_read_exact(file, db->string_text_block, (size_t)header.string_data_size);

    char* text_cursor = db->string_text_block;
    char* text_end = db->string_text_block + header.string_data_size;
    for (uint32_t i = 0; i < header.string_count; ++i) {
        ASSERT_TRUE(text_cursor < text_end);
        db->strings[i].text = text_cursor;
        db->strings[i].next = FDB_INVALID_ID;
        text_cursor += strlen(text_cursor) + 1;
    }

    db->paths = (FdbPath*)calloc(header.path_count ? header.path_count : 1, sizeof(FdbPath));
    ASSERT_TRUE(db->paths != NULL);
    db->path_count = header.path_count;
    db->path_capacity = header.path_count;

    ASSERT_TRUE(fseek(file, (long)header.path_segment_counts_offset, SEEK_SET) == 0);
    for (uint32_t i = 0; i < header.path_count; ++i) {
        uint8_t segment_count = 0;
        fdb_read_exact(file, &segment_count, sizeof(segment_count));
        db->paths[i].segment_count = segment_count;
    }

    db->path_string_id_block = (uint32_t*)malloc(sizeof(uint32_t) * header.segment_ref_count);
    ASSERT_TRUE(db->path_string_id_block != NULL);
    ASSERT_TRUE(fseek(file, (long)header.path_segment_ids_offset, SEEK_SET) == 0);
    for (uint32_t i = 0; i < header.segment_ref_count; ++i) {
        db->path_string_id_block[i] = fdb_read_varuint(file);
    }

    uint32_t segment_offset = 0;
    for (uint32_t i = 0; i < header.path_count; ++i) {
        db->paths[i].string_ids = db->path_string_id_block + segment_offset;
        segment_offset += db->paths[i].segment_count;
    }

    ASSERT_TRUE(fseek(file, (long)header.deleted_flags_offset, SEEK_SET) == 0);
    for (uint32_t i = 0; i < header.path_count; i += 8) {
        uint8_t bits = 0;
        fdb_read_exact(file, &bits, sizeof(bits));
        for (uint32_t bit = 0; bit < 8 && i + bit < header.path_count; ++bit) {
            db->paths[i + bit].deleted = (uint8_t)((bits >> bit) & 1u);
        }
    }

    fclose(file);
    return 1;
}

static int path_list_contains(char** paths, uint32_t count, const char* target)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(paths[i], target) == 0) return 1;
    }
    return 0;
}

TEST(TestFdbRoundTripSearchAndDelete)
{
    const char* fdb_path = "test_paths.fdb";
    remove(fdb_path);

    Fdb* db = fdb_open();
    fdb_import_path(db, "E:\\i4Tools7\\Files\\deviceinfo.txt");
    fdb_import_path(db, "E:\\i4Tools7\\Files\\config.json");
    fdb_import_path(db, "D:\\Work\\EveryZip\\README.md");
    fdb_import_path(db, "D:\\Work\\EveryZip\\device_notes.md");

    ASSERT_EQ_U32(db->path_count, 4);
    ASSERT_EQ_U32(db->string_count, 10);
    ASSERT_TRUE(fdb_save(db, fdb_path));
    fdb_close(db);

    db = fdb_open();
    ASSERT_TRUE(fdb_load(db, fdb_path));
    ASSERT_EQ_U32(db->path_count, 4);
    ASSERT_EQ_U32(db->string_count, 10);

    char** paths = NULL;
    uint32_t count = fdb_list_paths(db, &paths);
    ASSERT_EQ_U32(count, 4);
    ASSERT_TRUE(path_list_contains(paths, count, "E:\\i4Tools7\\Files\\deviceinfo.txt"));
    ASSERT_TRUE(path_list_contains(paths, count, "D:\\Work\\EveryZip\\README.md"));
    fdb_free_path_list(paths, count);

    count = fdb_search(db, "device", &paths);
    ASSERT_EQ_U32(count, 2);
    ASSERT_TRUE(path_list_contains(paths, count, "E:\\i4Tools7\\Files\\deviceinfo.txt"));
    ASSERT_TRUE(path_list_contains(paths, count, "D:\\Work\\EveryZip\\device_notes.md"));
    fdb_free_path_list(paths, count);

    count = fdb_search(db, "DEVICE", &paths);
    ASSERT_EQ_U32(count, 2);
    fdb_free_path_list(paths, count);

    ASSERT_TRUE(fdb_delete_path(db, "E:\\i4Tools7\\Files\\deviceinfo.txt"));

    count = fdb_list_paths(db, &paths);
    ASSERT_EQ_U32(count, 3);
    ASSERT_TRUE(!path_list_contains(paths, count, "E:\\i4Tools7\\Files\\deviceinfo.txt"));
    fdb_free_path_list(paths, count);

    count = fdb_search(db, "device", &paths);
    ASSERT_EQ_U32(count, 1);
    ASSERT_STREQ(paths[0], "D:\\Work\\EveryZip\\device_notes.md");
    fdb_free_path_list(paths, count);

    ASSERT_TRUE(fdb_save(db, fdb_path));
    fdb_close(db);

    db = fdb_open();
    ASSERT_TRUE(fdb_load(db, fdb_path));
    count = fdb_search(db, "device", &paths);
    ASSERT_EQ_U32(count, 1);
    ASSERT_STREQ(paths[0], "D:\\Work\\EveryZip\\device_notes.md");
    fdb_free_path_list(paths, count);
    fdb_close(db);

    remove(fdb_path);
}

static int run_import_mode(const char* txt_path, const char* fdb_path)
{
    Fdb* db = fdb_open();
    uint32_t imported = 0;

    if (!fdb_import_path_list_file(db, txt_path, &imported)) {
        printf("Failed to open input file: %s\n", txt_path);
        fdb_close(db);
        return 1;
    }

    if (!fdb_save(db, fdb_path)) {
        printf("Failed to save fdb file: %s\n", fdb_path);
        fdb_close(db);
        return 1;
    }

    printf("Imported %u paths from %s\n", imported, txt_path);
    printf("String pool: %u strings\n", db->string_count);
    printf("Search index: %u tokens\n", db->index_count);
    printf("Wrote %s\n", fdb_path);

    fdb_close(db);
    return 0;
}

static int run_import2_mode(const char* txt_path, const char* fdb_path)
{
    Fdb* db = fdb_open();
    uint32_t imported = 0;

    if (!fdb_import_path_list_file(db, txt_path, &imported)) {
        printf("Failed to open input file: %s\n", txt_path);
        fdb_close(db);
        return 1;
    }

    fdb_compact_string_storage(db);
    fdb_compact_path_storage(db);

    if (!fdb2_save(db, fdb_path)) {
        printf("Failed to save fdb2 file: %s\n", fdb_path);
        fdb_close(db);
        return 1;
    }

    printf("Imported %u paths from %s\n", imported, txt_path);
    printf("String pool: %u strings\n", db->string_count);
    printf("Wrote %s\n", fdb_path);

    fdb_close(db);
    return 0;
}

static int run_search_mode(const char* fdb_path, const char* keyword)
{
    Fdb* db = fdb_open();
    if (!fdb_load(db, fdb_path)) {
        printf("Failed to open fdb file: %s\n", fdb_path);
        fdb_close(db);
        return 1;
    }

    char** paths = NULL;
    uint32_t count = fdb_search(db, keyword, &paths);
    printf("Found %u paths for \"%s\"\n", count, keyword);
    const uint32_t preview = count < 20 ? count : 20;
    for (uint32_t i = 0; i < preview; ++i) {
        printf("%s\n", paths[i]);
    }
    fdb_free_path_list(paths, count);
    fdb_close(db);
    return 0;
}

static char* trim_command_line(char* line)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    return line;
}

static int run_open_mode(const char* fdb_path)
{
    Fdb* db = fdb_open();
    clock_t t0 = clock();
    if (!fdb_load(db, fdb_path)) {
        printf("Failed to open fdb file: %s\n", fdb_path);
        fdb_close(db);
        return 1;
    }
    clock_t t1 = clock();

    free(db->string_buckets);
    db->string_buckets = NULL;
    clock_t t2 = clock();

    printf("Opened %s\n", fdb_path);
    printf("Paths: %u, strings: %u, tokens: %u\n", db->path_count, db->string_count, db->index_count);
    printf("Load: %.2f ms, prepare: %.2f ms\n", elapsed_ms(t0, t1), elapsed_ms(t1, t2));
    print_process_memory_usage();
    printf("Commands: search <keyword>, quit\n");

    char line[4096];
    for (;;) {
        printf("fdb> ");
        fflush(stdout);
        if (!fgets(line, (int)sizeof(line), stdin)) break;

        char* command = trim_command_line(line);
        if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) break;
        if (strncmp(command, "search ", 7) == 0) {
            char* keyword = trim_command_line(command + 7);
            if (keyword[0] == '\0') {
                printf("Usage: search <keyword>\n");
                continue;
            }

            char** paths = NULL;
            clock_t s0 = clock();
            uint32_t count = fdb_search(db, keyword, &paths);
            clock_t s1 = clock();

            printf("Found %u paths for \"%s\" in %.2f ms\n", count, keyword, elapsed_ms(s0, s1));
            const uint32_t preview = count < 20 ? count : 20;
            for (uint32_t i = 0; i < preview; ++i) {
                printf("%s\n", paths[i]);
            }
            if (count > preview) printf("... %u more\n", count - preview);
            fdb_free_path_list(paths, count);
            continue;
        }

        if (command[0] != '\0') {
            printf("Unknown command. Use: search <keyword>, quit\n");
        }
    }

    fdb_close(db);
    return 0;
}

static int run_open2_mode(const char* fdb_path)
{
    Fdb* db = fdb_open();
    clock_t t0 = clock();
    if (!fdb2_load(db, fdb_path)) {
        printf("Failed to open fdb2 file: %s\n", fdb_path);
        fdb_close(db);
        return 1;
    }
    clock_t t1 = clock();

    printf("Opened %s\n", fdb_path);
    printf("Paths: %u, strings: %u\n", db->path_count, db->string_count);
    printf("Load: %.2f ms\n", elapsed_ms(t0, t1));
    print_process_memory_usage();
    printf("Commands: search <keyword>, quit\n");

    char line[4096];
    for (;;) {
        printf("fdb2> ");
        fflush(stdout);
        if (!fgets(line, (int)sizeof(line), stdin)) break;

        char* command = trim_command_line(line);
        if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) break;
        if (strncmp(command, "search ", 7) == 0) {
            char* keyword = trim_command_line(command + 7);
            if (keyword[0] == '\0') {
                printf("Usage: search <keyword>\n");
                continue;
            }

            char** paths = NULL;
            clock_t s0 = clock();
            uint32_t count = fdb_search(db, keyword, &paths);
            clock_t s1 = clock();

            printf("Found %u paths for \"%s\" in %.2f ms\n", count, keyword, elapsed_ms(s0, s1));
            const uint32_t preview = count < 20 ? count : 20;
            for (uint32_t i = 0; i < preview; ++i) {
                printf("%s\n", paths[i]);
            }
            if (count > preview) printf("... %u more\n", count - preview);
            fdb_free_path_list(paths, count);
            continue;
        }

        if (command[0] != '\0') {
            printf("Unknown command. Use: search <keyword>, quit\n");
        }
    }

    fdb_close(db);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "import") == 0) {
        const char* txt_path = argc >= 3 ? argv[2] : "scripts\\all_files.txt";
        const char* fdb_path = argc >= 4 ? argv[3] : "scripts\\all_files.fdb";
        return run_import_mode(txt_path, fdb_path);
    }

    if (argc >= 2 && strcmp(argv[1], "import2") == 0) {
        const char* txt_path = argc >= 3 ? argv[2] : "scripts\\all_files.txt";
        const char* fdb_path = argc >= 4 ? argv[3] : "scripts\\all_files.fdb2";
        return run_import2_mode(txt_path, fdb_path);
    }

    if (argc >= 2 && strcmp(argv[1], "search") == 0) {
        const char* fdb_path = argc >= 3 ? argv[2] : "scripts\\all_files.fdb";
        const char* keyword = argc >= 4 ? argv[3] : "device";
        return run_search_mode(fdb_path, keyword);
    }

    if (argc >= 2 && strcmp(argv[1], "open") == 0) {
        const char* fdb_path = argc >= 3 ? argv[2] : "scripts\\all_files.fdb";
        return run_open_mode(fdb_path);
    }

    if (argc >= 2 && strcmp(argv[1], "open2") == 0) {
        const char* fdb_path = argc >= 3 ? argv[2] : "scripts\\all_files.fdb2";
        return run_open2_mode(fdb_path);
    }

    printf("=== FDB format demo tests ===\n");
    RUN_TEST(TestFdbRoundTripSearchAndDelete);
    printf("\nTo import scripts\\all_files.txt, run:\n");
    printf("  TestFdb.exe import scripts\\all_files.txt scripts\\all_files.fdb\n");
    printf("To search scripts\\all_files.fdb, run:\n");
    printf("  TestFdb.exe search scripts\\all_files.fdb device\n");
    printf("To open hot-search interactive mode, run:\n");
    printf("  TestFdb.exe open scripts\\all_files.fdb\n");
    printf("To import/open fdb2, run:\n");
    printf("  TestFdb.exe import2 scripts\\all_files.txt scripts\\all_files.fdb2\n");
    printf("  TestFdb.exe open2 scripts\\all_files.fdb2\n");
    printf("\n=== All FDB tests passed! ===\n");
    return 0;
}
