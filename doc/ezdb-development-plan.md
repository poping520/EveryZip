# ezdb 自定义路径索引数据库需求与设计

## 1. 背景与目标

`ezdb` 是 EveryZip 面向“大量文件路径索引和快速搜索”的自定义数据库格式。它不追求通用 SQL 能力，而是针对 `path + size + modified_time` 这类路径记录做极致的构建、搜索、空间和运行时内存优化。

当前格式版本为 `EZDB0007`，不兼容 v1-v6。旧 `.ezdb` 文件需要重新构建。

核心目标：

- 文件扩展名为 `.ezdb`。
- 核心库使用 C 语言实现，在 Windows 上编译运行。
- 保存文件路径记录：`path`、`size`、`modified_time`。
- 支持 build、open、info、get、search、insert、update、delete。
- 支持事务批量写入和 `insert_many`。
- 支持 500 万到千万级路径数据的快速搜索，常见热查询目标 300ms 以内。
- 通过目录树、字符串复用、压缩 records、压缩 postings 降低磁盘占用。
- 打开后不常驻完整 path 数组，只按需重建返回结果路径。
- 文件头和 section 预留扩展能力，后续可保存其他类型数据。

第一阶段以独立库和 `EzdbBench` 命令行工具验证格式、空间和搜索性能；暂不直接替换 EveryZip 主程序现有 SQLite 数据库，也不接入 UI。

## 2. 数据模型

每条文件记录包含：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `uint32_t` | 记录内部编号，从 0 开始递增。 |
| `path` | UTF-8 字符串 | 文件完整路径，例如 `C:\Program Files\app\a.dll`。 |
| `size` | `uint64_t` | 文件字节大小。 |
| `modified_time` | `uint64_t` | 文件最后修改时间，Unix 秒级时间戳。 |
| `flags` | `uint32_t` | 删除标记、扩展标记等。 |

完整路径拆成目录和文件名：

```text
C:\Program Files\App\bin\a.dll
dir  = C:\Program Files\App\bin
name = a.dll
```

这样可以复用目录组件和常见文件名，记录区只保存 `dir_id + name_offset/name_len`，返回结果时再按需重建完整路径。

## 3. 文件格式

`.ezdb` 使用固定文件头加分段 section：

```text
+------------------+
| EzdbHeader       |
+------------------+
| file records     |
+------------------+
| dir records      |
+------------------+
| string pool      |
+------------------+
| postings         |
+------------------+
| index            |
+------------------+
| append delta log |
+------------------+
| reserved / ext   |
+------------------+
```

文件头保存：

- magic：固定为 `EZDB0007`。
- base_file_count：压缩基座中的文件记录数。
- file_count：逻辑文件记录总数，包含 delta log 中追加的新 id。
- active_count：未删除记录数。
- dir_count：目录节点数量。
- 各 section 的 offset、size、压缩标记和原始大小。
- delta offset/size：append-only 增删改日志位置和大小。
- reserved offset/size：未来扩展区域。

### 3.1 Records

file records 使用 compact varint 磁盘编码，section 可选 zlib 压缩。打开数据库时通过 zlib 流式解码，直接填充列式内存数组：

```text
parent_dir_id[]        uint32_t
name_offset[]          uint32_t
name_len[]             uint16_t
size32[]               uint32_t，超过 32 位时进入 overflow 表
modified_time32[]      uint32_t，超过 32 位时进入 overflow 表
```

这样避免常驻 `EzdbDiskFile[record]` 结构数组，也不需要在 open 时分配完整 raw records 临时缓冲。

### 3.2 Directories And Names

`dir records` 保存目录树节点：父目录 id、组件名 offset/len、该目录子树对应的连续 file-id range。构建阶段按目录树 DFS 顺序重排文件记录，使目录命中可直接扩展为 `first_file_id + file_count`。

`string pool` 保存目录组件名和文件名，按完整字符串去重。文件记录和目录记录只保存 offset/len。

### 3.3 Index And Postings

搜索索引拆成两类：

- 文件名 gram 索引：gram -> file id postings。
- 目录组件 gram 索引：gram -> dir id postings。

索引 entry：

```c
typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
    uint64_t offset;
} EzdbDiskIndex;
```

gram 以 UTF-8 token 为单位生成：

- ASCII 字符按单字节 token 处理，并做 ASCII casefold。
- 非 ASCII 字符按完整 UTF-8 codepoint token 处理，支持中文路径搜索。
- 1/2/3 token gram 同时用于文件名索引和目录组件索引。

postings 使用自适应容器：

- array container：低频或稀疏 id 使用 delta varint。
- range container：连续或近连续 id 使用 `start + len` run 编码。
- bitset container：高频稠密 id 使用 bitset。

每个 postings container 会按收益独立选择 zlib 压缩。查询时只读取命中的少数 container，不加载整个 postings section。

## 4. 搜索设计

搜索语义是“路径子串搜索”：

- `index.js` 匹配完整路径中包含 `index.js` 的文件。
- `.dll` 匹配后缀或文件名中包含 `.dll` 的文件。
- `node_modules` 匹配目录路径中包含 `node_modules` 的文件。
- `设计`、`方案` 等中文关键词按 UTF-8 token gram 命中。

