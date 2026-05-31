# EveryZip

快速检索本机归档文件（ZIP/7z/RAR）内部条目的桌面工具，类似 Everything 的体验。

## 功能

- **USN Journal 全盘扫描** — 利用 NTFS USN Journal 高速枚举所有归档文件，无需逐目录遍历
- **增量监控** — 初始扫描后持续监听文件系统变化，自动同步新增/修改/删除的归档文件
- **实时搜索** — 输入关键词即时过滤，支持按文件名和内部路径匹配
- **纯虚拟列表** — 仅加载可见行数据，36 万条目秒开，内存占用极低
- **关键词高亮** — 搜索匹配部分粗体显示
- **列排序** — 点击列头按名称/路径/大小排序（数据库 ORDER BY）
- **系统托盘** — 关闭窗口最小化到托盘，后台持续监控

## 构建

```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
```

> 需要管理员权限运行（USN Journal 访问）。

## 测试数据生成

仓库提供了一个独立 Python 工具，用于生成真实 ZIP 测试数据：

```powershell
python .\tools\generate_zip_test_data.py `
  --output-root G:\EveryZipTestData `
  --zip-count 15000 `
  --entry-count 5300000 `
  --clean `
  --compact-names `
  --report-json G:\EveryZipTestData\report.json
```

- `--compact-names` 适合大规模数据，能显著降低 ZIP 名称元数据带来的空间开销。
- 默认会把中文名称比例控制在约 `30%`。
- 建议先用较小样本试跑，例如 `--zip-count 300 --entry-count 100000`，确认目标机器磁盘空间足够。
- 仅校验已有数据时可加 `--verify-only`。

## 技术栈

- **语言**: C++17 / Win32 API
- **数据库**: SQLite（嵌入式，单文件 `everyzip.db`）
- **归档解析**: minizip（ZIP）+ 7-Zip C SDK（7z）+ RARLAB UnRAR 源码（RAR），第三方解析器均静态编译进单文件程序
- **UI**: Win32 ListView（虚拟列表模式）+ 状态栏 + 系统托盘

> 7z 的 solid archive 没有可靠的逐文件压缩后大小；这类条目的压缩大小列显示为 `-`。
> RAR 支持基于 RARLAB 官方 UnRAR 源码，仅用于处理 RAR 归档，不用于实现 RAR 压缩器；程序不携带 `unrar.dll`。

## 配置

程序目录下的 `everyzip.cfg` 当前包含测试用扫描盘符开关：

- `scan_drives = ["G"]`：只扫描和监控 G 盘。
- `scan_drives = []`：扫描和监控所有 NTFS 盘。

## TODO List

- [ ] 数据库优化：UTF-16 -> UTF-8 （进行中）
- [ ] 自定义应用图标（替换默认 Windows 图标）
- [ ] ListView 条目右键菜单（打开文件、复制路径、定位归档文件等）
- [ ] 程序配置 — 路径排除（跳过指定目录不扫描）
- [ ] 程序配置 — 自定义归档格式扩展名
- [ ] 支持更多归档格式（tar.gz 等；不采用 libarchive，需选择能提供压缩后大小的方案）
- [ ] 开机自启动选项


## 许可证

MIT
