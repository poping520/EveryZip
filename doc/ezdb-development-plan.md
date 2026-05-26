# ezdb 自定义路径索引数据库需求与设计

## 1. 背景与目标

EveryZip 当前使用 SQLite 保存归档文件和归档内部条目。SQLite 通用性强，但对于“只保存大量文件路径并做快速路径搜索”的场景，存在额外表结构、B-tree、事务和 SQL 执行开销，数据库文件体积也不容易做到极致紧凑。

`ezdb` 的目标是设计一个专门面向文件路径索引的自定义数据库格式：

- 文件扩展名为 `.ezdb`。
- 核心库使用 C 语言实现，能在 Windows 上编译运行。
- 保存文件路径记录：`path`、`size`、`modified_time`。
- 支持常规增删改查能力。
- 支持千万级别路径数据的快速搜索，热启动查询目标约 300ms 以内，越快越好。
- 当前 v2 以“任意关键词查询速度”为最高优先级，接受比 v1 更大的索引文件和中等运行时内存。
- 路径字符串仍复用重复目录前缀，但打开数据库时会加载搜索所需字符串池，避免热查询阶段大量随机读。
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
| records          |
+------------------+
| dirs             |
+------------------+
| names            |
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

- magic：固定为 `EZDB0002`，用于识别文件类型。
- version：格式版本号。v2 不兼容 v1，旧 `.ezdb` 文件需要重新构建。
- record_count：记录总数。
- active_count：未删除记录数。
- dir_count：目录字符串数量。
- 各 section 的 offset 和 size。
- reserved offset/size，用于未来扩展。

文件头必须保持可演进：

- 新版本可以在尾部增加字段。
- 旧版本读取器遇到未知 section 时可以跳过。
- 修改破坏兼容性的结构时必须提升 version。

### 3.2 records section

当前 v2 仍使用固定长度记录：

```c
typedef struct EzdbDiskRecord {
    uint32_t dir_id;
    uint32_t name_offset;
    uint32_t name_len;
    uint32_t flags;
    uint64_t size;
    uint64_t modified_time;
} EzdbDiskRecord;
```

优点：

- 按 id 随机访问非常快。
- 可以用 `id * sizeof(record)` 直接定位。
- 实现简单，便于第一阶段验证。

缺点：

- 每条记录固定 32 字节，千万级数据会占用约 320MB。
- 后续可改为 varint record 或分列存储以进一步压缩。

### 3.3 dirs section

`dirs` 保存目录字符串池。

每个目录项格式：

```text
uint32_t len
char text[len + 1]
```

记录中只保存 `dir_id`。打开数据库时加载目录 offset 数组和完整 `dirs` 字节块，搜索阶段直接从内存取目录字符串。

### 3.4 names section

`names` 保存文件名字符串池。

当前 v2 按追加方式保存：

```text
name\0name\0name\0...
```

记录中保存：

- `name_offset`
- `name_len`

当前文件名没有做去重，因为不同目录下同名文件很多，但文件名一般较短；第一阶段先优先控制实现复杂度。后续如果 `names` 成为空间瓶颈，可以增加文件名去重或块压缩。

### 3.5 index/postings section

`index` 保存 n-gram 到倒排表位置的映射：

```c
typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint64_t offset;
} EzdbDiskIndex;
```

`key` 高 8 bit 保存 gram 类型：

- `1`: 1 字节 gram，用于 1 字节关键词。
- `2`: 2 字节 gram，用于 2 字节关键词。
- `3`: 3 字节 gram，用于 3 字节及以上关键词。

低 24 bit 保存 ASCII casefold 后的 gram 字节。`postings` 保存 gram 对应的 record id 列表。record id 按递增顺序保存，并使用 delta varint 编码：

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

### 4.2 n-gram 倒排索引

对完整路径生成连续 1/2/3 字节 gram：

```text
index.js -> 1-gram: i, n, d, ...
index.js -> 2-gram: in, nd, de, ...
index.js -> 3-gram: ind, nde, dex, ex., x.j, .js
```

查询时按关键词长度选择 gram 宽度：

- 长度 1：使用 1-gram。
- 长度 2：使用 2-gram。
- 长度 >= 3：使用 3-gram。

然后读取每个 gram 的 postings，按 postings 数量从小到大对 record id 求交集，得到候选记录。

优点：

- 支持任意子串搜索，不局限于前缀。
- 查询多个 gram 求交集后候选集通常很小。
- 对文件名、扩展名、目录片段、普通短词统一走索引。

缺点：

- 完整路径 1/2/3-gram 倒排表明显增大 `.ezdb` 文件体积。
- 极高频短词，例如 `a`，即使走索引也可能返回接近全库的结果，耗时主要来自结果确认和回调。

### 4.3 v2 的速度优先策略

v1 为节省空间只对文件名建 3-gram，导致 `aaa`、`aaaa`、`Program Files` 等目录/普通词会降级扫描完整路径。500k 样本中：

- `aaa 0` 约 11.2 秒。
- `aaaa 0` 约 10.2 秒。

