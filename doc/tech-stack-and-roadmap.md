# EveryZip 技术栈、核心技术与开发路线

## 项目定位

EveryZip 是一个 Windows 桌面工具，目标是用接近 Everything 的体验，快速检索本机归档文件内部条目。当前实现重点是：快速发现本机 ZIP/APK/7z/RAR 等归档文件，解析内部文件列表，写入本地 SQLite 索引，并在 Win32 ListView 中低延迟检索和浏览。

## 技术栈

### 语言与构建

- C++17：主程序、索引器、解析器、数据库访问、Win32 UI 均使用 C++。
- C：SQLite amalgamation、zlib/minizip 源码以 C 代码集成。
- CMake 3.20+：统一构建主程序、测试工具、zlib 和 minizip 静态库。
- MSVC / Visual Studio 2022：当前 CMake 示例面向 Visual Studio 17 2022；MSVC 下启用 `/utf-8`。

### 桌面与系统 API

- Win32 API：主窗口、消息循环、菜单、托盘、剪贴板、文件属性、Shell 操作。
- Common Controls v6：ListView、StatusBar、进度/Spinner 等控件。
- Shell API：系统小图标、打开文件、打开所在目录、属性对话框、目录选择等。
- NTFS USN Journal：用于全盘快速枚举和后台增量监控。
- UAC Manifest：程序请求管理员权限，以访问 USN Journal；同时声明 PerMonitor DPI 感知。

### 存储与索引

- SQLite：嵌入式本地数据库，数据库文件为程序目录下的 `everyzip.db`。
- 当前主要表：
  - `archives`：归档文件元数据，包括盘符、文件引用号、父引用号、USN、文件名、路径、大小、修改时间。
  - `entries`：归档内部条目，包括归档 ID、内部路径、压缩大小、原始大小。
  - `configs`：保存每个盘符的 USN Journal 位置等运行状态。
- SQLite 优化：
  - WAL 模式。
  - `synchronous=NORMAL`。
  - 内存临时表。
  - mmap。
  - 批量事务写入。
  - 针对盘符、USN、文件名、路径、归档 ID 建索引。

### 归档解析

- zlib + minizip：用于 ZIP/APK 条目枚举和单条目解压。
- 7-Zip C SDK：用于 7z 条目枚举和单条目解压，源码静态编译进主程序，不依赖 7z DLL。
- RARLAB UnRAR 源码：用于 RAR 条目枚举和单条目解压，源码静态编译进主程序，不依赖 `unrar.dll`；许可限制为只能处理 RAR，不能用于实现 RAR 压缩器或重建压缩算法。
- 解析接口抽象：`IArchiveParser` 定义 `Open`、`ListEntries`、`ExtractEntry` 等通用能力。
- `ZipArchiveParser`：当前已接入主程序的 ZIP/APK 解析器。
- `SevenZipArchiveParser`：当前已接入主程序的 7z 解析器；solid archive 多文件共享压缩块时，压缩后大小列显示 `-`。
- `RarArchiveParser`：当前已接入主程序的 RAR 解析器；加密、缺卷或超大字典场景会失败并记录日志。
- libarchive 路线已放弃：它在 list 模式下无法可靠获取条目的压缩后大小，不满足当前索引和列表展示需求。

### UI 与交互

- Win32 原生窗口。
- `LVS_OWNERDATA` 虚拟 ListView：只维护 rowid 列表，可见行按需读取。
- LRU 行缓存：缓存近期显示行，避免滚动时频繁读取完整数据。
- 系统图标缓存：按文件扩展名获取系统小图标。
- 搜索框 debounce：输入后延迟刷新，减少数据库查询抖动。
- 列头排序：通过 SQLite `ORDER BY` 重新查询 rowid 列表。
- 关键词加粗高亮：自绘 ListView 文本，对匹配片段使用粗体。
- 系统托盘：关闭窗口时最小化到托盘，托盘菜单可显示窗口或退出。

### 配置

- 独立配置文件：程序目录下 `everyzip.cfg`。
- 自研 `AdvConfig::Parser`：支持布尔、整数、浮点、字符串、列表、字典、注释、序列化。
- 用户配置封装：`UserConfig` 当前主要管理归档扩展名列表。
- 默认归档扩展名：`.zip`、`.apk`、`.7z`、`.rar`。

### 测试与辅助工具

- `TestZip`：验证 ZIP/APK 条目解析。
- `TestSevenZip`：验证 7z 条目解析和单条目解压。
- `TestRar`：验证 RAR 条目解析和单条目解压。
- `TestUsnScan`、`TestMftScan`：验证 USN/MFT 扫描能力。
- `TestFileTracker`：早期文件追踪/USN/SQLite 验证工具。
- `QueryDB`：数据库查询辅助工具。
- `TestConfig`：配置解析器单元测试。

## 核心技术设计

### 快速文件发现：USN/MFT 枚举

项目没有递归遍历目录树，而是对 NTFS 卷使用 `FSCTL_ENUM_USN_DATA` 枚举 MFT/USN 数据。这样可以直接获取全盘文件记录，并按扩展名筛选归档文件。

关键点：

- 只扫描 NTFS 盘。
- 使用 `GetLogicalDriveStringsW` 获取盘符。
- 使用 `GetVolumeInformationW` 过滤 NTFS。
- 使用 `CreateFileW("\\\\.\\X:")` 打开卷。
- 使用 `FSCTL_QUERY_USN_JOURNAL` 获取 Journal 上界。
- 使用 `FSCTL_ENUM_USN_DATA` 枚举记录。
- 命中扩展名后，通过 `OpenFileById` + `GetFinalPathNameByHandleW` 补齐完整路径、大小和修改时间。

