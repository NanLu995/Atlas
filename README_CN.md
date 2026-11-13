# Atlas Engine

> 状态：当前项目入口说明。更细的阶段完成度以
> [`docs/roadmap/README.md`](docs/roadmap/README.md) 为准。
>
Atlas 是一个基于 **C++20** 与 **C# (.NET)** 的分布式 MMO 服务器框架。它采用类似 BigWorld 的 Login、Base、Cell、Database 与 Manager 多进程拆分，同时通过嵌入式 CoreCLR 运行托管玩法逻辑。

**[English](README.md)**

## Atlas 为什么存在

Atlas 面向大型、长线在线世界：服务器需要跨进程扩展，模拟权威需要留在服务端，玩法团队仍然需要用 C# 高效迭代。C++ 核心负责网络、进程协调、持久化、空间模拟与性能分析；C# 层负责实体行为、共享玩法契约、生成式客户端/服务端类型和示例玩法逻辑。

## 当前能力

- 通过 `machined` 做服务发现的多进程集群布局。
- Base/Cell 实体模型、Mailbox RPC 与生成式 C# 实体代码。
- CellApp 空间模拟，包括基于 Witness 的 AoI、Ghost 实体、BSP 分区与 offload 协调。
- 面向服务端玩法脚本的嵌入式 .NET 运行时。
- C# 客户端运行时，提供桌面与 Unity 集成表面；UE MVP 另有 C++ 插件 /
  客户端接入路径。
- 运行时加载的 MySQL/MariaDB 及显式选择的 SQLite 数据库插件。
- DBAppMgr range 分片、worker 重建式 manager HA，以及对 CellAppMgr /
  BaseAppMgr / DBAppMgr 的 Reviver 监督。
- Tracy instrumentation、基线压测驱动与 trace 对比工具。

主要 MySQL/MariaDB 后端已实现。DB 副本、shard auto-rebalance 和完整 session resume
不属于当前生产基线。

## MVP Unity 示例

`samples/mvp/` 下的 MVP 示例展示了从登录到 Base/Cell 交接、基于 AoI 的实体流式同步、Unity 客户端渲染、带本地预测的服务端权威移动、MovementCommand 冲刺、服务端权威投射物伤害、NPC AI、HP 属性复制、UI Toolkit 登录/HUD 和本地集群工具的完整闭环。

```bash
# Linux / macOS / Git Bash
tools/bin/build.sh debug
tools/bin/setup_mvp_unity.sh
tools/bin/run_mvp_cluster.sh

# Windows
tools\bin\build.bat debug
tools\bin\setup_mvp_unity.bat
tools\bin\run_mvp_cluster.bat
```

在 Unity Hub 中打开 `samples/mvp/UnityClient` 后点击 Play。客户端代码拆成 `Assets/Scripts/Runtime` 与 `Assets/Scripts/Editor` 两个 asmdef，Runtime 下按 app、camera、world、views、UI、input、projectiles、debug overlays 模块归类。需要打包 standalone player 时运行 `tools\bin\build_mvp_unity.bat`；脚本会按 `ProjectSettings/ProjectVersion.txt` 解析 Unity Editor，也可用 `--unity` / `UNITY_EXE` 覆盖编辑器路径。

![Atlas MVP Unity 示例](docs/mvp/mvp_unity.gif)

## 架构

```text
Client
  │
  ▼
LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
   │                         │             │
   └──────────────┐          │             ▼
                  ▼          ▼         CellAppMgr
                DBAppMgr ─► DBApp
                              │
                   MySQL/MariaDB / SQLite
```

| 进程 | 职责 |
|---|---|
| `machined` | 本机服务注册、心跳目标与 Birth/Death 通知中心。 |
| `Reviver` | 监督 manager 进程，并通过 worker 重建恢复拉起新实例。 |
| `LoginApp` | 面向客户端的登录网关。 |
| `BaseAppMgr` | BaseApp 注册、选择与集群协调。 |
| `BaseApp` | Base 实体所有权、客户端代理状态、Mailbox 路由、持久化转交与脚本运行。 |
| `CellAppMgr` | CellApp 注册、空间几何、BSP 分区与 offload 协调。 |
| `CellApp` | 空间实体、移动、Witness 更新、AoI、Ghost 与脚本运行。 |
| `DBAppMgr` | DBApp 注册、range shard table 权威与 shard table 恢复。 |
| `DBApp` | 通过配置的数据库后端异步持久化实体。 |

