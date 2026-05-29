# ezdb 自定义索引数据库需求与设计

## 1. 背景与目标

`ezdb` 是 EveryZip 面向“大量文件路径、归档文件和归档内部条目快速搜索”的自定义数据库格式。它不追求通用 SQL 能力，而是针对 EveryZip 的核心业务数据做紧凑存储、快速构建和低延迟搜索。

当前格式版本为 `EZDB0008`，不兼容 v1-v7。旧 `.ezdb` 文件需要重新构建。

核心目标：

- 文件扩展名为 `.ezdb`。
- 核心库使用 C 语言实现，在 Windows 上编译运行。
- 支持单项 archives 数据：本机压缩/归档文件。
- 支持双项 archives + entries 数据：归档文件及其内部文件条目。
- 支持 archive path、entry path、archive + entry 组合搜索。
- 支持 build、open、info、get、search、insert、update、delete。
- 支持事务批量写入和 `insert_many`。
- 支持百万到千万级路径数据快速搜索，常见热查询目标 300ms 以内。
- 通过目录树、字符串复用、压缩 records、压缩 postings 降低磁盘占用。
- 打开后不常驻完整 path 数组，只按需重建返回结果路径。
- 第一阶段只开发 ezdb 和 `EzdbBench`，暂不替换 EveryZip 主程序 SQLite，也不改 UI。

## 2. 数据模型

### 2.1 Archives

每条 archive 记录对应 EveryZip `archives` 表的一行：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `uint32_t` | ezdb 内部 archive id，从 0 开始。 |
| `drive_letter` | `char` | 盘符，例如 `C`。 |
| `file_ref_number` | `uint64_t` | NTFS file reference number。 |
| `usn` | `int64_t` | USN Journal 位置。 |
| `file_path` | UTF-8 字符串 | 归档文件完整路径。 |
| `file_size` | `uint64_t` | 文件字节大小。 |
| `modified_time` | `uint64_t` | 文件修改时间，原样保存调用方输入值。 |

archive path 仍拆成目录和文件名：

```text
C:\Program Files\App\bin\a.zip
dir  = C:\Program Files\App\bin
name = a.zip
```

构建阶段按目录树 DFS 顺序重排 archive id，使目录命中可扩展为连续 archive id range。

### 2.2 Entries

每条 entry 记录对应 EveryZip `entries` 表的一行：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `uint32_t` | ezdb 内部 entry id，从 0 开始。 |
| `archive_id` | `uint32_t` | 关联的 ezdb archive id。 |
| `entry_path` | UTF-8 字符串 | 规范化后的归档内部条目路径。 |
| `entry_raw_path` | BLOB | 可选，仅非 UTF-8 原始条目路径需要原始字节定位时保存。 |
| `compressed_size` | `int64_t` | 压缩后大小，允许 `-1` 表示不可得。 |
| `original_size` | `uint64_t` | 原始大小。 |
| `modified_time` | `uint64_t` | 条目修改时间，原样保存调用方输入值。 |

删除 archive 时，这个 archive 对应的 entries 在搜索和读取语义上级联失效。

## 3. 文件格式

`.ezdb` 使用固定文件头加分段 section：

```text
+----------------------+
| EzdbHeader           |
+----------------------+
| archive file records |
+----------------------+
| archive metadata     |
+----------------------+
| dir records          |
+----------------------+
| string pool          |
+----------------------+
| archive postings     |
+----------------------+
| archive indexes      |
+----------------------+
| entry records        |
+----------------------+
| entry raw/blob pool  |
+----------------------+
| entry postings       |
+----------------------+
| entry index          |
+----------------------+
| append delta log     |
+----------------------+
```

文件头保存：

- magic：固定为 `EZDB0008`。
- `file_count` / `active_count`：archive 逻辑记录数和活跃数。
- `entry_count` / `active_entry_count`：entry 逻辑记录数和活跃数。
- archive records、archive metadata、dir records、string pool 的 offset/size/raw_size/flags。
- archive file index、archive dir index、entry index 的 offset/count。
- postings section 的 offset/size，entry postings 追加在同一 postings 基址后。
- entry records、raw/blob pool 的 offset/size/raw_size/flags。
- delta offset/size：append-only 增删改日志位置和大小。

