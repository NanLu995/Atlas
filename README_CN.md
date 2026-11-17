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

`BgTaskManager` 最多接纳 4,096 项任务，包含排队、执行中及等待主线程回调的结果。
`AddTask` 在满载时返回 `RateLimited`，`StopAccepting` 后返回 `ReceiverGone`；
被拒绝的提交不触发完成回调。工作返回 `Result<void>`，每个已接纳任务在调度线程回报结果。
`CancelPending` 停止接单，并为尚未开始的工作回报 `Cancelled`，不打断已执行的工作。
既有回调异常边界捕获的异常回报 `InternalError`。

停服停止接单及正常模拟 tick，事件循环继续处理已接纳工作和回复；
`ScriptEngine::Drain` / `Lifecycle.OnDrain` 继续推进托管续延，
不执行实体 tick 和属性同步。停服后拒绝脚本新建实体、创建空间及传送请求；已有 Real 的
重复创建请求保留已确认的回复。BaseApp 确认会话释放及持久化登出保存后才销毁实体；
DBApp 排空已接纳操作和回调。默认截止时间为 30 秒。
已知保存失败以失败状态退出；到期记录未确认工作并失败退出，不在 worker 析构处无限等待。
强制退出仅保证已提交数据。按 LoginApp、BaseApp、CellApp/Manager、DBApp、machined 顺序
关闭，依赖方排空期间保留存储及控制服务。集群工具每组等待 35 秒，非零退出或强杀均报失败。

数据库接纳上限为 4,096 项，包含等待消费的回调。满载在执行前拒绝，返回 `RateLimited` /
`kOverloaded`；排空时新工作返回 `ReceiverGone` / `kStopping`。`BeginDrain` 和
`PendingOperations` 提供生命周期接口。checkout 清理及自动加载读写接口均通过 `Result`
回调传递错误，不能把失败解释为空的成功结果。数据库插件须导出 `AtlasCreateDatabaseV5`。
即时可靠 UDP 发送同样遵守发送窗口。每通道、每可靠性类别的延迟 Bundle 上限为
256 KiB，满载时返回 `WouldBlock` 且不追加。RPC 调用区分满载 `RateLimited`、重复请求
`AlreadyExists` 和停服 `ReceiverGone`，被拒绝的请求不发送。
原生登录和认证在发送被拒绝时立即返回忙或传输错误，清理未启动的尝试，允许重新登录，
不等待超时再报错。
`atlas_tool watch` 返回成功前刷新输出；标准输出写入失败时以非零状态退出并向标准错误
报告原因，不把未输出结果误报为成功。
LoginApp 在已接纳登录处理期间暂停客户端的 3 秒空闲计时，结束后恢复；断线仍取消登录，
既有 RPC 和客户端超时保持有效，不新增服务端整体登录截止时间。

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
- 使用下方命令构建的固定 OpenSSL 3.5.9 SDK（需要 Perl 与 make）
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

独立 OpenSSL SDK 工具固定官方 3.5.9 发布包及 SHA-256，构建静态库、执行上游测试，
并在 `build/openssl/3.5.9/Debug` 保留许可证、作者和来源记录。Windows 使用
`tools\bin\build_openssl.bat --config Debug`，Linux x64 使用
`bash tools/bin/build_openssl.sh --config Debug`。Windows 需要原生 Perl 和 NASM，
可通过 `--perl` / `--nasm` 指定现有工具；`--archive` 支持离线构建，`--help` 说明
隔离安装目录与 Release 选项。须先准备 SDK，再配置 Atlas：Debug 构建选择 Debug，
其他构建类型选择 Release；未设置构建类型时选择 Debug。可用 `-DOPENSSL_ROOT_DIR=<SDK 目录>`
指定位置。SDK 缺失或版本不符时配置失败，不回退系统库。构建工具需要 Python 3.11.8+，
产物目录的 `licenses/OpenSSL` 保留许可证与来源。仅构建此 SDK 不会为正在运行的监听端启用加密。

原生 `TlsContext` / `TlsStream` 传输原语固定 TLS 1.3，客户端必须明确提供信任文件和
目标 DNS/IP 身份，并校验证书 SAN。服务端拒绝证书缺失或私钥不匹配；每个流的两个
密文方向各限制为 64 KiB，缓冲区满时返回背压，不完整或篡改的记录不会交给应用读取。
流原语尚未连接 `TcpChannel`；公网 RUDP 使用下述 DTLS 接入。
`test_tls_stream` 使用固定 SDK 生成隔离测试证书，不向操作系统安装信任根。
`TlsTransport::kDatagram` 复用证书契约，使用 DTLS 1.2，并在有界 BIO 队列中保留数据报边界。
密文 MTU 为 1200 字节，握手后通过 `DatagramPayloadMtu()` 获取扣除加密开销的应用上限，
拒绝超大写入及会截断的读取。OpenSSL 丢弃重放或认证失败的记录，后续有效数据报仍可接收。
重传超时接口已提供，可接入事件循环计时器。
原生 `NetworkInterface.ConfigureDtls` 已提供共享 UDP 接入，必须在创建 socket 前配置；
配置后拒绝明文，先解密再解析 RUDP，并加密 RUDP 包头、ACK、分片与重传。
调用方须等待 `IsRudpReady` 后发送，提前发送返回 `WouldBlock`。事件循环驱动握手重传，
最多保留 128 项未完成握手，10 秒到期清理。排空中的通道保留自己的 DTLS 会话，关闭后
不把旧会话复用于新 RUDP 通道。socket 背压保留一个待发密文数据报，BIO 队列仍有界。
未知来源先经过 OpenSSL 无状态 DTLS Cookie 往返，Cookie 使用上下文独立安全随机密钥，
以 HMAC 绑定来源 IPv4、UDP 端口及有效期 60 秒的时间戳。错误、过期或跨上下文 Cookie
不能进入持久握手；未验证请求至多收到一个不大于输入数据报的挑战，不占用持久会话名额。
128 项握手上限在来源地址验证通过后生效。