### 增量更新：USN Journal 监控

初始索引完成后，程序保存每个盘符的 `journal_id` 和 `next_usn`。后台线程每约 2 秒读取一次 USN Journal 增量。

处理策略：

- 新建/修改：根据 FileReferenceNumber 获取文件信息，写入或更新 `archives`，重新解析条目。
- 删除/重命名旧名：按盘符 + FileReferenceNumber 删除归档记录和关联条目。
- 重命名/路径变化：先删除旧路径关联条目，再写入新路径并重新解析。
- Journal ID 变化：当前增量读取会失败并提示需要全量重扫，后续可完善自动重建。

### 索引写入：元数据与条目分离

索引分为两层：

- `archives` 记录归档文件本身。
- `entries` 记录归档内部文件条目。

这种结构便于：

- 按归档文件变化重建其条目。
- 搜索时只查内部条目并 JOIN 归档路径。
- 删除或重命名归档时快速清理关联条目。

### 查询体验：虚拟列表 + rowid 缓存

ListView 不直接持有所有行的完整文本，而是：

1. 后台查询匹配条件下的 `entries.id` 列表。
2. UI 设置虚拟列表总行数。
3. `LVN_GETDISPINFO` 到来时，根据 rowid 按需查询单行。
4. `RowCache` 维护 LRU 缓存，减少重复查询。

这个设计降低了大量条目时的内存占用，也避免初次刷新时把完整结果集搬到 UI 内存中。

### 解析器抽象

`IArchiveParser` 提供统一接口，当前主线按扩展名分发到 ZIP/APK、7z、RAR 专用解析器。这个抽象仍然保留给后续多解析器调度使用，但候选解析方案必须能在列表模式下提供条目的压缩后大小。

当前现实状态：

- ZIP/APK：已实际走 minizip。
- 7z：已实际走 7-Zip C SDK，单文件静态集成；只有单文件压缩块显示压缩后大小，solid 多文件共享块显示 `-`。
- RAR：已实际走 RARLAB UnRAR 源码，单文件静态集成；不携带 `unrar.dll`。
- tar、tar.gz 等：仍是长期目标，但不再采用 libarchive，需要评估其他解析库或格式专用解析器。

## 开发路线

### 阶段 1：稳定 ZIP/APK MVP

目标是把当前主线能力打磨为可日常使用的版本。

- 稳定全盘初始扫描、增量监控和数据库重建。
- 修复资源文件/中文字符串显示和编码问题。
- 完善 ZIP 条目路径编码处理。
- 完善右键菜单行为：打开归档、打开所在目录、复制名称/路径、属性、解压。
- 明确双击行为，例如打开归档、定位条目或解压预览。
- 补充错误提示和索引状态展示。

### 阶段 2：可配置化

目标是让用户能控制扫描范围和格式。

- 配置界面接入 `UserConfig`。
- 支持自定义归档扩展名。
- 支持排除目录、排除盘符、排除隐藏/系统位置。
- 支持手动重建索引。
- 支持清理数据库、Vacuum 和索引维护。

### 阶段 3：多格式归档

目标是从 ZIP/APK 扩展到常见归档格式。

- 评估非 libarchive 的解析库，或按格式逐步引入专用解析器。
- 实现按扩展名或探测结果选择解析器。
- 候选方案必须能在列表模式获取条目的压缩后大小。
- 逐步支持 tar、tar.gz、gz 等格式。
- 对不同格式的压缩大小、时间戳、编码差异做兼容处理。

### 阶段 4：搜索能力增强

目标是从简单 LIKE 过滤升级为更像 Everything 的体验。

- 支持多关键词、空格 AND 查询。
- 支持路径/文件名字段限定。
- 支持大小、扩展名、归档类型过滤。
- 评估 SQLite FTS5 或自建 token 索引。
- 优化排序表达式和大结果集响应。

### 阶段 5：后台服务与可靠性

目标是让索引长期可靠运行。

- 处理 Journal ID 改变后的自动全量重扫。
- 处理数据库繁忙、写入失败、归档损坏、权限不足等异常。
- 引入索引任务队列，避免大量文件变化时阻塞监控循环。
- 增加日志轮转和诊断信息。
- 支持开机自启。

### 阶段 6：产品化体验

目标是形成完整桌面产品。

- 自定义应用图标和安装包。
- 设置页、关于页、更新检查实现。
- 国际化资源整理。
- 快捷键和键盘导航。
- 更完整的测试覆盖与发布流程。

## 主要风险与注意事项

- USN Journal 依赖 NTFS 和管理员权限，非 NTFS 盘不会被扫描。
- 当前主线实际支持 ZIP/APK/7z/RAR，其中 7z 的 solid archive 条目没有可靠逐文件压缩大小，界面显示 `-`。
- 多格式解析库必须能在列表模式提供条目的 compressed size，否则不能用于索引展示。
- UnRAR 源码许可允许处理 RAR，但禁止用于实现 RAR 压缩器或重建 RAR 压缩算法。
- 归档内部路径编码复杂，尤其是旧 ZIP 使用本地代码页时，需要继续验证。
- 当前搜索基于 SQLite `LIKE`，大规模数据下可能需要 FTS 或更强索引策略。
- UI 和后台线程通过消息传递，异步对象生命周期需要保持严格约束。
- 数据库是可重建索引库，PRAGMA 设置偏向性能，需接受异常退出后的重建策略。