### 3.1 Archive Records

archive path records 使用 compact varint 磁盘编码，section 可选 zlib 压缩。打开数据库时通过 zlib 流式解码成列式数组：

```text
parent_dir_id[]        uint32_t
name_offset[]          uint32_t
name_len[]             uint16_t
size32[]               uint32_t，超过 32 位时进入 overflow 表
modified_time32[]      uint32_t，超过 32 位时进入 overflow 表
archive_meta[]         file_ref_number + usn + drive_letter
```

### 3.2 Entries

entries 使用固定记录数组加 blob pool：

```text
archive_id
entry_path_offset / entry_path_len
raw_offset / raw_len
compressed_size
original_size
modified_time
```

`entry_path` 和 `entry_raw_path` 都保存在 raw/blob pool 中；`entry_path` 追加 NUL 方便内部字符串处理，`entry_raw_path` 按原始字节保存。

### 3.3 Index And Postings

搜索索引拆成三类：

- archive 文件名 gram 索引：gram -> archive id postings。
- archive 目录组件 gram 索引：gram -> dir id postings，再扩展为 archive id range。
- entry path gram 索引：gram -> entry id postings。

索引 entry：

```c
typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
    uint32_t raw_size;
    uint64_t offset;
} EzdbDiskIndex;
```

gram 以 UTF-8 token 为单位生成：

- ASCII 字符按单字节 token 处理，并做 ASCII casefold。
- 非 ASCII 字符按完整 UTF-8 codepoint token 处理，支持中文路径搜索。
- 1/2/3 token gram 同时用于 archive 和 entry 索引。

postings 使用自适应容器：

- array container：低频或稀疏 id 使用 delta varint。
- range container：连续或近连续 id 使用 `start + len` run 编码。
- bitset container：高频稠密 id 使用 bitset。

每个 postings container 会按收益独立选择 zlib 压缩。查询时只读取命中的少数 container，不加载整个 postings section。

## 4. 搜索设计

搜索语义支持查询表达式。`ezdb_search_path` 保留为 archive-only 兼容入口；新增 `ezdb_search` 支持 scope 参数。

scope：

| Scope | 说明 |
| --- | --- |
| `EZDB_SEARCH_ARCHIVE_PATH` | 只搜索 `archives.file_path`。 |
| `EZDB_SEARCH_ENTRY_PATH` | 只搜索 `entries.entry_path`。 |
| `EZDB_SEARCH_COMBINED_PATH` | 搜索 archive path + entry path 的组合。 |
| `EZDB_SEARCH_ALL` | archive-only 结果 + entry-only 结果。 |

支持的查询语法：

| 语法 | 作用 | 示例 |
| --- | --- | --- |
| `关键词` | 普通包含搜索 | `发票` |
| `关键词1 关键词2` | AND，同时包含多个词 | `合同 甲方` |
| `"短语"` | 连续短语或字面特殊符号 | `"用户协议"`、`"a*b"` |
| `*` | 匹配任意长度 UTF-8 字符序列 | `张*明` |
| `?` | 匹配单个 UTF-8 字符 | `第?章` |
| `!关键词` | 排除关键词 | `合同 !草稿` |
| `词1 | 词2` | OR，任意一个匹配即可 | `发票 | 收据` |
| `( ... )` | 分组组合条件 | `(发票 | 收据) 报销` |

查询流程：

```text
输入 keyword + scope
  |
  +-- 解析为查询 AST
  +-- 从正向 term/phrase/wildcard 中提取可索引 literal
  +-- archive scope：复用 file index + dir index 生成 archive 候选 bitset
  +-- entry scope：复用 entry index 生成 entry 候选 bitset
  +-- combined scope：entry index 候选 + archive 命中扩展到 entries
  +-- AND 分支取交集，OR 分支取并集，NOT 只做最终过滤
  +-- 没有正向 literal 时允许全量扫描，并在 limit > 0 时提前停止
  +-- 对候选完整字符串做 AST 最终匹配
  +-- 只为最终结果构造路径并 callback
```

combined scope 中，每个 term/wildcard 可以命中 archive path 或 entry path；最终匹配使用 `archive_path + '\n' + entry_path`，不会把短语跨字段边界当作连续路径。