LoginApp、BaseApp 公网 RUDP 支持 `--external-dtls-certificate <chain.pem>` 与
`--external-dtls-private-key <key.pem>`，JSON 对应 `external_dtls_certificate`、
`external_dtls_private_key`。两项须同时提供，证书错误拒绝启动；`--production true` 或
JSON `is_production: true` 在启用公网监听时强制 DTLS，错误布尔值明确拒绝。
未选择 DTLS 且非生产时可运行明文开发模式。隔离集群工具接受 `--dtls-certificate`、
`--dtls-private-key`，提供后以生产模式启动公网进程。
SDK ABI 3.1 新增 `AtlasNetConfigureDtls`；桌面 C# 在登录前调用
`LoginClient.ConfigureDtls(trustFile, serverIdentity)`。登录、认证及移交等待 DTLS 握手，
显式配置失败后禁止明文登录，取消或断线清理待发工作。分发 SDK 前须构建 CMake 的
`_deploy` 目标并复制配套 DLL，不需要安装系统信任根。
桌面 `AtlasClient` 门面在 `ConnectAsync` 前提供相同的 `ConfigureDtls` 转发方法。
真实集群 C# 集成测试须同时设置四个环境变量以选择加密：`ATLAS_TEST_DTLS_CERTIFICATE`、
`ATLAS_TEST_DTLS_PRIVATE_KEY`、`ATLAS_TEST_DTLS_CA`、`ATLAS_TEST_DTLS_SERVER_IDENTITY`；
缺少任何一项时在启动集群前拒绝。`ATLAS_TEST_BASE_ASSEMBLY` 提供默认夹具程序集，测试默认使用 MySQL。

RUDP 在缓冲或处理 ACK 前拒绝超过通道 MTU 的数据报。分片须携带序号、有效的索引和片数，
且负载非空、不超过 `MaxUdpPayload()`，通过后才记录接收序号；保留现有重组表上限和超时。
可选 `CompressionFilter` 的发送及接收应用负载均限制为 16 MiB；压缩包必须恰好包含一个
完整 zlib 流，实际解压长度须与声明一致，损坏、尾随及超限输入返回错误。
公网监听器默认未安装此压缩过滤器。
公网 Base/Cell RPC 除方向及暴露权限外，还须属于目标实体类型；CellApp 转发后再次校验，
不能借用另一实体类型的公开组件方法，进入目标实体的组件槽位分发。
跨实体 `AllClients` 调用要求接收 CellApp 能确认调用者的有效 Real Witness 已发布目标的 Enter，
且尚未观察到 Leave。缺少权威可见范围、不同空间、Ghost 调用者或不可见目标均拒绝，
有回复 RPC 返回 `ReceiverGone`。尚未实现跨 Cell 可见性授权，不用 Ghost 坐标作为访问凭证；
调用自身实体保留原有行为。

集群 TCP/RUDP 与 machined 管理端口须部署在可信专网，并以部署防火墙规则限制访问；
内部传输未加密。Worker 只接受当前 machined TCP 连接上的查询回复、通知、Watcher 及停服命令。
CellApp 将已登记 BaseApp 的信任绑定到当前通道实例，断线时清理，拒绝旧通道或其他通道的
客户端 RPC/移动转发；machined 进程实例通知使被替换的旧连接失效。
管理工具须通过 machined 转发 Worker 命令，直接向 Worker 发送管理消息会被拒绝。

也支持直接调用 CMake：

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