服务器进程共享一套小型框架层级：

```text
ServerApp
├── ManagerApp     不带 CoreCLR 的守护/管理进程
└── ScriptApp      ServerApp 加嵌入式 CoreCLR
    └── EntityApp  ScriptApp 加实体定义与后台任务
```

`ServerApp` 提供配置、machined 注册、watcher 指标、tick 回调与信号分发。`BaseApp` 和 `CellApp` 通过 `EntityApp` 扩展它，使 C# 玩法运行在 C++ 服务器核心之上。

## 环境要求

Windows：

- Visual Studio 2026（推荐）或 Visual Studio 2022 17.14+，安装 Desktop development with C++ 工作负载；所有生成器均通过 .NET SDK CLI 构建托管项目
- CMake 3.28+
- .NET 10 SDK
- Python 3.9+
- Git

Linux：

- GCC 13+ 或支持 C++20 的 Clang
- CMake 3.28+
- debug 预设需要 Ninja
- OpenSSL 3.0+ Crypto 开发文件（Ubuntu 包名 `libssl-dev`）
- .NET SDK 必须满足 `global.json`（10.0.202 或更新的稳定 feature band）；
  Ubuntu 26.04 提供的 10.0.1xx SDK 不满足此要求。
- Python 3.9+
- Git

Windows 构建包装脚本会自动加载 MSVC 环境。debug 预设需要 Ninja 时，包装脚本会自动准备。

CMake 优先从 PATH 查找，缺失时使用所选 Visual Studio 安装中的 CMake。

认证拒绝空用户名和空密码。会话密钥使用操作系统安全随机源，生成失败时拒绝登录。
公网传输加密和服务端密码派生尚未实现。

## 构建

日常开发优先使用包装脚本：

```bash
# Windows
tools\bin\build.bat debug
tools\bin\build.bat release
tools\bin\build.bat profile

# Linux / macOS / Git Bash
tools/bin/build.sh debug
tools/bin/build.sh release
tools/bin/build.sh profile
```

常用选项：

```bash
tools\bin\build.bat debug --clean
tools\bin\build.bat debug --config-only
tools\bin\build.bat debug --build-only

tools\bin\build.bat debug --build-dir build/verify-windows
```

也支持直接调用 CMake：

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

| 预设 | 用途 |
|---|---|
| `debug` | 快速迭代，使用 Ninja Multi-Config、Debug、MSVC `/Z7`、PCH 与测试。 |
| `release` | 优化构建，禁用测试。 |
| `hybrid` | RelWithDebInfo，用于带符号的优化调试。 |
| `profile` | RelWithDebInfo，启用 Tracy 与 profiling helpers，禁用测试。 |
| `vs` | 在 `build/vs/` 生成 Visual Studio 解决方案，默认 Debug，包含全部测试（仅 Windows）。 |

支持的平台还提供 sanitizer 预设：`asan`、`asan-msvc`、`tsan`、`ubsan`。

### 在 Windows 下通过 WSL 构建与测试 Linux 版本

在同一份 Windows clone 上验证 Linux 构建——无需第二份 checkout、无需手动同步。源码就地从 `/mnt/<drive>/...` 读取；构建产物写到 WSL 文件系统的 `~/atlas-builds/<preset>`，避免跨文件系统写入拖慢编译。检测到 `sccache` 时自动启用，与 CI 一致。

前置条件（管理员 PowerShell，仅一次）：`wsl --install -d Ubuntu-26.04`。

```bash
# 一次性安装工具链（g++-13、ninja、.NET 10、sccache、clang-format 等）
tools\bin\setup_linux.bat

# 构建（语义与 build.bat 对齐）
tools\bin\build_linux.bat
tools\bin\build_linux.bat release
tools\bin\build_linux.bat --clean

# 构建 + ctest
tools\bin\build_linux.bat --with-tests
tools\bin\build_linux.bat --with-tests --label "unit|integration"
```

需要换 build 目录时通过 `ATLAS_LINUX_BUILD_DIR` 覆盖默认路径。

## 测试

```bash
ctest --test-dir build/debug -C Debug --label-regex unit --output-on-failure
ctest --test-dir build/debug -C Debug --label-regex integration --output-on-failure
ctest --test-dir build/debug -C Debug --label-regex managed --output-on-failure
ctest --test-dir build/debug -C Debug -R test_math --output-on-failure
```

