# ezdb 自定义路径索引数据库需求与设计

## 1. 背景与目标

EveryZip 当前使用 SQLite 保存归档文件和归档内部条目。SQLite 通用性强，但对于“只保存大量文件路径并做快速路径搜索”的场景，存在额外表结构、B-tree、事务和 SQL 执行开销，数据库文件体积也不容易做到极致紧凑。

`ezdb` 的目标是设计一个专门面向文件路径索引的自定义数据库格式：

- 文件扩展名为 `.ezdb`。
- 核心库使用 C 语言实现，能在 Windows 上编译运行。
- 保存文件路径记录：`path`、`size`、`modified_time`。
- 支持常规增删改查能力。
- 支持千万级别路径数据的快速搜索，热启动查询目标约 300ms 以内，越快越好。
- 当前 v3 以“空间优先但保性能”为目标，采用目录树和组件级索引，避免把完整目录路径重复写进每个文件的倒排表。
- 路径字符串按目录组件和文件名复用，打开数据库时加载文件表、目录表、字符串池和索引，热查询阶段只按需读取 postings。
- 文件格式预留扩展能力，后续可以保存其他类型的数据。

第一阶段以独立库和命令行基准工具验证格式、空间和搜索性能，不直接替换 EveryZip 主程序现有 SQLite 数据库，也不接入 UI。

## 2. 数据模型

### 2.1 文件路径记录

每条文件记录包含：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | `uint32_t` | 记录内部编号，从 0 开始递增。 |
| `path` | UTF-8 字符串 | 文件完整路径，例如 `C:\Program Files\app\a.dll`。 |
| `size` | `uint64_t` | 文件字节大小。 |
| `modified_time` | `uint64_t` | 文件最后修改时间，Unix 秒级时间戳。 |
| `flags` | `uint32_t` | 删除标记、扩展标记等。 |

### 2.2 路径拆分

完整路径拆成两个部分保存：

- `dir`: 最后一个路径分隔符之前的目录前缀。
- `name`: 最后一个路径分隔符之后的文件名。

例如：

```text
C:\Program Files\App\bin\a.dll
dir  = C:\Program Files\App\bin
name = a.dll
```

这样设计的原因：

- 同一目录下通常有大量文件，目录前缀重复率高。
- 目录字符串进入 `dirs` 字符串池后可以复用。
- 记录区只保存 `dir_id + name_offset`，避免每条记录重复写完整路径。
- 查询返回结果时再按需重建完整路径，减少常驻内存。

## 3. 文件格式