## 5. CRUD 设计

v8 延续“压缩基座 + append-only delta log + write transaction”：

- build 完成后的 records、dirs、strings、indexes、postings 作为不可变压缩基座。
- archive insert/update/delete 追加 delta log，不在基座中间移动数据。
- 单条写入默认保持“调用成功返回前持久化”。
- 批量写入通过事务延迟 flush，commit 时一次性写 batch frame、更新 header 并落盘。
- open 时 replay 已提交 delta batch，构建 id 覆盖层和 tombstone。
- `ezdb_delete_archive_by_ref` 根据 `drive_letter + file_ref_number` 删除 archive，并让其 entries 级联失效。

已实现：

- `ezdb_insert`
- `ezdb_update`
- `ezdb_delete`
- `ezdb_upsert_archive`
- `ezdb_delete_archive_by_ref`
- `ezdb_begin_write`
- `ezdb_commit_write`
- `ezdb_rollback_write`
- `ezdb_insert_many`

后续仍需要实现 compact，把 delta log 吸收到新的压缩基座中，并让大规模 entry 更新也支持更完整的增量索引。

## 6. 内存设计

打开数据库时加载：

- archive compact records 解码后的列式数组。
- archive metadata。
- entry records。
- raw/blob pool。
- 目录记录表。
- 字符串池。
- archive 文件名索引、archive 目录组件索引、entry path 索引。
- delta 覆盖层、active bitset、covered_base_bits。

按需读取：

- postings container。
- 返回结果对应的 archive path、entry path、raw bytes 副本。

设计原则：

- 不常驻完整 archive path 数组。
- 搜索候选去重使用 bitset。
- 查询只为最终结果构造路径和结果对象。
- entry raw bytes 只在 `get-entry` 或结果需要时复制给调用方。

## 7. 空间设计

当前主要空间来源：

- records：compact varint archive path 记录。
- archive metadata：盘符、file reference number、USN。
- dirs：archive 目录记录和目录组件。
- names：archive 文件名与目录组件字符串池。
- entries：entry 固定记录数组。
- raw/blob pool：entry_path 和 entry_raw_path。
- postings：archive 文件名、archive 目录组件、entry_path 的 1/2/3-gram 倒排表。
- indexes：gram 到 postings 的映射。
- delta：在线增删改日志。

已完成的空间优化：

- archive 完整路径改为目录树 + 文件名存储。
- archive id 按目录树 DFS 顺序分配，目录命中转换为 archive id range。
- archive records 使用 compact varint 和 zlib section 压缩。
- entry records 和 raw/blob pool 使用 zlib section 压缩。
- postings 使用 array/range/bitset 自适应容器，并按 container 独立压缩。
- open 阶段 records 流式解压成列式数组，降低临时内存和常驻内存。

后续空间优化方向：

- 实现 compact，吸收大规模 delta log。
- entry path/raw blob 增加字符串复用、分块压缩或按页加载。
- size/mtime 使用 block base-delta 或按页懒加载。
- 字符串池按页加载或增加块压缩。
- 高频 postings 支持更细粒度懒展开。

## 8. 性能验证

### 8.1 目标

- 100k archive-only 样本用于快速功能回归。
- 3200k archive-only 样本用于大数据量构建和搜索性能验证。
- `everyzip.db` SQLite 样本用于 archives + entries 双项导入和搜索验证。
- 常见 archive、entry、combined 热查询目标 300ms 以内。

### 8.2 工具

`EzdbBench`：

```text
EzdbBench build <input.txt> <output.ezdb>
EzdbBench build-archives <input.tsv> <output.ezdb>
EzdbBench import-sqlite <everyzip.db> <output.ezdb>
EzdbBench info <db.ezdb>
EzdbBench get <db.ezdb> <id>
EzdbBench get-archive <db.ezdb> <id>
EzdbBench get-entry <db.ezdb> <id>
EzdbBench search <db.ezdb> <keyword> [limit]
EzdbBench search-v2 <db.ezdb> <archive|entry|combined|all> <keyword> [limit]
EzdbBench open <db.ezdb> [limit]
EzdbBench insert <db.ezdb> <path> [size] [mtime]
EzdbBench insert-file <db.ezdb> <input.txt>
EzdbBench update <db.ezdb> <id> <path> [size] [mtime]
EzdbBench delete <db.ezdb> <id>
EzdbBench delete-archive-ref <db.ezdb> <drive> <file_ref_number>
EzdbBench crud <input.txt> <output.ezdb>
```

