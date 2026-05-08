# File Tracker Test Program

## 功能说明

这是一个使用 SQLite3 数据库和 Windows USN Journal 技术实现的文件跟踪系统测试程序。

### 主要功能

1. **SQLite3 数据库存储** - 使用 SQLite3 数据库存储所有文件属性
2. **完整文件扫描** - 能够保存当前系统磁盘中所有文件的属性
3. **USN Journal 快速查询** - 使用 Windows USN (Update Sequence Number) Journal 快速查询所有文件属性
4. **增量更新** - 每次打开程序自动检查变化的文件并更新数据库（效率优先）

### 存储的文件属性

- 驱动器盘符 (drive_letter)
- 文件引用号 (file_ref_number) - NTFS 唯一标识
- 文件名 (file_name)
- 文件大小 (file_size)
- 创建时间 (create_time)
- 修改时间 (modify_time)
- 文件属性 (attributes)
- USN 序列号 (usn) - 用于增量更新

### 编译方法

```bash
# 配置 CMake
cmake -B build -G "Visual Studio 17 2022" -A x64

# 编译 TestFileTracker
cmake --build build --config Release --target TestFileTracker

# 编译 QueryDB (查询工具)
cmake --build build --config Release --target QueryDB
```

### 运行方法

**重要**: 程序需要管理员权限才能访问 USN Journal。

#### 方法 1: 使用批处理文件
```bash
# 右键点击 run_test.bat，选择"以管理员身份运行"
run_test.bat
```

#### 方法 2: 直接运行
```bash
# 以管理员身份打开 PowerShell 或命令提示符
cd C:\Users\123\Desktop\EveryZip
.\build\test\Release\TestFileTracker.exe
```

#### 查询数据库
```bash
# 不需要管理员权限
.\build\test\Release\QueryDB.exe
```

### 工作原理

#### 初次运行（完整扫描）
1. 检测数据库是否为空
2. 如果为空，执行完整扫描（LowUsn = 0）
3. 遍历所有 NTFS 驱动器的 USN Journal
4. 将所有文件信息存入数据库

#### 后续运行（增量更新）
1. 从数据库读取每个驱动器的最大 USN 值
2. 只查询 USN > 上次最大值的记录
3. 处理文件变更：
   - 新建/修改：插入或更新记录
   - 删除：从数据库删除记录
4. 大幅提升效率（只处理变化的文件）

### 性能特点

- **初次扫描**: 扫描全部文件，建立完整索引
- **增量更新**: 只处理变化的文件，速度极快
- **批量事务**: 使用数据库事务批量提交，提高写入效率
- **索引优化**: 在驱动器、USN、文件名上建立索引，加速查询

### 测试结果

测试环境成功扫描了 **2,635,316** 个文件：
- C: 盘: 1,241,071 个文件
- D: 盘: 1,394,245 个文件

数据库文件: `file_tracker.db`

### 技术要点

1. **USN Journal**: Windows NTFS 文件系统的变更日志，记录所有文件操作
2. **FILE_ID_DESCRIPTOR**: 通过文件引用号直接打开文件，无需路径
3. **SeBackupPrivilege**: 启用备份权限，访问受保护的文件
4. **SQLite3**: 轻量级嵌入式数据库，支持事务和索引

### 注意事项

1. 只支持 NTFS 文件系统
2. 需要管理员权限运行
3. 非 NTFS 驱动器会被自动跳过
4. 数据库文件保存在程序运行目录