C# 测试：

`managed` 标签串行执行 Runtime、Generators、Client 测试，并从当前构建输出加载原生库。
构建和测试应使用同一个目录，隔离构建不得加载其他预设中的 DLL。
Windows 多进程测试使用测试程序同目录的服务器和已部署运行时，CTest 提供运行时配置；
缺失产物直接失败。两平台构建使用 database factory 的目标时，都会构建所有已启用的
数据库插件；Windows 还会将 DLL 部署到程序同目录。
登录、CellApp 及 Manager/Reviver 进程测试目前仅支持 Windows，
Linux 下的明确跳过不代表 Linux 进程恢复已验证。
`AtomicReplaceFile` 执行同卷路径替换，不承诺崩溃后的数据持久性。
Windows 对既有目标使用 `ReplaceFileW`，保留目标元数据，允许共享删除权限的读者继续读取旧文件。
调用前须关闭源文件的所有句柄；访问被阻止时明确失败，不自动重试。
目标不存在时使用 `MoveFileExW`；Linux 保持 rename 语义。
Linux 包装脚本的 `--with-tests` 默认执行 unit、integration、managed 三组；
可用 `--label` 缩小验证范围。

```bash
dotnet test tests/csharp/Atlas.Runtime.Tests/Atlas.Runtime.Tests.csproj --configuration Debug
dotnet test tests/csharp/Atlas.Generators.Tests/Atlas.Generators.Tests.csproj --configuration Debug
dotnet test tests/csharp/Atlas.Client.Tests/Atlas.Client.Tests.csproj --configuration Debug
```

提交 C++ 修改前，运行单元测试并对改动的 C++ 文件执行 clang-format：

```bash
clang-format --dry-run --Werror <changed files>
```

## 运行本地集群

开发集群启动顺序：