`.ezdb` 使用固定文件头加分段 section 的布局。

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
| reserved / ext   |
+------------------+
```

### 3.1 文件头

文件头保存：

- magic：固定为 `EZDB0003`，用于识别文件类型。
- version：格式版本号。v3 不兼容 v1/v2，旧 `.ezdb` 文件需要重新构建。
- file_count：文件记录总数。
- active_count：未删除记录数。
- dir_count：目录节点数量。
- 各 section 的 offset 和 size。
- reserved offset/size，用于未来扩展。

文件头必须保持可演进：

- 新版本可以在尾部增加字段。
- 旧版本读取器遇到未知 section 时可以跳过。
- 修改破坏兼容性的结构时必须提升 version。

### 3.2 records section

当前 v3 使用文件记录和目录记录两个固定长度表：

```c
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
```

优点：

- 按 id 随机访问非常快。
- 可以用 `id * sizeof(record)` 直接定位。
- 实现简单，便于第一阶段验证。

缺点：

- 文件记录仍是 32 字节，目录记录是 20 字节。
- 后续可改为分列存储或 varint record 进一步压缩。

### 3.3 dirs section

`dir records` 保存目录树节点。每个目录节点保存父目录 id、组件名 offset、组件名长度，以及该目录子树对应的连续 file-id 范围。

构建阶段按目录树 DFS 顺序重排文件记录，使每个目录子树都能用 `first_file_id + file_count` 表示。目录关键词命中目录节点后，可以直接扩展为文件 id range，不需要为目录下每个文件重复写入相同目录 gram。

### 3.4 names section

`string pool` 保存目录组件名和文件名，按完整字符串去重。文件记录和目录记录都只保存 `name_offset + name_len`。

这样既避免重复保存完整目录路径，也减少常见文件名、目录名在不同位置重复出现时的空间浪费。

### 3.5 index/postings section

v3 将搜索索引拆成两类：

- 文件名 gram 索引：gram -> file id postings。
- 目录组件 gram 索引：gram -> dir id postings。

每类索引都保存 n-gram 到倒排表位置的映射：

```c
typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint64_t offset;
} EzdbDiskIndex;
```

当前 gram 以 UTF-8 token 为单位生成：

- ASCII 字符仍按单字节 token 处理，并做 ASCII casefold。
- 非 ASCII 字符按完整 UTF-8 codepoint token 处理，避免中文字符被拆成无意义的字节片段。
- 1/2/3 token gram 会同时用于文件名索引和目录组件索引。

`key` 高位保存 gram 类型和编码方式：

- inline key：长度不超过 3 字节的 token gram 直接内联保存。
- hashed key：长度超过 3 字节的 UTF-8 gram 保存 FNV-1a hash 的低 24 bit。
- token 数量保存为 1/2/3，用于查询时选择同宽度 gram。

`postings` 保存 gram 对应的 file id 或 dir id 列表。id 按递增顺序保存，并使用 delta varint 编码：

```text
id0, id1-id0, id2-id1, ...
```

这样可以显著压缩倒排表空间，尤其是相邻 id 差值较小时。

## 4. 搜索设计

### 4.1 搜索语义

第一阶段目标语义是“路径子串搜索”：

- 输入 `index.js`，能匹配完整路径中包含 `index.js` 的文件。
- 输入 `.dll`，能匹配后缀或文件名中包含 `.dll` 的文件。
- 输入 `node_modules`，能匹配目录路径中包含 `node_modules` 的文件。

最终结果必须以完整路径子串判断为准，索引只用于缩小候选集。

### 4.2 组件级 n-gram 倒排索引

对文件名和目录组件分别生成连续 1/2/3 token gram：

```text
index.js -> 1-gram: i, n, d, ...
index.js -> 2-gram: in, nd, de, ...
index.js -> 3-gram: ind, nde, dex, ex., x.j, .js
设计.md -> 1-gram: 设, 计, ., m, d
设计.md -> 2-gram: 设计, 计., .m, md
```

查询时按关键词长度选择 gram 宽度：

- token 数量 1：使用 1-gram。
- token 数量 2：使用 2-gram。
- token 数量 >= 3：使用 3-gram。

然后分别读取文件名索引和目录组件索引的 postings，按 postings 数量从小到大求交集。

优点：

- 支持任意子串搜索，不局限于前缀。
- 查询多个 gram 求交集后候选集通常很小。
- 对文件名、扩展名、目录组件、普通短词统一走索引。
- 目录组件命中后转换为该目录子树的 file-id range。

缺点：

- 当前目录索引按组件匹配，不为跨组件子串建立专门索引。
- 极高频短词，例如 `a`，即使走索引也可能返回接近全库的结果，耗时主要来自结果遍历和回调。

### 4.3 v3 的空间优先策略

v2 对完整路径建立 1/2/3-gram，导致父目录 gram 被重复写入每个子文件的 postings。500k 样本中 v2 postings 约 103MB，是文件体积膨胀的主要原因。

v3 改为：

- 对文件名建 1/2/3-gram。
- 对目录组件建 1/2/3-gram。
- 文件 id 按目录树 DFS 顺序分配，目录命中可扩展为连续 file-id range。
- 查询时对关键词 gram 去重，并按 postings `count` 从小到大求交。
- 任一 gram 不存在时立即返回空结果。
- 字节长度大于 3 的关键词保留最终完整路径确认，避免 n-gram 或 hash 假阳性。
- 短关键词直接使用 gram 命中结果，减少高频短词的大量路径重建成本。

### 4.4 查询流程

```text
输入 keyword
  |
  +-- 按长度生成 1/2/3-gram key
  +-- 查 file index，得到文件名命中的 file id
  +-- 查 dir index，得到目录组件命中的 dir id
  +-- 将 dir id 展开为目录子树 file-id range
  +-- 合并候选 file id
  +-- 长关键词对候选完整路径做子串确认
  +-- 只为最终结果构造 path 并 callback