查询流程：

```text
输入 keyword
  |
  +-- 按 token 数选择 1/2/3-gram
  +-- 查 file index，得到文件名命中的 file id
  +-- 查 dir index，得到目录组件命中的 dir id
  +-- 将 dir id 展开为目录子树 file-id range
  +-- 合并候选 file id
  +-- 长关键词对候选完整路径做子串确认
  +-- 只为最终结果构造 path 并 callback
```

高频短词，例如 `a`，仍走索引，但如果 `limit=0` 返回接近全库，耗时主要来自候选确认和结果回调。

## 5. CRUD 设计

v7 使用“压缩基座 + append-only delta log + write transaction”：

- build 完成后的 records、dirs、strings、index、postings 作为不可变压缩基座。
- insert/update/delete 追加 delta log，不在基座中间移动数据。
- 单条写入默认保持“调用成功返回前持久化”。
- 批量写入通过事务延迟 flush，commit 时一次性写 batch frame、更新 header 并落盘。
- open 时 replay 已提交 delta batch，构建 id 覆盖层和 tombstone。

v7 已实现：

- `ezdb_insert`
- `ezdb_update`
- `ezdb_delete`
- `ezdb_begin_write`
- `ezdb_commit_write`
- `ezdb_rollback_write`
- `ezdb_insert_many`

批量插入时会预分配 active bitset、delta 数组和 delta hash bucket，避免逐条扩容。后续需要增加 compact，把 delta log 吸收到新的压缩基座中。

## 6. 内存设计

打开数据库时加载：

- compact file records 解码后的列式文件记录数组。
- 目录记录表。
- 字符串池。
- 文件名索引和目录组件索引。
- delta 覆盖层、active bitset、covered_base_bits。

按需读取：

- postings container。
- 返回结果对应的完整 path。

设计原则：

- 不常驻完整 path 数组。
- 搜索候选去重使用 bitset，530 万文件约 0.63 MB。
- 查询只为最终结果分配完整 path。
- 大规模 delta 会增加常驻内存，需要通过 compact 或 delta 压缩继续优化。

## 7. 空间设计

当前主要空间来源：

- records：compact varint 文件记录。
- dirs：目录记录和目录组件。
- names：文件名字符串池。
- postings：文件名和目录组件 1/2/3-gram 倒排表。
- index：gram 到 postings 的映射。
- delta：在线增删改日志，批量插入时保存新增完整 path。

已完成的主要空间优化：

- 完整路径改为目录树 + 文件名存储。
- 文件 id 按目录树 DFS 顺序分配，目录命中转换为 file-id range。
- records 使用 compact varint 和 zlib section 压缩。
- string pool 保存目录组件和文件名，避免重复完整路径。
- postings 使用 array/range/bitset 自适应容器，并按 container 独立压缩。
- open 阶段 records 流式解压成列式数组，降低临时内存和常驻内存。

后续空间优化方向：

- 实现 compact，吸收大规模 delta log。
- delta path 增加字符串池或块压缩。
- size/mtime 使用 block base-delta 或按页懒加载。
- 字符串池按页加载或增加块压缩。
- 高频 postings 支持更细粒度懒展开。

## 8. 性能验证

### 8.1 目标

- 500k 数据：常见关键词、目录词、不存在词搜索稳定在 300ms 以内。
- 500 万到千万级数据：热查询目标 300ms 以内。
- 极高频短词 `limit=0` 允许以返回量为主要成本单独记录。
- 500K 批量插入目标从分钟级降到秒级到十几秒级。

### 8.2 工具

`EzdbBench`：

```text
EzdbBench build <input.txt> <output.ezdb>
EzdbBench info <db.ezdb>
EzdbBench get <db.ezdb> <id>
EzdbBench search <db.ezdb> <keyword> [limit]
EzdbBench open <db.ezdb>
EzdbBench insert-file <db.ezdb> <input.txt> [--transaction]
```

`open` 会进入交互模式，打开一次 `.ezdb` 后可反复搜索。

### 8.3 当前 530 万样本 v7 Release 结果

测试输入：`test_data\files_5300K.txt`  
基座输出：`test_data\files_5300K_v7_final.ezdb`  
批量插入测试库：`test_data\files_5300K_v7_insert500k_final.ezdb`

构建指标：

| 指标 | 数值 |
| --- | ---: |
| 记录数 | 5,299,514 |
| 原始 txt | 777,734,595 bytes / 741.71 MB |
| ezdb v7 基座文件 | 146,861,300 bytes / 140.06 MB |
| 构建耗时 | 19,226 ms / 19.23s |
| 构建峰值 working set | 1,245.05 MB |
| 构建结束 working set | 8.14 MB |
| 构建结束 private | 2.20 MB |

构建阶段耗时：

| 阶段 | ms |
| --- | ---: |
| parse/tree | 5,828 |
| dfs | 43 |
| write base | 1,793 |
| file index | 10,581 |
| dir index | 928 |
| internal total | 19,226 |

section 体积：