可在独立构建目录使用 `-DATLAS_BUILD_CSHARP=OFF -DATLAS_BUILD_NET_CLIENT=OFF` 运行原生测试。
托管宿主及热重载测试跟随 C# 构建开关，原生 SDK 测试跟随客户端库开关；TLS/DTLS 传输测试仍保留。
默认构建继续包含全部托管及 SDK 测试。

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
tools/bin/test_cluster.sh --base-assembly <你的Base实体程序集>
# Windows
tools\bin\test_cluster.bat --base-assembly <你的Base实体程序集>
```

`test_cluster` 支持 `--base-assembly <脚本.dll>`，用于为 BaseApp 加载测试脚本。
必须显式提供程序集；缺少或不存在时，在启动集群进程前报错。
CMake 的 `atlas_rpc_fixture_base_dll_deploy`、`atlas_rpc_fixture_client_dll_deploy`
目标构建仅用于测试的生成式 RPC 两端。
`--cell-assembly <脚本.dll>` 增加一个 CellAppMgr 和一个 CellApp；配套的
`atlas_rpc_fixture_cell_dll_deploy` 目标构建 Cell 测试端。程序集缺失时在启动进程前报错，
`--baseapps 2` 为移交验证启动第二个 BaseApp；运行清单及 READY 输出记录实际进程数及端点。
`test_cluster --cellapps 2 --cell-assembly <脚本.dll>` 增加第二个 CellApp，用于恢复验证；
缺少脚本参数或使用不支持的数量时明确拒绝，数据库运行记录和 READY 输出标明实际 Worker 数量。
`--log-level debug` 为本次隔离集群启用既有服务端诊断；C# 夹具可通过
`ATLAS_TEST_CLUSTER_LOG_LEVEL` 传入相同设置，默认仍为 `info`。
测试启动器的标准输入接受 `kill-baseapp <内部端口>` 注入崩溃，并以 `KILLED` 确认终止；
只能终止本次启动器自己的存活 BaseApp 子进程。
`shutdown-baseapp <内部端口>` 通过 machined 请求正常停服，只有进程零退出才确认
`SHUTDOWN`；超时或非零退出使测试启动器失败。
`restart-baseapp <内部端口>` 仅按原参数重启本次启动器已停止的 BaseApp，以 `RESTARTED`
确认创建进程并保留此前日志；测试须另等注册就绪并检查旧会话不能重新取得控制权。
`kill-process <名称>`、`restart-process <名称>` 为本次启动器所属的 CellApp、DBApp、machined
提供相同的崩溃/重启控制；未知名称、重复终止和重启仍存活进程明确拒绝。重启复用原参数并保留日志，
测试须另等注册就绪并验证恢复结果。

运行独立的生成式 Mailbox 测试时，将 `ATLAS_BIN_DIR` 设为隔离二进制目录的绝对路径，
通过 `ATLAS_DB_MYSQL_*` 指向空的测试数据库。现有 C# 集群夹具负责进程启动与结束，
不依赖安装 Godot：

```bash
dotnet test tests/csharp/Atlas.Client.IntegrationTests --filter FullyQualifiedName~RpcMailboxRealTests
dotnet test tests/csharp/Atlas.Client.IntegrationTests --filter FullyQualifiedName~SessionRecoveryRealTests
```

覆盖 Client→Base、Client→Cell 回复，Base→Cell、Cell→Base 调用，Base/Cell→Client
通知，以及异步乱序、业务错误、单向调用、超时、取消、晚回复及清理；先构建三端测试夹具。

Manager 从存活 Worker 的报告重建状态。CellAppMgr 只接受登记的 CellApp 地址及通道发来的空间几何，
拒绝已失效通道和含尾随字节的 BSP，并要求报告者拥有其中的叶节点、所有叶节点持有者均已登记。
被拒绝的报告不会改变当前拓扑。DBAppMgr HA 验证必须确认恢复窗口已关闭，才能判为恢复健康。
CTest 的 `test_ha_verifiers` 合并运行现有 CellAppMgr、BaseAppMgr 及 DBAppMgr 验证器单测；
仅这些检查通过不代表进程崩溃和通信中断场景已经验收。
CellAppMgr 在叶节点持有者重新登记期间保留最高版本的有效几何，满足条件后重新校验来源和持有者再安装。
每个持有者必须在当前登记通道回报相同 BSP、几何版本和非零冻结 epoch；仅所有预期本地 Cell
均存在、处于相同且已解冻的 epoch、SpaceData 已初始化时，报告才标记稳定。
Manager 恢复原版本、epoch 及广播基线，相同拓扑不会凭空增加版本。缺少确认或旧版本确认保持等待，
不稳定或冲突报告暂停该空间；不自动推进中断的拓扑切换，需先核实 Worker 状态再重启暂停的 Manager。
每个 Manager 最多保留 1024 条恢复记录、16 MiB 几何及持有者缓冲；重复报告不增加缓存。
同版本几何冲突、来源或持有者失效、字节预算耗尽时暂停该空间，不降级采用旧报告。
等待或暂停的空间拒绝空间创建/复用、主机解析和拓扑发布，其他健康空间继续运行。
记录槽位耗尽时关闭未知空间准入，即使缓冲释放也不自动重开，因为被拒绝报告的版本水位无法保留。
需先核实 Worker 状态再重启 Manager 清理暂停；单独发送更新报告不会解除冲突。
暂停范围是 Manager 的拓扑决策；既有 Worker tick 与缓存实体路由继续运行。
可查询 `cellappmgr/ha/recovery_pending`、`recovery_bytes`、`recovery_blocked`、
`recovery_admission_closed`；HA 验证器不把待处理、暂停或缺失的恢复状态写为健康。
BSP 解码最多允许 8191 个节点、深度 64，损坏或超限数据明确拒绝。
machined 注册表查询使用 64 位请求 ID，明确区分成功与失败。并发查询互不替换，晚到和重复回复忽略；
每个客户端最多等待 128 项，ID 耗尽后拒绝且不复用。查询超时、断线、发送失败和过载不等于空注册表。
Reviver 不把这类失败计为进程缺失，也不据此冷启动；目标实例版本更新后，旧审计结果失效。
`atlas_tool list` 查询失败时非零退出。machined 注册协议为 3，查询布局已改变，须配套重建
machined、服务器和工具。
machined 断线引起的 watcher 失败不计为 Manager 健康失败。注册重连后，Reviver 先通过成功查询
确认目标在本轮注册表中，再启用依赖注册表的健康/缺失判断；本机进程存活与直接心跳检查继续。
TCP 错误/挂断事件关闭失败连接，
Worker 随后重新登记；分发期间关闭通道会停止处理该 bundle 的剩余消息。
BaseApp 在 BaseAppMgr 出生时携带已有 app_id 重新注册，匹配的死亡通知清理绑定，注册确认只接受
当前通道，避免 Manager 重启后保留失效通道或使用旧确认。
CellApp 在 Manager 死亡或断线后也保留已有 app_id，重注册时回报，避免返回顺序改变 Worker 身份。
Manager 健康探测和回复使用不可靠数据报，以 nonce 匹配；丢包继续由既有周期、超时及失败阈值处理，
晚到回复不能证明当前健康状态。探测不依赖通道重建前的可靠序列状态。

Reviver 只有确认旧本机进程退出后才重新拉起已观察的目标；终止前固定进程身份，启动前再次检查退出。
存活状态读取失败保守视为仍存活。远端或无法确认的目标在 `reviver/<manager>/status` 与
`/restart_blocked` 报告暂停，需运维隔离后恢复。自动监督要求目标位于同机回环地址；
不支持在多台主机为同名 Manager 配置冷启动，不提供跨主机 fencing 或自动搬迁。
Linux 使用稳定 pidfd 终止进程，避免“先检查 PID、再发信号”的复用竞争；不可用时不自动终止。
Windows 使用保留的进程句柄。

公共 Watcher 通过 `app/process_instance`、`machined/connected` 和
`machined/registration_generation` 关联进程实例与登记连接。`network/pending_sends` 统计内部网络
待发送数量。`app/shutdown/state` 区分 `running`、`draining`、`failed`、`timed_out`、`complete`，
同前缀的 `pending_operations`、`remaining_ms` 显示未完成任务和剩余排空时间。
失败时仍可能有未完成任务；排空且完成注销才报告 `complete`。这些值仅为本地观察，不探测数据库可达性。
BaseApp、CellApp、LoginApp 提供 `rpc/pending`、`replies`、`pending_limit`、`reply_limit`、
`oldest_pending_ms`、`stopping`。最长等待时间在查询时计算，无等待项时为零；聚合路径数量不随
实体或请求增加，也不包含请求内容。
`app/ready`、`app/not_ready_reason` 按进程职责报告新路由工作所需的本地条件：Manager 登记、
EntityID 余量、已知数据库路由、队列容量及恢复门禁。原因为空表示就绪；停服始终报告
`shutting_down`。恢复或空间暂停可令 Manager 报告降级，其他空间仍可能正常服务。
这两个字段不改变接单、不发探测、不保证远端数据库实时可达，也不保证具体账号、实体或空间请求成功。
查询 BaseAppMgr 不消耗过载接单名额。debug 级别的 RPC 失败日志包含回复/请求 ID、错误码和耗时，
可与进程日志及现有托管 RPC trace ID 联合排查；不输出 RPC 内容和错误文本。
DBApp 的 `dbapp/` 前缀新增 `pending_operations`、`operation_limit`、`pending_write_requests`、
`account_session_requests`、`process_retirements`、`allocator_recoveries`。
CellApp 提供 `pending_offloads`、`uncertain_offloads`、`pending_teleports`，不确定迁移继续沿用暂停契约。
BaseApp 提供 `incoming_handoffs`、`session_revocations`、`pending_persist`；BaseApp、LoginApp
在各自前缀下提供 `external_pending_sends`。不同生命周期层的计数可能重叠，不能相加当作独立用户请求数。
既有按空间查询的恢复 Watcher 保留。

重复执行框架恢复验收时，通过 `ATLAS_DB_MYSQL_*` 或 `--database-config` 指定隔离 MySQL 库，
先构建 RPC Base/Cell 夹具，再运行 Windows 的
`tools\bin\verify_framework_ha.bat --build framework-verify --scenario all`，或 Linux 的
`bash tools/bin/verify_framework_ha.sh --build framework-verify --scenario all`。
`managers` 验证三个 Manager 重启及重叠故障；`workers` 验证 DBApp、两台 CellApp、machined 崩溃重启。
工具复用既有 HA verifier 和启动器，检查一个双叶空间、两个 Real Probe，逐故障日志保存在
`.tmp/framework-ha-*`。SQLite 或缺少 MySQL 设置时明确拒绝。工具不创建数据库、不删除数据；
初始与最终诊断 JSON 快照验证九个进程均已登记且本地就绪，队列计数非负并且不超过报告的上限。
DBApp 会初始化表结构，须使用空测试库。BaseApp 会话/崩溃验收保留在既有
`ClientHandoffRealTests` 中；本机拓扑不验证跨主机 fencing。

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
多个 MySQL DBApp 同时启动时，使用数据库范围的命名锁串行初始化结构。
两个后端只初始化空库。已有库必须包含匹配的 `schema_version`（MySQL 4、SQLite 3）；
标记缺失或不兼容时拒绝启动，不覆盖版本、不自动升级结构。请重建配套进程并使用新的
隔离数据库；不提供旧格式迁移。SQLite 在一个事务中初始化结构和元数据。
MySQL 建表中断可能留下不完整库，应检查处理，框架不会自动修补或写入兼容标记。
启动还校验 SHA-256 `storage_schema_digest`，覆盖已注册实体字段、组件继承和槽位、
结构体、嵌套容器及其顺序，不包含 RPC 声明。当前存档保存 Base/Cell 完整快照，
未标记 `persistent` 的字段也会落盘，因此一并校验。摘要缺失或不匹配时拒绝启动，
不覆盖已有摘要；修改已注册数据结构后须使用新库。
`blob_format_version` 也必须匹配（当前为 2）。每份实体快照包含明确版本、Base/Cell 段长
和 SHA-256 校验和；截断、篡改、尾随及无版本数据均拒绝。新账号使用编码后的空快照，
显式保存和退出保存采用同一封装，包含最近一次 Cell 备份。此处 SHA-256 用于检测损坏，
不能替代传输认证。
ATDF 定义文件使用版本 4，包含整文件 SHA-256 校验和。定义缺失、为空或损坏时，DBApp 在
打开存储前拒绝启动。登录认证、实体 checkout 和写入携带完整实体定义摘要，DBApp 在访问
数据库前检查；BaseApp 等待 checkout 期间定义发生变化时，也拒绝完成装载。
内联同步组件在实体槽位描述中携带类型名和展平字段，无需分配全局组件 ID。
其字段和嵌套类型参与存档及定义校验；本地组件不进入快照持久化。
账号会话 payload 版本 2 保存恢复凭证摘要、受控实体的持久化身份与过期时间。
签发、消费并轮换、撤销复用所有权版本校验和操作去重事务。过期凭证或换操作ID重放会拒绝，
同一已确认操作重试返回其去重结果。摘要比较复用已有 OpenSSL 常量时间接口。有效期十分钟，由 MySQL 服务端时间或 SQLite
宿主时钟计算，客户端不能指定过期时间；释放会话或改变控制权会撤销凭证。
崩溃恢复接管还要求数据库明确确认旧进程已隔离，新进程持有账号与受控持久实体的当前checkout；
隔离记录缺失或checkout版本失效时拒绝，临时实体不能接管。
原生 SDK ABI 3.2 新增 `AtlasNetGetRecoveryCredential`、`AtlasNetResume` 和 `AtlasNetLogout`；
桌面 C# 提供 `GetRecoveryCredential()`、`ResumeAsync()`、`LogoutAsync()`。
网络断线后在内存保留凭证，向 LoginApp 发起恢复，再按普通登录流程对返回的 BaseApp 认证。
LoginApp 恢复要求非零 `--internal-port` 与注册的集群地址一致，来源地址或进程身份未注册时拒绝。
`test_cluster` 已显式分配该端口，公网 DTLS 端口独立。
恢复成功前会单次消费并轮换凭证，再返回新的连接密钥。原进程存活时复用实体；旧进程确认
隔离后通过 checkout 和既有快照装载恢复持久化身份，运行时 EntityID 可以改变。
临时实体不能在进程崩溃后恢复，也不会夺取其他进程仍持有的有效 checkout。
受控临时实体首次保存后，会用已确认 DBID 重新签发并发送恢复凭证；保存确认不代表该凭证
已送达客户端，恢复时应使用 SDK 最新收到的凭证。
登出成功表示数据库撤销已确认；超时或断线报告结果未确认。`Disconnect(Logout)` 发起同一交换，
需要继续 Poll 等待结果。凭证签发后十分钟过期，读取缓存不续期；禁止写入日志或普通配置。
密码重新认证、登出及受控实体变化会使旧凭证失效。
协议版本 2、存储 schema MySQL 4 / SQLite 3、数据库插件 ABI V5 要求进程配套重建并使用新的隔离数据库，
不提供旧格式迁移。`SessionRecoveryRealTests` 必须使用 MySQL，覆盖在线及崩溃恢复、移交后的控制权、
持久目标装载、临时目标拒绝、凭证轮换和登出撤销；SQLite 冒烟不能替代该验收。

MySQL 连接池在启动后打开连接时重新核对存储 schema、blob 版本和快照摘要。
不匹配时拒绝操作，不初始化或更新该存档；恢复到预期数据库后允许后续重新连接。

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
`baseapp/pending_checkin_count` 包含停止重试的记录。停服期间 BaseApp 保持事件循环运行，
等待这些确认完成或达到停服截止时间。

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
或固定版本的 [OpenSSL EVP 接口](https://docs.openssl.org/3.5/man3/EVP_DigestInit/)。
OpenSSL 3.0+ 使用 [Apache-2.0](https://openssl-library.org/source/license/index.html)，
固定 SDK 与二进制产物保留其许可证及来源记录。
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
MySQL 和 SQLite 还独立于实体 checkout 保存账号会话所有权。带版本的会话操作与操作回执
在同一事务中提交；修改操作内容、退役进程和已被替代的所有权不能复用旧授权。
提交回复丢失返回 `CommitUnknown`；损坏记录直接失败，不视为离线会话。账号存在活动或
交接中的会话时拒绝删除；释放后的会话版本在实体删除后保留。DBApp 校验已注册的 BaseApp
进程实例、配置的账号类型及分片，同时限制最多 1024 个未完成的会话数据库请求。
数据库接口支持本地转移及准备、接纳、客户端确认转换。登录确认会话授权后才绑定客户端；
重新登录和退出先确认释放，再复用或销毁实体。跨 BaseApp 移交只改变客户端控制权，
账号实体的存储位置及 checkout 保持在源端。
内部移交接收端核对已注册的源端和目标进程实例、实体定义及数据库准备记录，才接纳
未绑定的代理。凭证摘要匹配后仍须等待数据库确认目标成为活动持有者，才绑定客户端。
接收中的移交最多 1024 项；20 秒未完成时，先确认中止或释放，再清理预留。
数据库失败时保留预留，每隔 20 秒重试清理。结果不确定的操作在重放被拒绝时仍保留
原操作标识；所有权冲突须确认数据库版本已经推进，才能丢弃原操作。
准备登录前，BaseApp 查询账号会话；已有会话时，联系记录中的持有者确认撤销。
撤销校验已注册的请求进程与持有进程实例、账号及所有权版本，数据库确认后才回复成功。
持有者不可达、版本已改变或提交结果不确定时拒绝本次登录，不推断会话已经离线。
登录检查及接收的撤销请求分别限制为 1024 项；取消登录检查后不能继续 checkout，
取消等待不撤销已经完成的会话撤销。
数据库插件须导出 `AtlasCreateDatabaseV5`，需同时重建两个插件及服务器；
加载器明确拒绝旧接口，避免调用不兼容的虚函数表。
出生及死亡通知只接受当前 machined 通道。即使仅订阅死亡回调，`MachinedClient` 仍跟踪进程实例；
相同地址出现新实例时重建内部 RUDP 通道，旧实例的晚到死亡通知被忽略。
BaseApp 会话查询核对 DBApp 通道、请求进程、
账号及操作标识；已发出的修改在取消等待时返回 `CommitUnknown`，不表示已撤销授权。
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
    "mysql_database": "<隔离开发数据库>",
    "mysql_database_id": "<32位小写十六进制逻辑数据库ID>"
  }
}
```


