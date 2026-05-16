# 搜索能力增强规划

## 当前搜索基础

当前程序的搜索主要基于 SQLite `LIKE` 子串匹配。

- 归档内部条目搜索走 `Database::QueryEntryIds`，条件是 `entries.entry_path LIKE '%' || ? || '%'`。
- 条目总数统计走 `Database::GetEntryCount`，使用同样的 `entry_path LIKE` 条件。
- `Database::QueryEntries` 是完整条目查询接口，也按 `entries.entry_path` 做子串匹配。
- `Database::QueryArchives` 搜索归档文件路径，条件是 `archives.file_path LIKE '%' || ? || '%'`。
- `Database::QueryEntryById` 不参与搜索解析，只按 rowid 延迟取单条记录，服务于虚拟列表。
- UI 搜索框当前只传递一个纯文本 `filter`，列表高亮也按纯文本子串查找。

这意味着现阶段的搜索语义是：输入关键字后，只要关键字出现在归档内路径或归档文件路径中就匹配。当前没有独立的搜索表达式解析层，也没有 FTS 表；`archives.file_path` 有普通索引，`entries.entry_path` 目前没有专门索引。

## 第一阶段：通配符

先支持最常用、最容易与现有架构兼容的通配符：

| 操作符 | 含义 | 示例 |
| --- | --- | --- |
| `*` | 匹配 0 个或多个字符 | `*.txt` |
| `?` | 匹配 1 个字符 | `file?.png` |

实现方式：

- 保持普通输入的兼容语义，`abc` 仍表示包含 `abc`。
- 将用户输入转换成 SQLite `LIKE` pattern。
- `*` 转换为 `%`，`?` 转换为 `_`。
- 对用户输入中的 `%`、`_`、`\` 做转义，避免误触发 SQLite LIKE 自身通配符。
- SQL 使用 `LIKE ? ESCAPE '\'`，所有查询入口绑定同一套转换后的 pattern。

难度：低。数据库查询函数当前都接收 `std::wstring filter`，可以不改变外部接口。

风险点：

- 查询性能仍然是 `LIKE '%...%'` 的扫描型特征，通配符不会改善性能。
- UI 高亮当前只能高亮纯文本子串，第一阶段可以先保持现状，后续再让高亮理解通配符匹配范围。

## 第二阶段：常用搜索操作符

可以继续补充这些操作符：

| 操作符 | 示例 | 含义 | 难度 |
| --- | --- | --- | --- |
| 空格 AND | `foo bar` | 同时包含多个关键词 | 中 |
| `OR` | `foo OR bar` | 任一关键词匹配 | 中 |
| `-` / `NOT` | `foo -bar` | 包含 `foo` 且排除 `bar` | 中 |
| `"` | `"read me.txt"` | 连续短语匹配 | 低到中 |
| `name:` | `name:readme` | 只匹配文件名 | 中 |
| `path:` | `path:src/*` | 匹配完整归档内路径 | 中 |
| `archive:` | `archive:backup` | 匹配所在归档文件路径 | 中到高 |
| `ext:` | `ext:txt` | 按扩展名过滤 | 中 |
| `size:` | `size:>10MB` | 按大小过滤 | 中 |
| `mtime:` | `mtime:>=2025-01-01` | 按修改时间过滤 | 中 |
| `/regex/` | `/foo\d+\.txt/` | 正则表达式 | 高 |

第二阶段不建议继续把逻辑散落在多个 SQL 字符串里，应新增轻量搜索解析层，例如：

- `SearchQuery`：保存解析后的 token、字段、操作符。
- `SearchSqlBuilder`：生成 `WHERE` 子句、参数列表、是否需要 JOIN。
- 高亮信息：提供 UI 可使用的匹配片段或降级为普通文本高亮。

## 第三阶段：性能与索引

当数据量变大、复杂搜索增多后，再评估性能方案：

- 给 `entries.entry_path` 增加普通索引，只能改善部分前缀匹配，对 `%keyword%` 帮助有限。
- SQLite FTS5 适合分词/全文查询，但文件路径搜索需要额外设计 tokenizer 或 path 分段策略。
- trigram/ngram 辅助表更适合路径子串搜索，但会增加索引体积和写入复杂度。
- 正则搜索通常难以直接利用 SQLite 索引，适合作为高级功能或二次过滤。

建议路线是先保持小步落地：第一阶段完成 `*` / `?`，第二阶段再抽解析层，第三阶段基于真实搜索耗时决定是否引入专门索引。