查询语法回归脚本：

```text
python tools\ezdb_query_syntax_tests.py --bench cmake-build-codex-release\EzdbBench.exe
```

### 8.3 当前 v8 Release 结果

#### 100k archive-only

测试输入：`test_data\list_files_100k.tsv`  
输出：`test_data\list_files_100k_v8_entryidx.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 100,000 |
| entries | 0 |
| 构建耗时 | 631 ms |
| 文件大小 | 9,555,967 bytes / 9.11 MB |
| 构建峰值 working set | 47.34 MB |
| info private | 11.10 MB |

#### 3200k archive-only

测试输入：`test_data\list_files_3200k.tsv`  
输出：`test_data\list_files_3200k_v8_entryidx.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 3,221,428 |
| entries | 0 |
| 构建耗时 | 14,488 ms / 14.49s |
| 文件大小 | 151,120,729 bytes / 144.12 MB |
| 构建峰值 working set | 930.15 MB |
| open | 426-436 ms |
| info private | 197.41 MB |

抽测查询：

| scope | keyword | limit | search_ms | returned |
| --- | --- | ---: | ---: | ---: |
| archive | `index` | 20 | 4 | 20 |
| archive | `zzznotfoundzzz` | 20 | 0 | 0 |
| archive | `a` | 20 | 29 | 20 |

#### everyzip.db 双项导入

测试输入：`test_data\everyzip.db`  
输出：`test_data\everyzip_v8_entryidx.ezdb`

| 指标 | 数值 |
| --- | ---: |
| archives | 3,476 |
| entries | 547,902 |
| SQLite 读取耗时 | 157 ms |
| ezdb 构建耗时 | 2,094 ms |
| 文件大小 | 21,532,464 bytes / 20.54 MB |
| 构建峰值 working set | 365.23 MB |
| info private | 54.17 MB |
| `entry_raw_path` 回读 | 已验证，示例 raw=12 |

抽测查询：

| scope | keyword | limit | search_ms | returned |
| --- | --- | ---: | ---: | ---: |
| entry | `kwpsaitablestyle` | 5 | 1 | 5 |
| combined | `download.7z diff_resource_file` | 5 | 4 | 5 |
| combined | `kwpsaitablestyle diff_resource_file` | 5 | 4 | 1 |
| entry | `zzznotfoundzzz` | 5 | 0 | 0 |

级联删除验证：

- 对 `test_data\everyzip_v8_entryidx_delete_test.ezdb` 删除 archive id `3474` 后，顺序执行 combined 查询 `kwpsaitablestyle diff_resource_file` 返回 0。

查询语法回归：

- `tools\ezdb_query_syntax_tests.py` 全部通过。

## 9. C API

公开接口位于 `src/ezdb/ezdb.h`。