通过密钥存储或遮蔽输入，在启动进程的环境中设置 `ATLAS_DB_MYSQL_PASSWORD`，不要把凭据
放入命令、提交的配置或日志。DBApp 在配置密码为空时读取此变量；地址、用户和库名从配置或
CLI 读取。缺少库名/用户/密码、参数无效、插件不可用或连接失败均停止启动，不回退 SQLite。
工具还支持 `ATLAS_DB_MYSQL_HOST`、`PORT`、`USER`、`PASSWORD`、`DATABASE`、`DATABASE_ID`、`POOL_SIZE`
（均带 `ATLAS_DB_MYSQL_` 前缀），或 `--database-config <本地JSON>`。工具通过子进程环境
传递密码，不将密码写入生成的 JSON。

`mysql_database_id` 为必填项：用 `tools\bin\database_recovery.bat new-id`（Linux 用对应 `.sh`）
生成一次，所有共享逻辑库的 DBApp 使用同一个 32 位小写十六进制 ID。DBApp 也支持
`--db-mysql-database-id`。仅空库初始化写入该身份；启动及连接池打开新连接时，身份缺失或
不匹配均拒绝。备份、复制及恢复保留该 ID，换地址或恢复时不能重新生成。它标识逻辑数据，
不能证明副本已追平，也不能隔离旧主库。