```text
machined → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

在 Linux 或 Git Bash 下，集群包装脚本会通过 0 客户端的 world-stress 驱动启动同一套进程，并保持运行直到被中断：

```bash
tools/bin/run_cluster.sh
```

脚本化集群验证：

```bash
tools/bin/test_cluster.sh
# Windows
tools\bin\test_cluster.bat
```

`test_cluster` 支持 `--base-assembly <脚本.dll>`，用于为 BaseApp 加载测试脚本。
默认仍为 `Atlas.Runtime.dll`；显式程序集不存在时，在启动集群进程前报错。
CMake 的 `atlas_rpc_fixture_base_dll_deploy`、`atlas_rpc_fixture_client_dll_deploy`
目标构建仅用于测试的生成式 RPC 两端。
`--cell-assembly <脚本.dll>` 增加一个 CellAppMgr 和一个 CellApp；配套的
`atlas_rpc_fixture_cell_dll_deploy` 目标构建 Cell 测试端。程序集缺失时在启动进程前报错，
运行清单记录拓扑中是否包含 CellApp。

运行独立的生成式 Mailbox 测试时，将 `ATLAS_BIN_DIR` 设为隔离二进制目录的绝对路径，
通过 `ATLAS_DB_MYSQL_*` 指向空的测试数据库。现有 C# 集群夹具负责进程启动与结束，
不依赖安装 Godot：

```bash
dotnet test tests/csharp/Atlas.Client.IntegrationTests --filter FullyQualifiedName~RpcMailboxRealTests
```

覆盖 Client→Base、Client→Cell 回复，Base→Cell、Cell→Base 调用，Base/Cell→Client
通知，以及异步乱序、业务错误、单向调用、超时、取消、晚回复及清理；先构建三端测试夹具。

## 数据库

Atlas 通过 database factory 加载 SQLite 和 MySQL/MariaDB 插件。主要开发、联调、正式部署
和集群验收使用 MySQL/MariaDB，多个 DBApp 共享同一个逻辑数据库。
`ATLAS_DB_MYSQL` 默认 ON，使用 MariaDB Connector/C 构建插件。SQLite 必须显式选择，
仅用于单 DBApp 轻量开发、快速冒烟和独立测试，支持 WAL 和 busy-timeout。
通过 JSON 的 `database.type` 或 `--db-type` 选择 `sqlite` 或 `mysql`（MariaDB 也使用 `mysql`）。
XML 数据库后端已移除。显式选择 `xml` 或其他不支持的类型时，配置校验和工厂创建均明确失败，
即使程序旁仍有旧插件也不会加载，不会回退到其他后端。现有数据文件不删除，也不提供迁移工具。
通用 XML 解析、序列化、配置数据及实体定义支持保留。

EntityID 经后端原子预留后才发放。MySQL 锁定计数行并等待提交确认，失败不会回退到 ID 1。
崩溃允许留下未使用的区间；已初始化的计数器丢失时，包括重启后也返回错误。
备份必须同时保留计数器及其初始化元数据。

DBApp 分片游标恢复同时查询左闭右开区间内的现存实体、持久操作回执结果及 checkout 版本历史。
删除实体不删除其保留的回执或租约历史，因此这些已确认身份在重启后仍低于恢复的游标。
备份必须包含这三类数据；此机制不承诺恢复被任意删除的元数据，也不将后端原始删除视为 ID 回收。
多个 MySQL DBApp 同时启动时，先锁住已有结构元数据行，再读取初始化标记并建立 EntityID 计数器，
统一锁顺序以避免初始化死锁。

事务提交失败返回结果不确定，不视为成功。SQLite 批量回调等待事务结果。
Checkout 区分实体不存在与数据库错误。注销使用 `kLogOff` 在同次写入中释放 checkout；
保存失败时 BaseApp 保留实体。

BaseApp 写入携带随机 128 位操作标识，路由重试保持不变，回复回传该标识。
MySQL 将请求 SHA-256 摘要及成功结果写入 `atlas_write_receipts`，与数据修改同事务提交。
相同请求在 DBApp 重启后仍返回原 DBID；同一标识对应不同请求时拒绝。
写入或回执失败会一并回滚。请求摘要使用平台 SHA-256 实现；回执不自动过期，
备份必须将其与实体数据一起保留。

共享 MySQL 在事务中递增 checkout 版本，版本历史通过 `atlas_checkout_versions`
保留到实体删除及重启之后，备份也须包含此表。版本耗尽返回失败，不回绕。
写入、checkin 和独立删除核对期望持有者及版本，DBApp 同时核对来源通道。
已 checkout 的实体拒绝无版本写入。Checkin 在数据库确认后才清理本地记录。
取消确认原租约已失效，未知取消请求拒绝，成功缓存回复重放时重新核对数据库所有权。
旧回调及成功写入的重放不能释放新持有者的租约。
`CheckinEntityAck` 回显请求编号、数据库实体和完整持有者。BaseApp 在发送失败、回复丢失
及 DBApp 路由变更后保留等待记录，并核对回复来源和完整请求。最多尝试三次，间隔至少
一秒；耗尽后记录错误并继续阻塞该数据库身份，晚到且匹配的成功回复仍可完成确认。
DBApp 在条件释放成功，或数据库读取确认原租约已失效后，才确认成功。读取失败、原租约
仍有效均不能确认释放；重复或旧 checkin 不会释放新租约。
`baseapp/pending_checkin_count` 包含停止重试的记录。此确认依赖 BaseApp 事件循环运行，
停服排空仍属后续工作。

共享 MySQL 首次保存将创建实体、获取 checkout 和保存回执放在同一事务中。
确认回复携带所有权版本；重放返回原 DBID 和版本。请求摘要绑定创建者地址、AppID
及运行时实体，其他创建者不能复用同一操作标识。

保存或退出写入等待确认期间，新的保存会被拒绝并记录警告。断线退出等待保存成功后
继续，竞争的强制退出请求明确失败。提交不确定、断线或回复超时后保留原操作及数据，
最多重试三次；错误回复及发送失败后的重试至少间隔一秒。结果仍不确定时，阻止该实体
的新持久化及退出销毁。晚到成功可确认原操作；重试耗尽会记录错误，始终未确认则需
重启进程。尚未出现不确定结果时的普通失败正常返回失败。

SQLite 后端持久保存递增的 checkout 版本，并支持核对完整持有者的条件释放。
版本上限为 `INT64_MAX`，耗尽或存储版本损坏时失败；版本历史通过
`atlas_checkout_versions` 保留到删除、重启之后，备份也须包含此表。
提交失败不返回已获取版本或释放成功。带版本的写入、注销及独立删除在修改已有实体的
同一事务内核对全部持有者字段。

所有后端的 DBApp checkout 持有者均绑定来源通道，数据库确认条件释放后才删除本地
租约。取消通过原操作回执解析租约，也支持换一个 DBApp 确认并条件释放。
原租约已经失效时，确认取消不会修改新持有者。回执缺失或请求不匹配时失败，解析不会获取新租约。
旧释放回调不能清除新租约。
每次服务器启动通过操作系统安全随机源生成非零进程实例身份，随机源失败则拒绝启动。
注册、发现、死亡通知和 checkout 消息均携带该身份；DBApp 拒绝缺少身份的请求。
即使地址及 AppID 被复用，旧死亡通知也不能清理新进程的所有权。
`atlas_checkout_processes` 在同一事务中记录进程退役并清理其租约，阻止此身份之后的
checkout 和首次保存，包括其他 DBApp 中已排队的请求。该表须纳入备份，记录不自动过期。
退役失败或提交不确定时保留本地所有权，并在 DBApp 运行期间重试。
仅不需要进程死亡跟踪的直接后端调用可使用实例零。更新后的协议及实体表结构要求
进程使用配套重建产物及新数据库，不提供旧格式迁移。
成功的 checkout 缓存回复重新核对数据库所有权，同一请求标识不能复用
目标或持有者不同的缓存结果。按名称 checkout 的取消使用解析后的数据库 ID。

两个后端的 `WriteEntity`、`DeleteEntity` 和 `CheckoutEntity` 均支持持久操作回执。
SQLite 将请求的 SHA-256 摘要和结果与数据修改一同保存到 `atlas_write_receipts`。
备份必须保留回执，回执不自动过期。
请求摘要使用 [Windows CNG](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthash)
或系统 [OpenSSL EVP 接口](https://docs.openssl.org/3.5/man3/EVP_DigestInit/)。
OpenSSL 3.0+ 使用 [Apache-2.0](https://openssl-library.org/source/license/index.html)，
Linux 配置工具及 CI 安装发行版提供的开发包。
写入重放返回原 DBID 及首次保存时发放的租约，不会再次执行或释放新持有者。checkout 跨 DBApp
重试保留同一随机操作标识，确认原实体和原所有权版本；数据库回执在租约已释放或被替代时返回
`checkout_lease_expired`，不会重新获取。DBApp 也可能先根据本地状态拒绝持有者冲突。
按名称 checkout 的重试仍绑定原实体，即使实体已经改名。
相同操作标识对应不同操作种类、目标、持有者或内容时明确拒绝。回执不可读时返回结果不确定，
不视为回执不存在。
SQLite 的 DBApp 首次保存通过同一事务提交实体和租约，持久化成功后才在回复中返回持有者；
创建持有者无效或租约版本耗尽时不会创建实体。
数据库插件未实现持久操作时返回 `write_operation_not_supported` 或 `checkout_operation_not_supported`；
未实现 checkout 解析时返回 `checkout_resolution_not_supported`。
自动建号以实体类型和账号名的唯一约束确认身份。名称冲突或提交结果不确定时重新查询
账号并校验凭证，不再次创建；只有确认存在且非空凭证匹配的账号可以认证成功，账号不存在
或查询失败时返回内部错误。尚未完成的 checkout 请求也校验目标和持有者，
冲突请求不会覆盖原请求或改变其取消状态。回复携带操作标识及明确的数据库错误种类，
BaseApp 同时匹配预期来源和操作标识，不缓存不确定回复。登录和原生数据库装载保留相同
操作及持有者，最多重试三次，重试至少间隔一秒。原生装载使用显式等待记录，装载期间拒绝
持久化。请求 ID 回绕跳过尚未完成的记录。
结果不确定或装载失败回滚时，BaseApp 等待取消确认。取消请求携带原目标及持有者，
确认回复匹配来源及操作标识。三次取消重试仍未确认时保留停滞记录及已解析账号的保护，
不会按时间过期；匹配的晚到成功回复可重新触发解析。
`baseapp/pending_entity_checkout_count` 和 `baseapp/stalled_checkout_count` 提供等待状态。
始终未确认则需等待数据库确认或重启进程；取消不承诺撤销远端已经执行的工作。
死亡清理尚缺进程实例校验，共享存储故障切换尚未完成验收。
使用新初始化的数据库并重建所有参与进程；
旧格式及混合进程版本没有兼容层，启动时的格式校验也尚未完整。

MySQL CI 使用临时数据库运行后端及真实 DBApp checkout 网络测试。本地共享存储测试
需要完整的 `ATLAS_TEST_MYSQL_*` 设置；缺失时必要的 MySQL 检查明确失败并报告环境阻断。
SQLite 通过不能替代 MySQL 验收。通用 Windows/Linux CI 明确使用 `ATLAS_DB_MYSQL=OFF`
和 SQLite 冒烟范围；独立 MySQL CI 验证数据库及网络契约。
checkout 回复丢失用例启动两个真实 DBApp 子进程，屏蔽成功回复交付后强制终止原 DBApp，
通过另一进程重试，并核对重复取消和过期租约。此用例验证该故障边界，不代表完整 HA 验收通过。
后端故障用例在不改变 TLS 协商的情况下转发连接；独立数据库连接确认实体及回执已经提交，
再截断返回数据。调用方必须报告 `kCommitUnknown`；通过另一后端实例重试及重启后重放，
均须返回原 DBID 和租约，不得再次创建实体。

DBApp 配置示例：

```json
{
  "database": {
    "type": "mysql",
    "mysql_host": "127.0.0.1",
    "mysql_port": 3306,
    "mysql_user": "<数据库用户>",
    "mysql_database": "<隔离开发数据库>"
  }
}
```


通过密钥存储或遮蔽输入，在启动进程的环境中设置 `ATLAS_DB_MYSQL_PASSWORD`，不要把凭据
放入命令、提交的配置或日志。DBApp 在配置密码为空时读取此变量；地址、用户和库名从配置或
CLI 读取。缺少库名/用户/密码、参数无效、插件不可用或连接失败均停止启动，不回退 SQLite。
工具还支持 `ATLAS_DB_MYSQL_HOST`、`PORT`、`USER`、`PASSWORD`、`DATABASE`、`POOL_SIZE`
（均带 `ATLAS_DB_MYSQL_` 前缀），或 `--database-config <本地JSON>`。工具通过子进程环境
传递密码，不将密码写入生成的 JSON。

```bat
rem 默认 MySQL；使用现有隔离开发库和本地配置。
tools\bin\test_cluster.bat --database-config <本地DBApp配置>
tools\bin\run_login_stress.bat --database-config <本地DBApp配置>
tools\bin\run_world_stress.bat --database-config <本地DBApp配置>
rem 显式单 DBApp 快速冒烟，不作为 MySQL 验收。
tools\bin\test_cluster.bat --db-backend sqlite
bin\debug\atlas_dbapp.exe --type dbapp --db-type sqlite --db-sqlite-path .tmp/sqlite-smoke/store.sqlite3
```

Linux 使用同名 `tools/bin/*.sh` 包装和 `bin/debug/atlas_dbapp`，参数相同。SQLite 专用构建
可显式设置 `-DATLAS_DB_MYSQL=OFF`；运行前重建所选输出目录。C# 真实集群测试默认 MySQL，
显式设置 `ATLAS_TEST_DB_BACKEND=sqlite` 才选择冒烟范围。工具输出 `database-run.json`，记录
后端、拓扑和范围，控制台/READY 也标明后端。SQLite 结果标记为 `sqlite_smoke_only`，不能作为
容量或共享存储验收结论。

SQLite 在规范化的本地存档路径旁持有操作系统文件锁，覆盖连接整个生命周期。同一存档的
第二个 Atlas 实例在访问结构和数据前失败；进程退出后锁释放，`.atlas-lock` 文件保留。
仅支持本地文件系统，不能删除或替换活动锁文件；外部数据库维护须先停服。不同 SQLite 文件
保持独立，不支持共享存档、复制或生产 HA。策划配置使用 SQLite 是独立用途；本次不新增配置
系统，也不将配置读取接入实体存档的 `IDatabase`。

## C# 玩法与客户端运行时

托管代码位于 C# 项目树中，并在 `ATLAS_BUILD_CSHARP=ON` 时随 native server 一起构建。`Atlas.Shared` 承载共享契约，`Atlas.Runtime` 将服务端玩法绑定到引擎，`Atlas.Client` 驱动客户端实体状态，`Atlas.Generators.Def` 将实体定义转成强类型代码。

示例玩法项目覆盖 base、client、stress 与 Unity MVP 场景。

Unity 接入会为 Unity 工程准备 Atlas 托管客户端程序集和 native `atlas_net_client` 插件。MVP 包装脚本还会部署 `Atlas.Mvp.Client.dll`；打包脚本会先执行同一套 staging，再调用 Unity batchmode。

```bash
tools/bin/setup_unity_client.sh --unity-project <path>
tools/bin/setup_mvp_unity.sh
tools/bin/build_mvp_unity.sh
# Windows
tools\bin\setup_unity_client.bat --unity-project <path>
tools\bin\setup_mvp_unity.bat
tools\bin\build_mvp_unity.bat
```

打包后的 MVP Player 可以不启动集群，直接运行 native movement predictor 自检：

```bash
./out/mvp-unity/windows/AtlasMvp.exe -batchmode -nographics -atlas-predictor-self-test -logFile out/mvp-unity/predictor-self-test.log
```

## 属性同步与 RPC

实体在 `entity_defs/<Name>.def` 中声明。生成器为服务端与客户端各产出一个强类型 `partial` 类，玩法代码只需要补实现。

```xml
<!-- entity_defs/Avatar.def -->
<entity name="Avatar">
  <properties>
    <property name="hp"    type="int32" scope="all_clients" reliable="true" />
    <property name="gold"  type="int32" scope="base"        persistent="true" />
    <property name="level" type="int32" scope="own_client" />
  </properties>

  <client_methods>
    <method name="ShowDamage">
      <arg name="amount"     type="int32" />
      <arg name="attackerId" type="uint32" />
    </method>
  </client_methods>

  <base_methods>
    <method name="UseItem" exposed="own_client">
      <arg name="itemId" type="int32" />
    </method>
  </base_methods>
</entity>
```

属性的 `scope` 决定哪些观察者能看到写入：`all_clients`、`own_client`、`other_clients`、`cell_public`、`cell_public_and_own`、`base`、`base_and_client`。`persistent="true"` 接入 DBApp 持久化；`reliable="true"` 当前只保留在描述符中，属性 delta 目前全部走可靠通道。

服务端 — 给生成属性赋值会向 scope 内每个观察者发出 delta；`Client.<Method>(...)` 是强类型 stub，自动路由到对应的客户端 RPC。

```csharp
[Entity("Avatar")]
public partial class Avatar : ServerEntity
{
    public partial void UseItem(int itemId)        // exposed="own_client"
    {
        Hp -= 5;                                    // 同步到 all_clients
        Client.ShowDamage(50, EntityId);            // 服务端 → 拥有者客户端
    }
}
```

客户端 — `OnXxxChanged(old, new)` 是生成的变更钩子；客户端方法的 partial 会在服务端推送时被调用。

```csharp
[Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    partial void OnHpChanged(int oldValue, int newValue) { /* 响应同步 */ }

    public partial void ShowDamage(int amount, uint attackerId) { /* 处理推送 */ }
}
```

容器（`list[T]`、`dict[K,V]`、`list[list[int32]]` …）与用户 `<struct>` 类型同样通过 setter 路径以字段级 / op-log 级别 delta 复制；完整覆盖矩阵参见 `entity_defs/StressAvatar.def` 与 `samples/stress/`。

实体 RPC 请求与回复头使用 64 位请求 ID。生成的 ID 在托管运行时内不复用，耗尽时拒绝新调用。
原生 ABI 版本 7 要求运行时、服务器及生成调用代码一起重建，不支持旧 RPC 消息头。
等待注册遇到重复键时明确拒绝，不替换原等待者。回复必须来自注册的通道，重复、晚到及
错误通道的回复不能完成该等待者。取消表示停止等待，并阻止发送前已取消的生成调用；
不承诺撤销远端已经执行的工作。
CellApp 将托管 RPC 等待接入原生注册表。Cell 到 Base、Ghost 到 Real 调用在发送前绑定
实际通道；目标不可用或发送失败会通知等待者。本地 Base/Cell 回复与远端调用使用同一套
原生回复句柄；完成、断线、实体销毁或注册表关闭后使句柄失效，脚本晚完成不会访问
过期通道或再次完成其他调用。ClientSession 按所属会话分发实体回复，生成的客户端 Mailbox
使用该会话等待表；重置或 Desktop 传输断线时以 ReceiverGone 完成等待。
服务器 C# 调用和 BaseApp 中转共用原生进程的 64 位 ID 分配器，脚本重载不重置计数器，
耗尽时拒绝。BaseApp 改写 Client 到 Cell 的请求 ID，回复时恢复客户端原 ID，隔离不同
客户端的同 ID 请求。中转等待在 10 秒后超时；客户端或 Cell 通道断开、损坏回复及发送
失败均清理中转状态。Cell 路由未就绪时，有回复调用立即失败；单向调用保留已有有界队列。
每个原生注册表最多保留 4096 个发起方等待和 4096 个回复上下文；每个托管注册表最多
保留 4096 个等待。完成、取消或清理后释放容量，超限注册失败，不替换原等待者。
BaseApp、CellApp 在结束流程卸载脚本前关闭 RPC 注册表；关闭后拒绝新请求，
包括清理回调中重入的注册。此行为不代表完整的停服任务排空已经实现。
生成的单向 Mailbox 和组件发送方法返回 `RpcSendResult`：`Queued`、`InvalidTarget`、
`Overloaded`、`Stopping`、`SendFailed`、`PartiallyQueued`。已入队只确认本地分发或传输
接纳，不确认远端已执行。广播汇总所有尝试的 BaseApp 和 Ghost 通道；部分入队表示至少
一个通道已接纳且另一个失败，框架不会自动重试。`OtherClients` 无接收者时无需发送即成功。
独立客户端 SDK 的 RPC 发送也传播底层错误，不再无条件报告成功。
独立 Godot/桌面宿主可以向 `LoginClient` 传入 `ClientSession`；它接通该会话的
Base/Cell 发送入口，并将回复和断线清理路由到该会话。每个并行连接使用独立会话，
通过 `DefBootstrap.RegisterInto(session)` 注册生成的客户端定义。无参构造函数沿用
仅供单连接使用的 `DefaultSession`。Dispose 会解除自身的发送委托。
`ClientSession.CellReadyReceived` 分发服务器按实体发送的路由就绪通知；它是当时的状态
通知，不保证后续调用一定成功。原生分发只接纳当前绑定 BaseApp 通道的回复与状态通知，
断线等待清理期间拒绝这些消息。
底层通道断开后，在轮询中完成原生断线清理，并且只通知一次原因码 2；在途登录/认证
回调收到失败，会话 RPC 等待以 `ReceiverGone` 完成。Unity 网络管理器在登出和销毁时
也会清理会话等待。
原生回调内允许断线或销毁上下文：清理等当前原生调用退出后进行；销毁立即停用后续
回调，递归 `AtlasNetPoll` 返回 `BUSY`。已请求断线时，在实际清理前也拒绝新发送。
生成的有回复 Mailbox 同时订阅调用实体的生命周期令牌和显式取消令牌；即使调用方省略
`ct`，实体销毁或脚本热重载也会取消等待。回收等待源时解除两种令牌订阅，避免影响后续调用。
有回复调用中的同步脚本异常、异步回复序列化失败均返回 `RemoteException`，同时消费
原生回复句柄，避免调用方只等超时和持续占用回复上下文。

## Profiling 与压测

`profile` 预设启用 Tracy instrumentation 与 profiling helpers。默认基线为 200 客户端、120 秒，捕获文件写入 `.tmp/prof/baseline/`。

Linux 构建将 Tracy 共享库及其加载用符号链接放在服务器旁，确保托管与原生代码使用
同一份运行库，无需另外修改 `LD_LIBRARY_PATH`。

```bash
tools\bin\build.bat profile
tools\bin\run_baseline_profile.bat
```

```bash
tools/bin/build.sh profile
tools/bin/run_baseline_profile.sh
```

对比 CellApp 捕获：

```bash
tools\bin\compare_tracy.bat ^
  .tmp\prof\baseline\cellapp_<old>.tracy ^
  .tmp\prof\baseline\cellapp_<new>.tracy
```

```bash
tools/bin/compare_tracy.sh \
  .tmp/prof/baseline/cellapp_<old>.tracy \
  .tmp/prof/baseline/cellapp_<new>.tracy
```

更深入的背景可从 `docs/` 下的 profiling、scripting、gameplay、stress-test、Unity 与 coding-style 文档开始。

## 许可证

Atlas 基于 [MIT License](LICENSE) 发布。