```

## 5. CRUD 设计

### 5.1 当前状态

当前 v3 已实现：

- build：从 txt 构建 `.ezdb`。
- open/close：打开和关闭数据库。
- info/stats：读取记录数、文件大小和 section 大小。
- get by id：按 id 读取记录并重建完整路径。
- search：路径子串搜索。

当前 v3 暂未真正实现：

- insert
- update
- delete

接口已预留，当前返回只读错误，避免误以为已经支持持久化写入。

### 5.2 后续增删改方案

建议采用 append-only delta log：

- 新增记录：追加 record、dir/name 字符串和必要索引 delta。
- 更新记录：追加新版本，旧版本标记为 stale。
- 删除记录：写 tombstone 标记。
- 查询时过滤 deleted/stale 记录。
- 定期 compact：重写主数据区，丢弃 tombstone 和 stale 版本。

这样做的原因：

- 避免在大文件中间频繁移动数据。
- 写入逻辑简单，崩溃恢复更容易设计。
- compact 可以在后台或用户空闲时执行。

后续需要增加：

- `delta` section：保存增删改日志。
- `record_version` 或 `generation`：区分旧版本和新版本。
- `compact` 工具：重建干净数据库文件。
- 崩溃恢复规则：写入顺序、校验和、提交标记。

## 6. 内存设计

v3 的原则是热查询不做原始文本全表扫描，也不在候选确认阶段频繁随机读文件。

打开数据库时当前加载：

- records 固定记录区。
- index gram 映射表。
- 文件记录表。
- 目录记录表。
- 字符串池。
- 文件名索引。
- 目录组件索引。

按需读取：

- postings 列表。

优点：

- 查询阶段目录组件和文件名字符串都来自内存。
- `get/search` 只有在返回结果时才分配完整 path。
- 相比 v1 使用更多内存，但 `open_ms` 和 `search_ms` 明显更稳定。

后续可继续优化：

- Windows 下用 `CreateFileMapping` / `MapViewOfFile` 做 mmap。
- postings 分块，避免一次加载超大 postings。
- records 改为 mmap 或按页缓存。
- 常用目录字符串增加 LRU 缓存。

## 7. 空间设计

当前主要空间来源：

- records：每条固定 32 字节。
- dirs：目录字符串池。
- names：文件名字符串池。
- postings：文件名和目录组件 1/2/3-gram 倒排表。
- index：文件名/目录组件 gram 到 postings 的映射。

500k 样本当前结果：

```text
files_500k.txt     : 60,188,257 bytes
files_500k_v2.ezdb : 134,873,425 bytes
files_500k_v3.ezdb : 45,809,794 bytes