常规启动拒绝空 MySQL 库。仅首次创建时，显式设置 `database.mysql_initialize` 为 JSON `true`，
或使用 `--db-mysql-initialize true`，初始化后改回 false。启动工具支持
`ATLAS_DB_MYSQL_INITIALIZE=true|false`，默认 false。正式启动、恢复和切换保持 false，
避免误配空库被当作新存储认领。已有库无论是否启用初始化都先校验，不覆盖身份、不升级元数据。

开发恢复工具复用已安装且匹配的 `mariadb`/`mariadb-dump` 或 `mysql`/`mysqldump`；
不在 PATH 时用 `--client`、`--dump-tool` 指定。备份及恢复前停止全部 DBApp 和其他写入者，
`--quiesced` 表示操作者确认此条件，不提供在线备份协调。使用新的私有备份目录及已有空目标库，
源、目标配置使用相同逻辑身份。

```bat
tools\bin\database_recovery.bat backup --database-config <源配置JSON> --directory .tmp/db-backup --quiesced
tools\bin\database_recovery.bat restore --database-config <空目标库配置JSON> --directory .tmp/db-backup --quiesced
tools\bin\database_recovery.bat verify --database-config <目标配置JSON> --directory .tmp/db-backup
```

Linux 使用 `bash tools/bin/database_recovery.sh` 和相同参数。工具保留全部七张框架表，包括
checkout 版本、进程退役、操作回执、会话凭证摘要和 ID 水位，记录 dump SHA-256 及一致性快照的
逐行摘要。非空目标拒绝恢复，不删除失败后的部分数据；失败目标保持隔离，重试选另一个空库。
只导入可信备份。dump 含账号及实体数据，应私有保存并排除出 Git。校验比较存储字节及元数据，
实体装载仍会检查快照内容和定义；原生启动及校验通过前不能开放流量。

