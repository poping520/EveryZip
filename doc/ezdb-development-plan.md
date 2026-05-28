# ezdb 自定义路径索引数据库需求与设计

## 1. 背景与目标

EveryZip 当前使用 SQLite 保存归档文件和归档内部条目。SQLite 通用性强，但对于“只保存大量文件路径并做快速路径搜索”的场景，存在额外表结构、B-tree、事务和 SQL 执行开销，数据库文件体积也不容易做到极致紧凑。

`ezdb` 的目标是设计一个专门面向文件路径索引的自定义数据库格式：

- 文件扩展名为 `.ezdb`。
- 核心库使用 C 语言实现，能在 Windows 上编译运行。
- 保存文件路径记录：`path`、`size`、`modified_time`。
- 支持常规增删改查能力。
- 支持千万级别路径数据的快速搜索，热启动查询目标约 300ms 以内，越快越好。
- 当前 v7 在 v6 的 append-only delta log 基础上增加事务批量写入和 `insert_many`，解决大批量插入时每条记录强制 `_commit` 导致的性能瓶颈，同时保持压缩基座不可变。
- 路径字符串按目录组件和文件名复用，打开数据库时加载列式文件表、目录表、字符串池和索引，热查询阶段只按需读取 postings。
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
| append delta log |
+------------------+
| reserved / ext   |
+------------------+
```

### 3.1 文件头

文件头保存：

- magic：固定为 `EZDB0007`，用于识别文件类型。
- version：格式版本号。v7 不兼容 v1/v2/v3/v4/v5/v6，旧 `.ezdb` 文件需要重新构建。
- base_file_count：压缩基座中的文件记录数。
- file_count：逻辑文件记录总数，包含 delta log 中追加的新 id。
- active_count：未删除记录数。
- dir_count：目录节点数量。
- 各 section 的 offset 和 size。
- delta offset/size：append-only 增删改日志位置和大小。
- reserved offset/size，用于未来扩展。

文件头必须保持可演进：

- 新版本可以在尾部增加字段。
- 旧版本读取器遇到未知 section 时可以跳过。
- 修改破坏兼容性的结构时必须提升 version。

### 3.2 records section

当前 v7 沿用 v5/v6 的 compact file records 和目录记录两个 section。磁盘上 file records、dir records、string pool 可以独立 zlib 压缩；打开数据库时 file records 通过 zlib 流式解码直接写入列式内存数组，目录数组和字符串池仍一次性解压加载，保证 `get/search` 可以按 id 快速访问。

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

磁盘记录仍为 compact varint 编码。打开后的运行时 file columns 为：

```text
parent_dir_id[]        uint32_t
name_offset[]          uint32_t
name_len[]             uint16_t
size32[]               uint32_t，超过 32 位时进入 overflow 表
modified_time32[]      uint32_t，超过 32 位时进入 overflow 表
```

优点：

- 磁盘上的 file records 使用 varint 压缩，显著小于 v3 固定 32 字节记录。
- 打开数据库时不再分配完整解压 records 临时缓冲，而是流式 inflate 并边解码边填充列式数组。
- 运行时不再保存 `EzdbDiskFile[record]` 结构数组，`name_len`、`size`、`modified_time` 按实际取值压缩，530 万样本常驻私有内存从约 233 MB 降到约 163 MB。
- `get/search` 仍按 id O(1) 读取必要列，只有返回结果时才重建完整 path。

缺点：

- records/dirs/names 压缩会增加 cold open 时间，但 file records 流式解码已经显著降低打开峰值内存。
- size/mtime 目前为 32 位主体 + overflow 表，尚未做 block base-delta 或按页懒加载，后续仍有进一步压缩空间。

### 3.3 dirs section

`dir records` 保存目录树节点。每个目录节点保存父目录 id、组件名 offset、组件名长度，以及该目录子树对应的连续 file-id 范围。

构建阶段按目录树 DFS 顺序重排文件记录，使每个目录子树都能用 `first_file_id + file_count` 表示。目录关键词命中目录节点后，可以直接扩展为文件 id range，不需要为目录下每个文件重复写入相同目录 gram。

### 3.4 names section

`string pool` 保存目录组件名和文件名，按完整字符串去重。文件记录和目录记录都只保存 `name_offset + name_len`。

这样既避免重复保存完整目录路径，也减少常见文件名、目录名在不同位置重复出现时的空间浪费。

### 3.5 index/postings section

v6 沿用 v5 的搜索索引结构，拆成两类：

- 文件名 gram 索引：gram -> file id postings。
- 目录组件 gram 索引：gram -> dir id postings。

每类索引都保存 n-gram 到倒排表位置的映射：

```c
typedef struct EzdbDiskIndex {
    uint32_t key;
    uint32_t count;
    uint32_t container_type;
    uint32_t encoded_size;
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

`postings` 保存 gram 对应的 file id 或 dir id 列表。v5 写入时按数据形态自动选择 container：

- array container：低频或稀疏 id 使用 delta varint。
- range container：连续或近连续 id 使用 `start + len` run 编码。
- bitset container：高频稠密 id 使用 bitset。

每个 postings container 会先编码成 array/range/bitset 的原始 payload，再按收益独立选择 zlib 压缩。index entry 保存 `container_type`、`encoded_size`、`raw_size` 和 `offset`，查询时只解压命中的少数 container，不需要加载或解压整个 postings section。

### 3.6 delta log section

v6 在压缩基座之后追加 append-only delta log。基座仍然由 records、dirs、strings、index、postings 组成，一旦 build 完成后不在原地修改；在线写入只追加小型 delta 记录并更新 header。

delta 记录包含：

```c
typedef struct EzdbDeltaDiskHeader {
    uint32_t magic;
    uint32_t type;      /* INSERT / UPDATE / DELETE */
    uint32_t id;
    uint32_t path_len;
    uint64_t size;
    uint64_t modified_time;
} EzdbDeltaDiskHeader;
```

写入顺序：

- 先把 delta record 追加到文件尾部并 `fflush/_commit`。
- 再更新 header 中的 `file_count`、`active_count`、`delta_size`、`reserved_offset` 并再次 `fflush/_commit`。
- 打开数据库时 replay delta log，构建稀疏覆盖层：`id -> 当前版本记录或 tombstone`。

运行时只为 delta 记录保存路径字符串和按 id 查找的小哈希表；基座记录只增加 `active_bits` 和 `covered_base_bits` 两个 bitset，530 万记录约增加 1.3 MB 以内的常驻内存。

这样可以显著压缩倒排表空间，尤其是高频 gram、bitset 和目录子树连续 id 场景，同时保留 gram 级随机访问能力。

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

### 4.3 v5 的空间优先策略

v2 对完整路径建立 1/2/3-gram，导致父目录 gram 被重复写入每个子文件的 postings。500k 样本中 v2 postings 约 103MB，是文件体积膨胀的主要原因。

v3 改为：

- 对文件名建 1/2/3-gram。
- 对目录组件建 1/2/3-gram。
- 文件 id 按目录树 DFS 顺序分配，目录命中可扩展为连续 file-id range。
- 查询时对关键词 gram 去重，并按 postings `count` 从小到大求交。
- 任一 gram 不存在时立即返回空结果。
- 字节长度大于 3 的关键词保留最终完整路径确认，避免 n-gram 或 hash 假阳性。
- 短关键词直接使用 gram 命中结果，减少高频短词的大量路径重建成本。

v4 在此基础上继续优化：

- postings 从单一 delta varint 改为 array/range/bitset 自适应容器。
- file records 从固定 32 字节结构改为 compact varint 磁盘编码。
- 构建索引不再保留全量 `GramPair` 数组，而是按 gram key 聚合 id list，并分阶段构建 file postings 和 dir postings，避免两套 builder 同时占用内存。
- UTF-8 token/key 生成使用栈上小缓冲，减少构建期大量小 malloc。
- 构建 postings 时对 gram key 的哈希桶位做混合散列，并扩大 file/dir builder 桶数，减少高频构建阶段的链表查找冲突。
- postings id list 利用“按递增 id 扫描输入、单条记录内 gram key 已去重”的性质，写出阶段不再重复排序和去重，显著降低 file index 构建耗时。

v5 在 v4 基础上继续优化磁盘空间和运行时内存：

- postings container 级可选 zlib 压缩：每个 gram postings 独立压缩，压缩收益不足时保留原始 payload，避免小 container 膨胀。
- file records、dir records、string pool 以 section 为单位可选 zlib 压缩，header 保存压缩后大小、原始大小和 flags。
- file records 在 open 阶段流式解压并解码为列式内存数组，避免完整 raw records 临时缓冲和 32 字节结构体常驻。
- 查询仍按需读取 postings；dirs/names 在 open 阶段一次性解压到内存，换取热查询阶段无需反复解压。
- 格式升级到 `EZDB0005`，不兼容旧版本。

v6 在 v5 基础上增加持久化 CRUD：

- 格式升级到 `EZDB0006`，不兼容旧版本。
- 压缩基座保持不可变，在线写入进入 append-only delta log。
- `insert/update/delete` 调用成功后立即持久化并刷新到磁盘。
- `get/search` 优先读取 delta 覆盖层，并过滤 deleted/stale 的基座 id。
- 搜索仍以基座 postings 为主；delta 覆盖层数量通常很小，当前使用轻量内存扫描合并结果。

v7 在 v6 基础上增加事务批量写入：

- 格式升级到 `EZDB0007`，不兼容旧版本。
- 新增 `ezdb_begin_write/ezdb_commit_write/ezdb_rollback_write` 和 `ezdb_insert_many`。
- 单条 `insert/update/delete` 仍保持调用返回前持久化；批量事务只在 commit 时统一刷新。
- delta log 增加 batch begin/commit frame；header 只在 commit 后更新，半截事务不会被 replay。
- 批量插入时预分配 active bitset、delta 数组和 delta hash bucket，避免逐条扩容。

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

当前 v7 已实现：

- build：从 txt 构建 `.ezdb`。
- open/close：打开和关闭数据库。
- info/stats：读取记录数、文件大小和 section 大小。
- get by id：按 id 读取记录并重建完整路径。
- search：路径子串搜索。
- insert：分配单调递增 id，追加 INSERT delta。
- update：保持 id 不变，追加 UPDATE delta，并覆盖旧版本。
- delete：追加 DELETE tombstone，查询和搜索均过滤该 id。
- transaction：批量写入期间延迟 flush，commit 时一次性更新 header 并落盘。
- insert_many：批量插入文件记录，避免逐条 `_commit`。

### 5.2 v7 增删改方案

v7 采用 append-only delta log + write transaction：

- 新增记录：追加完整 path、size、modified_time，并增加逻辑 `file_count` 与 `active_count`。
- 更新记录：追加新版本，旧版本标记为 stale；如果更新的是基座 id，则 `covered_base_bits` 标记该基座记录不再参与搜索返回。
- 删除记录：写 tombstone 标记并减少 `active_count`。
- 事务写入：事务开始保存 header、active bitset 和 delta 覆盖层快照，批量写入期间延迟 flush，commit 时一次性落盘，rollback 可恢复内存快照。
- 批量插入：`ezdb_insert_many` 会预分配 active bitset、delta 数组和 delta hash bucket，并在内部事务中写入全部记录。
- 查询时通过 id hash 读取最新 delta 版本，并过滤 deleted/stale 记录。
- 定期 compact：重写主数据区，丢弃 tombstone 和 stale 版本。

这样做的原因：

- 避免在大文件中间频繁移动数据。
- 写入逻辑简单，崩溃恢复更容易设计。
- compact 可以在后台或用户空闲时执行。

后续需要增加：

- `compact` 工具：重建干净数据库文件，吸收 delta log。
- 崩溃恢复增强：校验和、截断不完整 delta。
- delta 记录数量很大时的独立倒排索引或自动 compact 策略。

## 6. 内存设计

v4 的原则是热查询不做原始文本全表扫描，也不在候选确认阶段频繁随机读文件。

打开数据库时当前加载：

- compact file records 流式解码后的列式文件记录数组。
- index gram 映射表。
- 目录记录表。
- 字符串池。
- 文件名索引。
- 目录组件索引。

按需读取：

- postings 列表。

优点：

- 查询阶段目录组件和文件名字符串都来自内存。
- `get/search` 只有在返回结果时才分配完整 path。
- 搜索候选去重使用 bitset，530 万文件只需要约 0.63 MB 临时 `seen` 空间。
- 相比 v1 使用更多索引内存，但 `open_ms` 和 `search_ms` 明显更稳定；v5 运行时列式记录后，530 万样本稳定 private 约 163 MB。

后续可继续优化：

- Windows 下用 `CreateFileMapping` / `MapViewOfFile` 做 mmap。
- postings 分块，避免一次加载超大 postings。
- size/mtime 元数据按页懒加载或 block base-delta 常驻压缩。
- records 改为 mmap 或按页缓存。
- 常用目录字符串增加 LRU 缓存。

## 7. 空间设计

当前主要空间来源：

- records：compact varint 文件记录。
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

v4 已完成的空间优化：

1. file records 从固定 32 字节改为 compact varint 磁盘编码。
2. postings 引入 array/range/bitset 自适应容器。
3. 构建阶段分阶段释放 postings builder，降低峰值内存。

v5 已完成的构建峰值内存优化：

1. DFS 重排完成后立即释放旧 file 顺序数组，避免新旧 `BuildFile` 同时保留到 build 结束。
2. base section 写出后提前释放目录哈希表和字符串去重哈希表，避免与 postings builder 峰值叠加。
3. `BuildFile` 去掉构建后未使用字段，并在 records 写出后释放完整 `BuildFile[]`，只保留 `file_name_offset[]` 给文件名索引阶段使用。
4. postings builder 改为两遍构建：第一遍统计每个 gram 的 postings 数量，第二遍填充到一整块精确大小的连续 `id_block`，减少大量小块 `realloc`、容量翻倍空洞和堆碎片。
5. count/fill 阶段不再对每条记录的 gram key 排序，依赖 builder 侧同一 id 去重，抵消两遍扫描带来的额外 CPU 成本。

下一阶段空间优化优先级：

1. size/mtime 使用 block base-delta 或按页懒加载，进一步降低 records 常驻内存。
2. 对字符串池增加块压缩或按页缓存，降低 names 常驻内存。
3. postings 查询支持更细粒度的懒展开，减少高频短词 `limit > 0` 的候选标记成本。
4. 继续压缩 build 阶段目录树和字符串池辅助结构，目标是在 530 万样本上把 build 峰值内存压到 1GB 左右。

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

#### 530 万样本 v4 Release 结果

测试命令使用 `cmake-build-codex-release\EzdbBench.exe`，输入为 `test_data\files_5300K.txt`，输出为 `test_data\files_5300K_v4_release.ezdb`。

构建指标：

| 指标 | v3 Release | v4 初版 Release | v4 构建优化后 Release |
| --- | ---: | ---: | ---: |
| 记录数 | 5,299,514 | 5,299,514 | 5,299,514 |
| ezdb 文件 | 576,650,570 bytes / 549.94 MB | 348,730,600 bytes / 332.58 MB | 348,730,600 bytes / 332.58 MB |
| 构建耗时 | 66,941 ms / 66.94s | 64,141 ms / 64.14s | 26,238 ms / 26.24s |
| 构建峰值内存 | 4,613.01 MB | 1,841.57 MB | 1,843.72 MB |

构建阶段耗时：

| 阶段 | v4 初版 | v4 构建优化后 |
| --- | ---: | ---: |
| parse/tree | 5,135 ms | 5,112 ms |
| dfs | 56 ms | 48 ms |
| write base | 926 ms | 972 ms |
| file index | 55,347 ms | 18,787 ms |
| dir index | 2,489 ms | 1,173 ms |
| internal total | 64,141 ms | 26,238 ms |

section 体积：

| section | v3 bytes | v4 bytes |
| --- | ---: | ---: |
| records | 169,584,448 | 76,451,208 |
| dirs | 12,336,800 | 12,336,800 |
| names | 57,250,211 | 57,250,211 |
| index | 1,695,168 | 2,542,752 |
| postings | 335,783,791 | 200,149,477 |

查询性能：

| 关键词 | open_ms | search_ms | returned | private MB |
| --- | ---: | ---: | ---: | ---: |
| `a` | 104 | 617 | 5,238,437 | 233.71 |
| `aa` | 106 | 23 | 126,107 | 232.14 |
| `aaa` | 104 | 5 | 4,056 | 232.11 |
| `aaaa` | 103 | 6 | 272 | 232.12 |
| `index.js` | 107 | 32 | 20 | 233.14 |
| `.dll` | 107 | 2 | 20 | 232.42 |
| `metadata.bin` | 109 | 14 | 20 | 232.09 |
| `node_modules` | 109 | 13 | 5 | 232.12 |
| `Program Files` | 109 | 4 | 20 | 232.23 |
| `设计` | 102 | 3 | 3 | 232.09 |
| `方案` | 115 | 4 | 18 | 232.08 |
| `zzznotfoundzzz` | 115 | 5 | 0 | 232.09 |

结论：

- 文件体积从 549.94 MB 降到 332.58 MB，达到 `<400 MB` 目标。
- 构建峰值内存从 4.61 GB 降到 1.84 GB，达到 `1.5GB~2GB` 目标区间。
- 常见词、中文词、不存在词搜索继续稳定在 300ms 内。
- 构建时间从 66.94s 降到 26.24s，已经明显优于 40s 左右的阶段目标。
- 构建加速主要来自 file index：`build_file_index_ms` 从约 55.35s 降到 18.79s。核心原因是减少 postings builder 哈希冲突，并去掉写出阶段对已递增 id list 的重复排序。
- 当前剩余主要成本仍在 file index 和 parse/tree；如果继续追求 20s 以内，可进一步考虑并行 file index 构建、按文件名长度分层的 gram 生成优化，或更连续的 postings id arena。

#### 530 万样本 v5 Release 结果

测试命令使用 `cmake-build-codex-release\EzdbBench.exe`，输入为 `test_data\files_5300K.txt`，输出为 `test_data\files_5300K_v5_release.ezdb`。

构建指标：

| 指标 | v4 构建优化后 Release | v5 Release |
| --- | ---: | ---: |
| 记录数 | 5,299,514 | 5,299,514 |
| ezdb 文件 | 348,730,600 bytes / 332.58 MB | 146,861,276 bytes / 140.06 MB |
| 构建耗时 | 26,238 ms / 26.24s | 27,109 ms / 27.11s |
| 构建峰值内存 | 1,843.72 MB | 1,844.90 MB |

构建阶段耗时：

| 阶段 | v4 构建优化后 | v5 |
| --- | ---: | ---: |
| parse/tree | 5,112 ms | 5,199 ms |
| dfs | 48 ms | 48 ms |
| write base | 972 ms | 1,690 ms |
| file index | 18,787 ms | 18,751 ms |
| dir index | 1,173 ms | 1,274 ms |
| internal total | 26,238 ms | 27,109 ms |

section 体积：

| section | v4 bytes | v5 bytes |
| --- | ---: | ---: |
| records | 76,451,208 | 31,149,103 |
| dirs | 12,336,800 | 4,241,127 |
| names | 57,250,211 | 12,151,677 |
| index | 2,542,752 | 3,390,336 |
| postings | 200,149,477 | 95,928,841 |

v5 section 占比：

| section | MB | 占比 |
| --- | ---: | ---: |
| records | 29.71 | 21.21% |
| dirs | 4.04 | 2.89% |
| names | 11.59 | 8.27% |
| index | 3.23 | 2.31% |
| postings | 91.49 | 65.32% |
| 总计 | 140.06 | 100.00% |

查询性能：

| 关键词 | open_ms | search_ms | returned | working set MB | peak working set MB | private MB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `a` limit=20 | 404 | 29 | 20 | 168.91 | 182.00 | 163.77 |
| `aa` | 409 | 1 | 20 | 167.30 | 175.65 | 162.16 |
| `aaaa` | 406 | 0 | 20 | 167.95 | 175.65 | 162.80 |
| `index.js` | 408 | 17 | 20 | 168.56 | 175.66 | 163.69 |
| `.dll` | 410 | 1 | 20 | 168.25 | 175.64 | 163.16 |
| `metadata.bin` | 395 | 7 | 20 | 167.84 | 248.43 | 163.35 |
| `Program Files` | 409 | 5 | 20 | 167.94 | 175.67 | 162.89 |
| `设计` | 404 | 4 | 3 | 167.96 | 175.65 | 162.80 |
| `zzznotfoundzzz` | 406 | 4 | 0 | 167.96 | 175.66 | 162.82 |
| `a` limit=0 | 410 | 623 | 5,238,437 | 168.94 | 181.99 | 163.81 |

结论：

- 文件体积从 v4 的 332.58 MB 降到 140.06 MB，约减少 57.89%，已经接近 Everything 约 390 万条 / 150 MB 的参考量级。
- postings 仍是最大 section，但已经从 190.88 MB 降到 91.49 MB；records、dirs、names 也通过 section 压缩明显下降。
- 构建时间从 26.24s 变为 27.11s，构建峰值内存基本持平，说明压缩写入没有明显破坏构建性能。
- 热查询 search_ms 仍稳定低于 300ms；`a limit=0` 仍主要受全量返回 523 万结果影响。
- cold open 从约 100ms 增加到约 400ms；file records 改为流式解码后，打开峰值 working set 从约 319 MB 降到约 176-182 MB。
- 稳定查询 private 从 v5 初版约 233 MB 降到约 163 MB，主要来自文件记录列式常驻、`size/mtime` 32 位主体 + overflow 表，以及搜索 `seen` bitset。

#### 530 万样本 v5 build 峰值内存优化结果（当前电脑）

测试命令使用 `cmake-build-codex-release\EzdbBench.exe`，输入为 `test_data\files_5300K.txt`，输出为 `test_data\files_5300K_v5_memopt8.ezdb`。本节数据来自当前电脑；由于 CPU、磁盘和缓存状态不同，耗时不能直接等同于上一节来自另一台电脑的 v5 Release 数据。

构建指标：

| 指标 | 当前电脑 v5 优化前 | 当前电脑 v5 build 内存优化后 |
| --- | ---: | ---: |
| 记录数 | 5,299,514 | 5,299,514 |
| 原始 txt | 777,734,595 bytes / 741.71 MB | 777,734,595 bytes / 741.71 MB |
| ezdb 文件 | 146,861,276 bytes / 140.06 MB | 146,861,276 bytes / 140.06 MB |
| 构建耗时 | 46,409 ms / 46.41s | 35,108 ms / 35.11s |
| 构建峰值内存 | 1,844.48 MB | 1,244.00 MB |
| 构建结束 working set | 9.68 MB | 6.62 MB |
| 构建结束 private | 5.26 MB | 1.80 MB |

构建阶段耗时：

| 阶段 | 优化前 | 优化后 |
| --- | ---: | ---: |
| parse/tree | 9,911 ms | 10,238 ms |
| dfs | 90 ms | 69 ms |
| write base | 2,501 ms | 2,761 ms |
| file index | 31,609 ms | 20,215 ms |
| dir index | 1,975 ms | 1,716 ms |
| internal total | 46,409 ms | 35,108 ms |

section 体积保持不变：

| section | bytes | MB |
| --- | ---: | ---: |
| records | 31,149,103 | 29.71 |
| dirs | 4,241,127 | 4.04 |
| names | 12,151,677 | 11.59 |
| index | 3,390,336 | 3.23 |
| postings | 95,928,841 | 91.49 |

打开数据库后的常驻内存：

| 指标 | 数值 |
| --- | ---: |
| info working set | 166.71 MB |
| info peak working set | 175.05 MB |
| info private | 162.07 MB |

抽查查询性能：

| 关键词 | open_ms | search_ms | returned | private MB |
| --- | ---: | ---: | ---: | ---: |
| `index.js` | 844 | 27 | 20 | 163.58 |
| `设计` | 840 | 7 | 3 | 162.70 |

完整查询矩阵也已验证，常见词、中文词、不存在词仍保持原有热搜索水平。`a limit=0` 仍主要受全量返回 5,238,437 条结果影响。

结论：

- build 峰值内存从 1,844.48 MB 降到 1,244.00 MB，减少约 600 MB，降幅约 32.5%。
- ezdb 文件体积、section 体积和打开后的常驻内存保持不变。
- 当前电脑 build 总耗时从 46.41s 降到 35.11s，没有为了降内存牺牲构建速度。
- 收益主要来自 postings builder 的连续精确 id 存储，以及更早释放构建阶段临时结构。

#### 530 万样本 v6 CRUD 与性能结果（当前电脑）

测试命令使用 `cmake-build-codex-release\EzdbBench.exe`，输入为 `test_data\files_5300K.txt`，输出为 `test_data\files_5300K_v6_crudperf.ezdb`。v6 相比 v5 增加 header 字段和 append-only delta log，压缩基座 section 不变。

构建指标：

| 指标 | 数值 |
| --- | ---: |
| 记录数 | 5,299,514 |
| 原始 txt | 777,734,595 bytes / 741.71 MB |
| ezdb v6 文件 | 146,861,300 bytes / 140.06 MB |
| 构建耗时 | 33,134 ms / 33.13s |
| 构建峰值 working set | 1,244.17 MB |
| 构建结束 working set | 7.11 MB |
| 构建结束 private | 3.04 MB |

构建阶段耗时：

| 阶段 | ms |
| --- | ---: |
| parse/tree | 9,723 |
| dfs | 78 |
| write base | 2,807 |
| file index | 18,950 |
| dir index | 1,443 |
| internal total | 33,134 |

section 体积：

| section | bytes | MB |
| --- | ---: | ---: |
| records | 31,149,103 | 29.71 |
| dirs | 4,241,127 | 4.04 |
| names | 12,151,677 | 11.59 |
| index | 3,390,336 | 3.23 |
| postings | 95,928,841 | 91.49 |

打开数据库后的常驻内存：

| 指标 | 数值 |
| --- | ---: |
| info working set | 167.97 MB |
| info peak working set | 175.69 MB |
| info private | 163.34 MB |

查询性能：

| 关键词 | limit | open_ms | search_ms | returned | private MB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `a` | 20 | 650 | 66 | 20 | 163.33 |
| `aa` | 20 | 664 | 1 | 20 | 163.51 |
| `aaa` | 20 | 620 | 1 | 20 | 163.30 |
| `aaaa` | 20 | 650 | 1 | 20 | 163.97 |
| `index.js` | 20 | 633 | 25 | 20 | 163.31 |
| `.dll` | 20 | 645 | 2 | 20 | 163.35 |
| `metadata.bin` | 20 | 629 | 12 | 20 | 163.33 |
| `node_modules` | 20 | 644 | 13 | 20 | 163.35 |
| `Program Files` | 20 | 666 | 10 | 20 | 164.03 |
| `设计` | 20 | 690 | 8 | 3 | 163.96 |
| `方案` | 20 | 716 | 8 | 18 | 163.98 |
| `zzznotfoundzzz` | 20 | 673 | 13 | 0 | 163.98 |
| `a` | 0 | 661 | 1,336 | 5,238,437 | 163.36 |

CRUD 性能：

| 操作 | 结果 |
| --- | ---: |
| insert 新记录 | 4 ms |
| update 插入记录 | 4 ms |
| update base id=0 | 3 ms |
| delete 插入记录 | 5 ms |
| CRUD 后 records | 5,299,515 |
| CRUD 后 active | 5,299,514 |
| CRUD 后文件大小 | 146,861,547 bytes |
| delta 增长 | 247 bytes |

结论：

- v6 文件体积只比同样数据的 v5 多 24 bytes header 扩展；少量 CRUD 后仅增长 247 bytes。
- `insert/update/delete` 在 530 万样本上均为 3-5ms，且调用成功后立即持久化。
- 常见查询、中文查询和不存在词仍保持 300ms 内；`a limit=0` 主要受全量返回 5,238,437 条结果影响。
- 打开后的 private 内存保持在约 163-164 MB，未因 CRUD 覆盖层明显膨胀。

#### 530 万样本 v7 批量 CRUD 与性能结果（当前电脑）

测试命令使用 `cmake-build-codex-release\EzdbBench.exe`，输入为 `test_data\files_5300K.txt`，基座输出为 `test_data\files_5300K_v7_final.ezdb`，批量插入测试库为 `test_data\files_5300K_v7_insert500k_final.ezdb`。v7 相比 v6 将批量写入从“每条 delta append + header patch + 两次 `_commit`”改为事务批量追加，commit 时一次性更新 header 和 flush。

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

500K 批量插入指标：

| 指标 | 数值 |
| --- | ---: |
| 插入源 | `test_data\files_500k.txt` |
| 解析记录 | 500,000 |
| 跳过记录 | 0 |
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
| 插入后 private | 321.95 MB |
| reopen info private | 250.70 MB |

插入 500K 后查询性能：

| 关键词 | limit | open_ms | search_ms | returned | private MB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `a` | 20 | 546 | 78 | 20 | 252.42 |
| `index.js` | 20 | 532 | 87 | 20 | 252.88 |
| `metadata.bin` | 20 | 557 | 70 | 20 | 251.32 |
| `设计` | 20 | 559 | 70 | 3 | 251.40 |
| `zzznotfoundzzz` | 20 | 547 | 73 | 0 | 251.39 |

正确性抽查：

| id | 结果 |
| ---: | --- |
| 5,299,514 | `C:\$Recycle.Bin\...\$I5ZQDGW.deb` |
| 5,799,513 | `C:\Users\123\.gradle\...\metadata.bin` |

结论：

- v6 逐条插入 500K 会因为每条两次 `_commit` 退化到数十分钟级；v7 事务批量插入 500K 在当前电脑为 1.49s。
- v7 基座文件体积与 v6/v5 压缩基座保持一致；批量插入后的体积增长来自 append-only delta 中保存完整 path。
- 插入 500K 后常见查询、中文查询和不存在词仍稳定在 300ms 内。
- 大规模 delta 会增加打开后的常驻内存，500K delta 后 info private 约 250.70 MB；后续需要 compact 或 delta path 压缩/索引化。

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

写接口：

```c
int ezdb_begin_write(Ezdb* db, uint32_t flags);
int ezdb_commit_write(Ezdb* db);
int ezdb_rollback_write(Ezdb* db);
int ezdb_insert_many(Ezdb* db, const EzdbFileRecord* records, uint32_t count, uint32_t* first_id);

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
- 单条 `insert/update/delete` 成功返回前已经追加 delta log 并刷新到磁盘；事务内写入延迟到 `ezdb_commit_write` 统一刷新；只读打开时返回 `EZDB_ERR_READ_ONLY`。

## 10. 编码与兼容性

- 输入 txt 按 UTF-8 读取。
- `.ezdb` 内部路径字符串按 UTF-8 保存。
- 查询关键词应以 UTF-8 传入。`EzdbBench` 在 Windows 下已使用宽字符命令行转 UTF-8，避免中文参数被本地代码页破坏。
- 当前索引按 UTF-8 token 建 1/2/3-gram，因此中文路径可以被索引搜索。
- 当前大小写折叠只处理 ASCII，适合英文路径、扩展名和 Windows 常见程序路径。
- 非 ASCII 路径可保存、返回和精确搜索，但大小写不敏感搜索暂不完整。
- 后续如果需要完整 Unicode casefold，需要引入更复杂的 Unicode 规范化表或在索引阶段保存额外 normalized key。

## 11. 已知限制

- v7 不兼容 v1/v2/v3/v4/v5/v6 `.ezdb` 文件，旧文件需要重新 build。
- 文件记录已做磁盘压缩和运行时列式常驻，但目录记录、字符串池和索引仍在 open 后整体常驻。
- 目录索引基于路径组件，不为任意跨组件子串单独建索引。
- 高频 1 字节关键词可能返回接近全库，耗时主要由候选确认和结果回调决定。
- v7 单条写入仍按立即持久化处理；批量事务写入 batch begin/commit frame，header 只在 commit 后更新。当前还没有校验和、自动截断不完整 delta、compact、并发读写控制。
- delta 覆盖层已能承受 50 万级批量插入并保持常见查询 300ms 内；更大规模增删改后仍需要 compact 或独立 delta 索引避免 open replay 和 delta 搜索扫描增长。
- 530 万 v7 Release 基准已完成；build 峰值内存保持在当前电脑约 1.24 GB，500K 批量插入约 1.49s。后续重点是 compact、崩溃恢复、进一步降低大 delta 覆盖层常驻内存，并优化高频短词 `limit > 0` 的候选展开。

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

当前已完成 `files_5300K.txt` 的 Release 基准、build 峰值内存优化，以及 v7 批量 CRUD 写入优化。下一步重点是 compact、delta 覆盖层内存压缩，以及高频短词候选展开优化。

### 阶段 3：增删改与 compact

- append-only delta log 已实现。
- insert/update/delete 已实现。
- write transaction 和 insert_many 已实现。
- 实现 compact 重写。
- 增加崩溃恢复和校验机制。

### 阶段 4：空间和内存优化

- records 已完成磁盘 varint 和运行时列式常驻。
- build 阶段已完成 postings builder 连续精确 id 存储、临时哈希表提前释放、`BuildFile` 瘦身和提前释放。
- names 去重或块压缩。
- 目录 component trie。
- postings 分块和 mmap。
- 常用目录/name 缓存。

### 阶段 5：EveryZip 集成

- 在不破坏现有 SQLite 流程的前提下增加实验开关。
- 先用于文件路径搜索，再评估是否替换部分 SQLite 查询。
- 增加 UI 层搜索耗时统计和回退策略。