records_size  : 16,000,000 bytes
dirs_size     :  1,359,180 bytes
names_size    :  2,834,590 bytes
index_size    :    811,248 bytes
postings_size : 24,804,624 bytes
```

下一阶段空间优化优先级：

1. 压缩 records：把 `parent_dir_id`、`name_offset`、`name_len`、flags、size、mtime 分列或 varint 化。
2. postings 分块或引入 bitmap/Roaring bitmap，优化高频 gram 的体积和交集速度。
3. 对字符串池增加块压缩。
4. 对构建阶段内存峰值做优化，当前 Debug 500k 构建峰值约 332MB。

## 8. 性能目标与验证方法

### 8.1 性能目标

- 100k 数据：任意关键词搜索应为毫秒级。
- 500k 数据：常见关键词、目录词、不存在词搜索应稳定在 300ms 以内。
- 500 万到千万级数据：热启动搜索目标 300ms 以内，极高频短词允许以返回量为主要成本。
- 打开数据库时内存占用可按速度优先使用中等内存，但不能构造完整 path 数组。

### 8.2 验证工具

命令行工具 `EzdbBench`：

```text
EzdbBench build <input.txt> <output.ezdb>
EzdbBench info <db.ezdb>
EzdbBench get <db.ezdb> <id>
EzdbBench search <db.ezdb> <keyword> [limit]
```

### 8.3 当前验证结果

- `files_500k.txt`：原 txt 约 60.2 MB。
- v3 Debug 构建 500k 建库约 10.3 秒，`.ezdb` 约 45.8 MB。
- 500k v3 Debug 热查询样例：
  - `a 0`：约 227ms，返回 497,196 条。
  - `aa 0`：约 5ms，返回 7,551 条。
  - `aaa 0`：约 0ms，返回 102 条。
  - `aaaa 0`：约 1ms，返回 0 条。
  - `index.js`：约 22ms，返回 20 条。
  - `.dll`：约 3ms，返回 20 条。
  - `metadata.bin`：约 3ms，返回 20 条。
  - `node_modules`：约 1ms，返回 5 条。
  - `Program Files`：约 1ms，返回 20 条。
  - `zzznotfoundzzz 0`：约 1ms，返回 0 条。
  - 打开后查询内存约 21MB private / 25MB working set。

#### 530 万样本 Release 结果

测试命令使用 `cmake-build-codex-release\EzdbBench.exe`，输入为 `test_data\files_5300K.txt`，输出为 `test_data\files_5300K_v3_release.ezdb`。

构建指标：

| 指标 | 数值 |
| --- | ---: |
| 记录数 | 5,299,514 |
| 原始 txt | 777,734,595 bytes / 741.71 MB |
| ezdb 文件 | 576,650,570 bytes / 549.94 MB |
| 构建耗时 | 66,941 ms / 66.94s |
| 构建峰值内存 | 4,613.01 MB |
| 构建结束 working set | 6.88 MB |
| 构建结束 private | 1.43 MB |

section 体积：

| section | bytes | MB |
| --- | ---: | ---: |
| records | 169,584,448 | 161.73 |
| dirs | 12,336,800 | 11.77 |
| names | 57,250,211 | 54.60 |
| index | 1,695,168 | 1.62 |
| postings | 335,783,791 | 320.23 |

打开数据库后的常驻内存：

| 指标 | 数值 |
| --- | ---: |
| info working set | 236.23 MB |
| info private | 231.27 MB |

查询性能：

| 关键词 | open_ms | search_ms | returned | private MB |
| --- | ---: | ---: | ---: | ---: |
| `a` | 65 | 648 | 5,238,437 | 232.30 |
| `aa` | 80 | 23 | 126,107 | 231.49 |
| `aaa` | 66 | 5 | 4,056 | 231.30 |
| `aaaa` | 72 | 6 | 272 | 231.29 |
| `index.js` | 73 | 54 | 20 | 232.35 |
| `.dll` | 62 | 3 | 20 | 232.26 |
| `metadata.bin` | 72 | 15 | 20 | 231.59 |
| `node_modules` | 89 | 11 | 5 | 231.47 |
| `Program Files` | 60 | 3 | 20 | 231.35 |
| `设计` | 71 | 3 | 3 | 231.27 |
| `方案` | 66 | 3 | 18 | 231.27 |
| `zzznotfoundzzz` | 65 | 3 | 0 | 231.28 |

结论：

- Release 下常见英文、目录词、不存在词和中文词查询均远低于 300ms。
- `a limit=0` 返回 523 万条，耗时 648ms，瓶颈主要是大结果集枚举、路径重建和 callback，而不是索引定位。
- 实际 UI 场景应默认分页或设置 limit；极高频短词的全量返回需要单独优化，例如只计数、分页游标、bitmap/range 扫描或延迟构造完整路径。

## 9. C API 设计

当前公开接口位于 `src/ezdb/ezdb.h`。

核心接口：

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
```