生产备份和复制仍由数据库负责。切换前隔离旧主库及全部写入者，确认新库身份和复制位置，
再重启或重连 DBApp 并检查就绪状态。框架不自动选主、不合并分叉数据、不清除 checkout 所有权，
也不自动重试结果未确认的写入。只能恢复成功保存的状态，备份不包含其后发生的事务。

```bat
rem 默认 MySQL；使用现有隔离开发库和本地配置。
tools\bin\test_cluster.bat --database-config <本地DBApp配置> --base-assembly <你的Base实体程序集>
tools\bin\run_login_stress.bat --database-config <本地DBApp配置>
tools\bin\run_world_stress.bat --database-config <本地DBApp配置>
rem 显式单 DBApp 快速冒烟，不作为 MySQL 验收。
tools\bin\test_cluster.bat --db-backend sqlite --base-assembly <你的Base实体程序集>
dotnet build/debug/csharp/Atlas.Tools.DefDump.dll --assembly <你的Base实体程序集> --out .tmp/sqlite-smoke/entity_defs.bin
bin\debug\atlas_dbapp.exe --type dbapp --db-type sqlite --db-sqlite-path .tmp/sqlite-smoke/store.sqlite3 --entitydef-bin-path .tmp/sqlite-smoke/entity_defs.bin
```

Linux 使用同名 `tools/bin/*.sh` 包装和 `bin/debug/atlas_dbapp`，参数相同。SQLite 专用构建
可显式设置 `-DATLAS_DB_MYSQL=OFF`；运行前重建所选输出目录。C# 真实集群测试默认 MySQL，
显式设置 `ATLAS_TEST_DB_BACKEND=sqlite` 才选择冒烟范围。工具输出 `database-run.json`，记录
后端、拓扑和范围，控制台/READY 也标明后端。SQLite 结果标记为 `sqlite_smoke_only`，不能作为
容量或共享存储验收结论。