| section | MB | 占比 |
| --- | ---: | ---: |
| records | 29.71 | 21.21% |
| dirs | 4.04 | 2.89% |
| names | 11.59 | 8.27% |
| index | 3.23 | 2.31% |
| postings | 91.49 | 65.32% |
| total | 140.06 | 100.00% |

500K 批量插入指标：

| 指标 | 数值 |
| --- | ---: |
| 插入源 | `test_data\files_500k.txt` |
| 解析记录 | 500,000 |
| open | 389 ms |
| parse | 202 ms |
| insert/commit | 903 ms / 0.90s |
| total | 1,494 ms / 1.49s |
| 吞吐 | 553,709.86 rows/s |
| first_id / last_id | 5,299,514 / 5,799,513 |
| 插入后 records | 5,799,514 |
| 插入后 active | 5,799,514 |
| delta 增长 | 66,788,758 bytes / 63.69 MB |
| 插入后文件大小 | 213,650,058 bytes / 203.75 MB |
| 插入后进程峰值 working set | 323.13 MB |
| reopen info private | 250.70 MB |

插入 500K 后查询性能：

| 关键词 | limit | open_ms | search_ms | returned | private MB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `a` | 20 | 546 | 78 | 20 | 252.42 |
| `index.js` | 20 | 532 | 87 | 20 | 252.88 |
| `metadata.bin` | 20 | 557 | 70 | 20 | 251.32 |
| `设计` | 20 | 559 | 70 | 3 | 251.40 |
| `zzznotfoundzzz` | 20 | 547 | 73 | 0 | 251.39 |

结论：

- v7 基座在 530 万样本上约 140.06 MB，明显小于原始 txt。
- 常见查询、中文查询和不存在词在插入 500K delta 后仍保持 300ms 内。
- 500K 批量插入从 v6 逐条强制落盘的分钟级问题降到约 1.49s。
- 大规模 delta 主要问题变为磁盘增长和 reopen 后常驻内存增长，下一步应优先实现 compact 与 delta 压缩。

## 9. C API

公开接口位于 `src/ezdb/ezdb.h`。

```c
int ezdb_build_from_text(const char* input_txt, const char* output_ezdb);
int ezdb_open(const char* path, Ezdb** out_db);
void ezdb_close(Ezdb* db);

uint32_t ezdb_count(Ezdb* db);
uint32_t ezdb_active_count(Ezdb* db);
uint64_t ezdb_file_size(Ezdb* db);
int ezdb_stats(Ezdb* db, EzdbStats* out_stats);

int ezdb_get_by_id(Ezdb* db, uint32_t id, EzdbSearchResult* out_result);
void ezdb_free_result(EzdbSearchResult* result);

int ezdb_search_path(Ezdb* db,
                     const char* keyword,
                     uint32_t limit,
                     EzdbSearchCallback callback,
                     void* user_data);

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
- 查询结果字符串由 `ezdb` 分配，调用方用 `ezdb_free_result` 释放。
- 搜索使用 callback，避免一次性分配巨大结果数组。
- 单条写入成功返回前已持久化；事务内写入延迟到 `ezdb_commit_write`。
- 只读打开时写接口返回 `EZDB_ERR_READ_ONLY`。

## 10. 编码与兼容性

- 输入 txt 按 UTF-8 读取。
- `.ezdb` 内部路径字符串按 UTF-8 保存。
- `EzdbBench` 在 Windows 下使用宽字符命令行转 UTF-8，避免中文参数被本地代码页破坏。
- 当前索引按 UTF-8 token 建 1/2/3-gram，中文路径可以被索引搜索。
- 当前大小写折叠只处理 ASCII。
- 完整 Unicode casefold 暂不作为当前目标，后续如有需要再引入 Unicode 规范化表或额外 normalized key。

## 11. 已知限制

- v7 不兼容 v1-v6 `.ezdb` 文件。
- 目录记录、字符串池和索引仍在 open 后整体常驻。
- 目录索引基于路径组件，不为任意跨组件子串单独建索引。
- 高频 1 字节关键词可能返回接近全库，`limit=0` 耗时主要由结果遍历决定。
- batch frame 具备 committed replay 语义，但还没有 CRC、自动截断不完整 delta 和并发读写控制。
- 大规模 delta 会增加磁盘占用和 reopen 后常驻内存，需要 compact 或独立 delta 压缩索引。

## 12. 后续迭代路线

优先级从高到低：

1. 实现 compact：重建干净基座，吸收 delta log，丢弃 tombstone 和 stale 版本。
2. 为 delta path 增加字符串复用或块压缩，降低批量插入后的文件增长。
3. 增强崩溃恢复：batch CRC、打开时截断半截 frame、写入错误回滚。
4. 优化大 delta 覆盖层内存：delta path arena 压缩、按页加载或独立 delta 索引。
5. 优化高频短词 `limit > 0` 的候选展开，尽早停止 range/bitset 扫描。
6. 评估 Windows mmap，让 records、strings、postings 支持更细粒度懒加载。
7. 在 EveryZip 中增加实验开关，先用于文件路径搜索，再评估替换部分 SQLite 查询。