预留写接口：

```c
int ezdb_insert(Ezdb* db, const EzdbFileRecord* record, uint32_t* out_id);
int ezdb_update(Ezdb* db, uint32_t id, const EzdbFileRecord* record);
int ezdb_delete(Ezdb* db, uint32_t id);
```

错误处理：

```c
const char* ezdb_error_message(int code);
```

API 设计原则：

- C ABI 简单稳定，方便后续被 C++ UI 或其他工具调用。
- 返回值使用整数错误码。
- 查询结果中的字符串由 `ezdb` 分配，调用方必须用 `ezdb_free_result` 释放。
- 搜索使用 callback，避免一次性分配巨大结果数组。

## 10. 编码与兼容性

- 输入 txt 按 UTF-8 读取。
- `.ezdb` 内部路径字符串按 UTF-8 保存。
- 查询关键词应以 UTF-8 传入。`EzdbBench` 在 Windows 下已使用宽字符命令行转 UTF-8，避免中文参数被本地代码页破坏。
- 当前索引按 UTF-8 token 建 1/2/3-gram，因此中文路径可以被索引搜索。
- 当前大小写折叠只处理 ASCII，适合英文路径、扩展名和 Windows 常见程序路径。
- 非 ASCII 路径可保存、返回和精确搜索，但大小写不敏感搜索暂不完整。
- 后续如果需要完整 Unicode casefold，需要引入更复杂的 Unicode 规范化表或在索引阶段保存额外 normalized key。

## 11. 已知限制

- 当前 v3 还没有真实持久化增删改。
- v3 不兼容 v1/v2 `.ezdb` 文件，旧文件需要重新 build。
- 文件记录固定 32 字节，空间仍有较大压缩空间。
- 目录索引基于路径组件，不为任意跨组件子串单独建索引。
- 高频 1 字节关键词可能返回接近全库，耗时主要由候选确认和结果回调决定。
- 没有崩溃恢复、校验和、compact、并发读写控制。
- 530 万 Release 基准已完成；后续仍需继续压缩 records/postings 并降低构建峰值内存。

## 12. 后续迭代路线

### 阶段 1：可运行原型

- 完成 `.ezdb` 文件格式。
- 完成 txt 导入。
- 完成 info/get/search。
- 完成 100k 和 500k 样本验证。

当前已基本完成。

### 阶段 2：全量性能验证

- 使用 Release 构建运行 500 万级样本。
- 记录 build 时间、文件体积、打开时间、搜索时间。
- 对比 txt、SQLite 或现有 EveryZip 查询路径。
- 根据真实瓶颈决定优先优化 records、names、postings 或高频短词搜索。

当前已完成 `files_5300K.txt` 的 Release 基准。下一步重点是空间压缩和高频短词全量返回优化。

### 阶段 3：增删改与 compact

- 实现 append-only delta log。
- 实现 insert/update/delete。
- 实现 compact 重写。
- 增加崩溃恢复和校验机制。

### 阶段 4：空间和内存优化

- records 分列/varint 化。
- names 去重或块压缩。
- 目录 component trie。
- postings 分块和 mmap。
- 常用目录/name 缓存。

### 阶段 5：EveryZip 集成

- 在不破坏现有 SQLite 流程的前提下增加实验开关。
- 先用于文件路径搜索，再评估是否替换部分 SQLite 查询。
- 增加 UI 层搜索耗时统计和回退策略。