DBApp 在打开数据库前必须加载包含实体类型和非零定义摘要的 ATDF 文件。集群工具使用配套
构建的 DefDump，从选定的 Base 程序集导出到本次运行目录。`test_cluster` 必须传入
`--base-assembly`；登录及世界压测默认使用压测 Base 程序集，也支持显式覆盖。
客户端摘要来自该导出，不再读取其他构建 `obj` 中的生成源码。DefDump 在加载程序集前检查
离线导出契约，仅在导出进程内跳过生成的原生注册。旧生成程序集须重建。
只导出可信程序集：其他用户编写的模块初始化仍可能执行。

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

实体 RPC 请求与回复头使用 64 位请求 ID。生成的 ID 在服务器进程内不复用，耗尽时拒绝新调用。
原生 ABI 版本 11 要求运行时、服务器及生成调用代码一起重建，不支持旧 RPC 消息头。
CLR 宿主在绑定回调和生命周期前校验 Bootstrap 及服务端生命周期 ABI。
托管服务端和桌面端初始化也在注册前校验已加载引擎 DLL 的 ABI；版本入口缺失或不匹配
时明确报错并停止初始化。

Base 脚本可等待 `PersistToDbAsync(ct)`，返回 `AtlasTask<RpcReply<long>>`，成功值为数据库
确认后的 DBID。确定的数据库拒绝返回失败；提交未确认或 DBApp 断线返回 `OutcomeUnknown`，
原生恢复逻辑继续保留并隔离该写入。取消和超时只停止等待，不撤销已经发出的写入。
`PersistToDb()` 仅发起请求，本地接单失败时抛出异常。

Cell 脚本通过 `AddProximity(range, userArg)` 创建邻近控制器，并重写 `OnProximityEnter` /
`OnProximityLeave`。事件经现有主线程队列在原生范围更新返回后执行，因此回调可以安全取消
控制器。执行前复核原实体及其生命周期，销毁、Ghost 或已取消的实体不接收事件。非法或负
半径返回零，零半径仍受支持；通过 `CancelController(id)` 取消。队列回调异常由现有队列记录。

`StartTimer` 只接受有限的正间隔，拒绝时返回零。定时器通知仍在主线程同步执行；脚本异常
记录到日志，不遗留原生 CLR 错误。Ghost、已销毁或生命周期已取消的实体不执行定时器回调。
热重载在卸载前取消旧实例的原生定时器，脚本在 `OnInit(isReload: true)` 中重新创建。

可选 EDR SDK 的 `AtlasEdrEntityComponentAtSlot` 同时支持具名及内联组件；内联组件的
类型 ID 为零，结构描述属于所在槽位。
每个 EDR Context 只允许一次加载尝试；失败会清空部分定义，重试须新建 Context。成功后
定义保持只读，返回的视图有效至 Context 销毁。
服务端 `NativeApi` 必须在主 tick 线程调用；线程守卫在 Debug 下断言，Release 调用方同样须遵守
该契约。输入 Span 仅在调用期间借用，异步持久化在返回前复制快照。原生快照消费者在下一次
序列化回调或运行时重置可能释放固定缓冲区前完成复制。
`Time.DeltaTime` 读取所属 BaseApp/CellApp 的 `GameClock.FrameDelta` 秒数，包含该时钟的
时间缩放；首个 tick 前、暂停和时钟重置后为零。`Time.ServerTime` 为起点未指定的单调
时钟秒数，不是 UTC 时间戳。
`SpaceMaster.Register` 属于 BaseApp 操作；不支持的 Provider 会明确记录错误。
登录用户名和凭据均须非空，且不能包含嵌入的 NUL 字节。桌面客户端在原生字符串转换前
拒绝这些输入；原生客户端、登录消息解析及 DBApp 也在账号查询或创建前拒绝。
DBApp 在后台线程使用 Argon2id v19（64 MiB、3 次迭代、4 lanes、16 字节随机盐、
32 字节输出）派生账号凭据，两个数据库后端均保存生成的 PHC 记录；旧的原始凭据记录
及不支持的参数无法认证，不提供凭据迁移。客户端继续传入现有登录参数，不能把服务端
存储的 PHC 记录作为登录凭据发送。认证最多处理 2 项完整请求（查询、派生、提交确认），
另有 128 项等待队列；队列满返回 `ServerBusy`，停服以 `ServerNotReady` 拒绝等待者，
并排空在途工作。`dbapp/auth_active`、`dbapp/auth_queued` 可查询数量。
密码存储加固与传输加密独立；公网接入须配置上文的 DTLS 监听参数。
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
包括清理回调中重入的注册。
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

## 实体迁移边界

本地客户端移交使用可等待的 `GiveClientToAsync(destinationId)`，数据库确认所有权变更后
才转移绑定，返回 `RpcReply<bool>`。提交不确定返回 `OutcomeUnknown`；源端保留绑定并冻结
输入，撤销流程确认原操作结果后才释放。取消脚本等待不撤销已经发出的移交。
本地客户端移交拒绝正在销毁的实体、已有客户端的目标及已有预备会话的目标；拒绝时保留
两端原绑定。清除会话同时撤销轮换前的旧密钥；正在销毁的实体不能认证。`CurrentCell`
只允许已注册的来源通道及未过期、无冲突的 epoch 更新路由并排空等待调用。
空间定位回复须匹配传送请求的实体、目标空间和申请时的实体 epoch。一个实体只允许
一个待定位的传送；迁移 epoch 或定位请求 ID 耗尽时拒绝新申请，进程内不复用定位请求 ID。