```c
int ezdb_build_from_text(const char* input_txt, const char* output_ezdb);
int ezdb_build_from_archives_tsv(const char* input_tsv, const char* output_ezdb);
int ezdb_build_snapshot(const EzdbArchiveRecord* archives,
                        uint32_t archive_count,
                        const EzdbEntryRecord* entries,
                        uint32_t entry_count,
                        const char* output_ezdb);

int ezdb_open(const char* path, Ezdb** out_db);
void ezdb_close(Ezdb* db);

uint32_t ezdb_archive_count(Ezdb* db);
uint32_t ezdb_active_archive_count(Ezdb* db);
uint32_t ezdb_entry_count(Ezdb* db);
uint32_t ezdb_active_entry_count(Ezdb* db);
uint64_t ezdb_file_size(Ezdb* db);
int ezdb_stats(Ezdb* db, EzdbStats* out_stats);

int ezdb_get_archive(Ezdb* db, uint32_t id, EzdbArchiveResult* out_result);
int ezdb_get_entry(Ezdb* db, uint32_t id, EzdbEntryResult* out_result);
void ezdb_free_archive_result(EzdbArchiveResult* result);
void ezdb_free_entry_result(EzdbEntryResult* result);

int ezdb_search(Ezdb* db,
                const char* keyword,
                uint32_t scope,
                uint32_t limit,
                EzdbSearchV2Callback callback,
                void* user_data);

int ezdb_search_path(Ezdb* db,
                     const char* keyword,
                     uint32_t limit,
                     EzdbSearchCallback callback,
                     void* user_data);

int ezdb_upsert_archive(Ezdb* db, const EzdbArchiveRecord* record, uint32_t* out_id);
int ezdb_delete_archive_by_ref(Ezdb* db, char drive_letter, uint64_t file_ref_number);

int ezdb_begin_write(Ezdb* db, uint32_t flags);
int ezdb_commit_write(Ezdb* db);
int ezdb_rollback_write(Ezdb* db);
int ezdb_insert_many(Ezdb* db, const EzdbFileRecord* records, uint32_t count, uint32_t* first_id);

int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id);
int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record);
int ezdb_delete(Ezdb* db, uint32_t id);

const char* ezdb_error_message(int code);
```

API 约定：

- 返回值使用整数错误码。
- 查询结果字符串和 raw bytes 由 `ezdb` 分配，调用方使用对应 free API 释放。
- 搜索使用 callback，避免一次性分配巨大结果数组。
- `ezdb_search_path` 是兼容旧 path-only 使用方式的 archive-only 包装。
- 单条写入成功返回前已持久化；事务内写入延迟到 `ezdb_commit_write`。
- 只读打开时写接口返回 `EZDB_ERR_READ_ONLY`。

## 10. 编码与兼容性

- 输入文本按 UTF-8 读取。
- `.ezdb` 内部路径字符串按 UTF-8 保存。
- `EzdbBench` 在 Windows 下使用宽字符命令行转 UTF-8，避免中文参数被本地代码页破坏。
- 当前索引按 UTF-8 token 建 1/2/3-gram，中文路径可以被索引搜索。
- 当前大小写折叠只处理 ASCII。
- `?` 通配符按 UTF-8 字符匹配，一个中文字符算一个字符。
- `modified_time` 不做 epoch/FILETIME 转换，调用方传入什么就保存什么。
- `entry_raw_path` 只保存和回传，不参与搜索。
- 完整 Unicode casefold 暂不作为当前目标，后续如有需要再引入 Unicode 规范化表或额外 normalized key。

## 11. 已知限制

- v8 不兼容 v1-v7 `.ezdb` 文件。
- 目录记录、字符串池、entry records、raw/blob pool 和索引仍在 open 后整体常驻。
- entry path 当前没有目录树压缩，主要依赖 raw/blob section 压缩和 postings 索引。
- 目录索引基于路径组件，不为任意跨组件子串单独建索引。
- 高频 1 字节关键词可能返回接近全库，`limit=0` 耗时主要由候选确认和结果回调决定。
- 纯排除和纯通配符查询允许全库扫描，`limit=0` 在大库上可能明显慢于普通索引查询。
- batch frame 具备 committed replay 语义，但还没有 CRC、自动截断不完整 delta 和并发读写控制。
- 大规模 delta 会增加磁盘占用和 reopen 后常驻内存，需要 compact 或独立 delta 压缩索引。

## 12. 后续迭代路线

优先级从高到低：

1. 实现 compact：重建干净基座，吸收 delta log，丢弃 tombstone 和 stale 版本。
2. 增强崩溃恢复：batch CRC、打开时截断半截 frame、写入错误回滚。
3. 优化 entry path/raw blob：字符串复用、按页加载、块压缩或 mmap。
4. 增强 entry 增量写入：支持替换某个 archive 的 entries 并维护增量 entry index。
5. 优化大 delta 覆盖层内存：delta path arena 压缩、按页加载或独立 delta 索引。
6. 优化高频短词 `limit > 0` 的候选展开，尽早停止 range/bitset 扫描。
7. 评估 Windows mmap，让 records、strings、postings 支持更细粒度懒加载。
8. 在 EveryZip 中增加实验开关，先用于文件路径搜索，再评估替换部分 SQLite 查询。