v2 删除“文件名关键词 / 目录关键词”的启发式分流：

- 对完整路径建 1/2/3-gram。
- 构建时对同一路径内重复 gram 去重，避免 postings 膨胀。
- 查询时对关键词 gram 去重，并按 postings `count` 从小到大求交。
- 任一 gram 不存在时立即返回空结果。
- 最终仍做完整路径子串确认，避免 n-gram 假阳性。

这个策略牺牲文件体积，换取任意关键词不再进入全表扫描。

### 4.4 查询流程

```text
输入 keyword
  |
  +-- 按长度生成 1/2/3-gram key
  +-- 查 index，任一 key 不存在则返回空
  +-- 按 postings count 排序
  +-- 读取 postings 并求交集
  +-- 对候选完整路径做子串确认
  +-- 只为最终结果构造 path 并 callback
```

## 5. CRUD 设计

### 5.1 当前状态

当前 v2 已实现：

- build：从 txt 构建 `.ezdb`。
- open/close：打开和关闭数据库。
- info/stats：读取记录数、文件大小和 section 大小。
- get by id：按 id 读取记录并重建完整路径。
- search：路径子串搜索。

当前 v2 暂未真正实现：

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

v2 的原则是热查询不做全表扫描，也不在候选确认阶段频繁随机读文件。

打开数据库时当前加载：

- records 固定记录区。
- index gram 映射表。
- dirs offset 数组。
- dirs 字符串池字节块。
- names 字符串池字节块。

按需读取：

- postings 列表。

优点：

- 查询阶段目录和文件名字符串都来自内存。
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
- postings：完整路径 1/2/3-gram 倒排表。
- index：gram 到 postings 的映射。

500k 样本当前结果：

```text
files_500k.txt  : 60,188,257 bytes
files_500k_v2.ezdb : 134,873,425 bytes

records_size  : 16,000,000 bytes
dirs_size     :  5,613,358 bytes
names_size    :  9,391,354 bytes
index_size    :    636,944 bytes
postings_size : 103,231,641 bytes
```

下一阶段空间优化优先级：

1. 压缩 records：把 `dir_id`、`name_offset`、`name_len`、flags、size、mtime 分列或 varint 化。
2. postings 分块或引入 bitmap/Roaring bitmap，优化高频 gram 的体积和交集速度。
3. 压缩 names：按块压缩或对重复文件名去重。
4. 增加可选压缩 section：对冷数据区使用 zlib/zstd 类压缩，读取时按块解压。
5. 目录路径分层复用：把目录拆为 path component trie，而不是保存完整目录字符串。

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
- v2 Debug 构建 500k 建库约 87.4 秒，`.ezdb` 约 134.9 MB。
- 500k v2 Debug 热查询样例：
  - `aaa 0`：约 0ms，返回 102 条。
  - `aaaa 0`：约 1ms，返回 0 条。
  - `index.js`：约 56ms，返回 20 条。
  - `.dll`：约 4ms，返回 20 条。
  - `metadata.bin`：约 25ms，返回 20 条。
  - `node_modules`：约 77ms，返回 5 条。
  - `Program Files`：约 107ms，返回 20 条。
  - `zzznotfoundzzz 0`：约 0ms，返回 0 条。
  - `a 0`：约 325ms，返回 497,196 条。
  - `aa 0`：约 8ms，返回 7,551 条。

这些数字是当前开发机和 Debug 构建下的阶段性结果，后续需要用 Release 构建和 `files_all.txt` 做最终判断。

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
- 当前大小写折叠只处理 ASCII，适合英文路径、扩展名和 Windows 常见程序路径。
- 非 ASCII 路径仍可保存和返回，但大小写不敏感搜索暂不完整。
- 后续如果需要完整 Unicode casefold，需要引入更复杂的 Unicode 规范化表或在索引阶段保存额外 normalized key。

## 11. 已知限制

- 当前 v2 还没有真实持久化增删改。
- v2 不兼容 v1 `.ezdb` 文件，旧文件需要重新 build。
- records 固定 32 字节，空间仍有较大压缩空间。
- 完整路径 1/2/3-gram 索引显著增大文件体积。
- 高频 1 字节关键词可能返回接近全库，耗时主要由候选确认和结果回调决定。
- 没有崩溃恢复、校验和、compact、并发读写控制。
- 还没有 Release 构建和 500 万全量样本的完整基准数据。

## 12. 后续迭代路线

### 阶段 1：可运行原型

- 完成 `.ezdb` 文件格式。
- 完成 txt 导入。
- 完成 info/get/search。
- 完成 100k 和 500k 样本验证。

当前已基本完成。

### 阶段 2：全量性能验证

- 使用 Release 构建运行 `files_all.txt`。
- 记录 build 时间、文件体积、打开时间、搜索时间。
- 对比 txt、SQLite 或现有 EveryZip 查询路径。
- 根据真实瓶颈决定优先优化 records、names、postings 或高频短词搜索。

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