Cell 迁移 ACK 携带本次尝试的 epoch，并校验目标通道。超时或断线时源端保留被动 Ghost
及回退快照，只有匹配的明确拒绝才恢复源端 Real；晚到的匹配成功仍可完成移交。
Ghost 删除、位置、增量、快照及下一 Real 提示须来自当前 Real 地址及绑定通道；
旧宿主或被替换通道的消息会被忽略。
Ghost 删除、位置、增量和快照还须匹配当前实体 epoch；回到同一宿主后，旧迁移的数据仍被拒绝。
Ghost 创建携带实体迁移 epoch；所有权通知须匹配原宿主、更高 epoch 及新宿主的注册通道。
新宿主通知可先于旧宿主提示到达；晚到提示及重复所有权通知不能回退归属。
每个 Ghost 最多暂存 16 个后续所有权转换，前序宿主到达后按每实体归属链应用，
处理不同通道上的乱序。超限记录错误并拒绝；缺少前序时不猜测宿主。
提升为 Real 时清空暂存，已失效通道的暂存通知不能取得所有权。
迁移被明确拒绝后，源端向 Ghost 重发保留的所有权 epoch。
Ghost 和空间初始化消息布局均已改变，须配套重建全部服务器。
BaseApp 空间初始化明确复用已有主 Cell，或等待同一空间的并发创建；空间实体类型
冲突时拒绝，普通创建请求仍拒绝重名。创建回调匹配 Manager 通道和空间，只完成一次，
发送失败、断线或五秒超时时返回失败。两端待处理空间创建上限均为 1,024；
不可用或请求 ID 耗尽时明确失败。
结果不确定时不会自动接管。控制器状态损坏时拒绝接纳；源端回退恢复失败时保留 Ghost
及快照，不激活部分恢复的 Real。发送未确认时也保留源端被动 Ghost 和快照，
不能因本地发送错误推断目标没有收到分片。同进程传送在目标恢复成功前保留源实体；
拒绝后恢复原 Space 和位置，回退恢复失败则保留 Ghost 和快照，不能计为成功。
接收进程按实体保留最新处理结果，实体销毁后仍保留，防止
重复迁移重新创建已销毁实体。此内存回执每进程最多记录 65,536 个不同实体 ID；达到
上限后，新 ID 的迁移返回 `overloaded`，已记录 ID 仍可继续迁移。回执不是持久化重启
恢复机制。ACK 布局已变更，所有 CellApp 必须配套重建，不支持旧布局。

远端代理使用 `GiveClientToAsync(new ClientHandoffTarget(entityId, baseIp,
internalPort, processInstance))`，提供已注册 BaseApp 的内部 IPv4 数值（`Address::Ip()`）、
端口及当前进程身份；框架不提供全局实体目录。源端冻结输入、准备数据库会话并等待
目标接纳，再将客户端引导至目标注册的外部地址。目标确认认证成功后才释放源端绑定。
拒绝后须确认数据库中止才恢复输入；结果不确定时源端保持冻结。账号实体及其 checkout
保留在源端，不隐式销毁源实体或目标实体。
`TryGetClientHandoffTarget(out target)` 根据注册实例取得当前本地代理的目标描述；
尚未注册或实体正在销毁时返回 false。
Godot 的 `AtlasClientRoundTrip` 验证场景提供移交模式：测试集群使用 `--baseapps 2`，
集群和 Godot 进程的 `ATLAS_RPC_FIXTURE_DIR` 指向同一个空的本机隔离目录。
每个夹具 BaseApp 在该目录发布 Probe 端点，测试明确选择另一进程，不依赖登录分配；
夹具 Cell 的通知记录用于确认旧客户端不能修改状态。文件不含凭据，仅用于测试。
两个夹具 BaseApp 在同一空间创建 Probe，覆盖并发初始化。
Godot 进程设置 `ATLAS_GODOT_TEST_HANDOFF=1`，验证错误目标身份拒绝、换服、目标 RPC
及重新登录后的旧控制权撤销。

原生 SDK（`atlas_net_client`，C ABI `0x03020000`）为 Godot 等 DLL 宿主支持该切换。
切换开始时清理旧实体、空间数据及 RPC 等待，`Handoff` 状态拒绝发送，校验目标通道、实体及进程身份，
并在 `CellReady` 前派发 `EntityTransferred`。认证失败或超时会断开客户端，应重新登录，
不能推断旧连接仍有控制权。首次认证声明移交支持；独立 `atlas_client` CLI 尚不支持
远端切换，在准备前明确拒绝。认证协议和原生 API 已变更，须配套重建全部 SDK 和服务器。
远端脚本等待上限为 40 秒；取消等待不撤销远端已经执行的工作。
RUDP 接口不共享已绑定的端点；重复绑定返回 `AddressInUse`，显式指定客户端源端口时也一样。
TCP 帧和 UDP/RUDP 数据报使用连接协议版本 2，前缀为 `ATLS` 和小端 16 位版本号。
TCP 随后是 32 位负载长度；UDP/RUDP 保留前缀之后的原有负载和报头。
TCP 版本不匹配时断开；不匹配或截断的数据报在分配对端通道前丢弃。
RUDP 的 MTU 与分片计算包含六字节前缀。服务器、SDK 和协议工具须配套重建，
不保留无版本流量的兼容路径；Mesh gossip 保留独立版本。

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
