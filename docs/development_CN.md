# 开发、Godot 接入与交付

从 Atlas 根目录使用 `tools/bin` 包装入口，实际逻辑使用 Python 标准库。
主路线为 Windows x86-64 或 Ubuntu 24.04 LTS x86-64、Oracle MySQL 8.4 LTS、
Godot .NET 桌面客户端。Unity、UE、WSL、压测和诊断工具保留为各自用途。

## 环境与构建

Windows 使用 VS 2022 17.14 正式版与 MSVC 14.44，允许系列内补丁更新；Linux 使用 GCC 13。
Python 最低 3.11.8，CMake 最低 3.28，Ninja 最低 1.11，不要求与验收机器的补丁版本相等。
.NET SDK 按 `global.json` 精确选择；运行时允许 .NET 10 servicing 补丁。
Godot 版本由业务项目决定，编辑器与 Godot.NET.Sdk 必须兼容；Atlas 不限制 Godot 补丁版本。
其他引擎版本需运行相同冒烟验证。满足版本要求不等于已经完整验收。

```bat
tools\bin\build_openssl.bat --config Debug
tools\bin\prepare_mysql.bat
tools\bin\build.bat debug
```

Ubuntu 使用 `bash tools/bin/<同名工具>.sh` 与相同参数。构建不修改系统设置；
`setup_linux` 会安装系统软件包，属于需要单独授权的管理工具，不是自动构建步骤。
`--build-dir build/<唯一名称>` 可隔离构建；切换工具链使用新名称，避免混用旧对象文件。
如果 MSVC 头文件依赖元数据被拒绝，使用全新构建目录重新生成全部对象文件。
Debug 用于日常开发，Release 用于交付，Profile 和 Sanitizer 用于诊断。
每个构建目录只使用一个配置，本地与 CI 共用 SDK 声明。

## 隔离数据库与检查

```bat
tools\bin\test_mysql.bat -- ctest --test-dir build/debug -C Debug --output-on-failure --no-tests=error
```

fixture 启动自己的固定版本 MySQL，创建专用数据库账号，向子命令提供 TLS 配置，
结束后关闭自己的服务；数据和日志保留在隔离目录供诊断，不分发其中私钥或备份。
Linux CI 使用固定 digest 的 Docker fixture，不连接现有开发数据库。
工作区存储超过 MySQL 超时预算时，使用 `--directory <全新本地目录>` 将测试库放在更快的本地存储，
保留现有超时与断言。
许可边界见 [Oracle MySQL 说明](oracle_mysql_CN.md)。 fixture 的 RELOAD 权限和可信函数创建设置仅用于认证缓存及故障触发器测试，不是生产账号要求。

已有隔离开发数据库使用 `--database-config <本地.json>` 或 `ATLAS_DB_MYSQL_*`。
配置字段优先，环境变量补缺，`--db-backend` 必须与配置一致。
密码使用进程环境变量，不提交到 JSON。CA、主机身份和逻辑数据库 ID 必须明确。
缺配置、连接或启动失败直接报错，不回退 SQLite。

显式选择 SQLite 实体存档会被拒绝；已有文件不会自动删除或迁移。
本次不新增策划配置数据库系统。

集群启动器先等待 MySQL DBApp 初始化完成，再启动 LoginApp、BaseApp 等使用方。

VS Code 的 `CMake: Test All` 同样使用该隔离 MySQL 夹具；单独运行公共单测仍无需数据库。
单项测试在所选平台没有匹配用例时明确失败。

原生全栈登录测试仅在 Windows 注册。两平台测试工作流都会执行已有托管真实登录用例，
要求恰好执行一次并通过，跳过或零匹配均失败：

```bat
tools\bin\test_mysql.bat -- python tools/verify_real_login.py --build debug
```

Linux 使用 `bash tools/bin/test_mysql.sh -- python3 tools/verify_real_login.py --build debug`。
工具选择同构建的 `Atlas.RpcFixture.Base.dll`，保留 `ATLAS_TEST_BASE_ASSEMBLY` 显式覆盖。
工具打印保留的 TRX 报告路径；MySQL 环境或构建产物缺失时明确失败。

## 开发集群与 Godot

构建业务 Base 和可选 Cell 程序集后，明确选择它们：

```bat
tools\bin\test_mysql.bat -- python tools/run_cluster.py --base-assembly <Base.dll> --cell-assembly <Cell.dll>
```

已配置隔离数据库时可直接使用 `tools\bin\run_cluster.bat`。
启动完成打印 READY、登录端口和日志目录；Ctrl+C 仅关闭本次进程。
默认不加载压测或 MVP 程序集；实体定义从所选 Base 程序集导出至本次运行目录。
地图和证书应明确指定，不从历史运行目录猜测。

```bat
tools\bin\client_sdk.bat export --build-dir build/debug --base-assembly <Base.dll> --client-assembly <Client.dll> --output .tmp/sdk-debug
tools\bin\client_sdk.bat verify --bundle .tmp/sdk-debug
tools\bin\client_sdk.bat install --bundle .tmp/sdk-debug --project <Godot项目目录>
```

更新时使用新的导出目录。现有 RPC 验证场景增加 `--fixture`，并选择配套的 RpcFixture
Base/Cell 程序集。发现过期 DLL 时安装停止并列出具体路径，删除仍需单独确认。
安装前关闭正在加载 SDK 的客户端或编辑器；失败时恢复原文件。
导出的实体定义须与服务器业务程序集配套。

SDK 包括 Shared 契约、Client 实体/RPC、Desktop/ClrHost 桌面胶水、原生网络库、
运行依赖和许可来源材料；RpcFixture 仅用于测试。
清单记录文件 hash、ABI、实体定义摘要和源码／构建来源，不替代已有运行时协议校验。

Godot 持有 `ClientSession` 和 `LoginClient`，在主循环 Poll 客户端并 Drain AtlasLoop；
等待 LoginAsync 完成后使用业务生成的实体和 RPC 代理。
退出会话时 Disconnect，退出场景时 Dispose 客户端和循环。
复用现有项目的 `Verification/Automated/AtlasClientRoundTrip.tscn` 验证认证、RPC 和断线；
所需测试环境变量见该项目 README。错误先查 SDK 清单，再查 READY 端口、CA/身份、
DBApp 日志、实体定义摘要与生成式 RPC 契约，不输出凭据。

## 交付

先准备 Release OpenSSL，构建 Release 并导出配套 SDK；另建启用 `ATLAS_BUILD_TESTS=ON`
的 Release 验证目录。交付工具构建该目录并运行 CTest，通过后生成新归档：

```bat
tools\bin\build.bat release --build-dir build/release-check --with-tests
tools\bin\test_mysql.bat -- python tools/package_delivery.py --build-dir build/release --verification-build build/release-check --sdk .tmp/sdk-release --base-assembly bin/release/<Base.dll> --output .tmp/atlas-delivery.zip
```

归档包含服务器、SDK、实体定义、工程源码、许可和构建来源，不自动收集 Unity/UE 工程或美术素材。
使用 `--fixture` 的 SDK 会将交付包明确标记为验证夹具用途，不能当作游戏产品发布包。Oracle 客户端维持现有完整源码
FOSS 分发边界，不代表获得闭源产品分发授权，也不代表完成整产品法律审查。
Godot 导出／AOT、移动端、Web、容量和 HA 不因打包成功自动获得验收。
Windows/Linux 分别记录命令及通过、代码失败、环境阻断、未运行、原有平台跳过；
Ubuntu 26.04 结果不能代替 24.04。

## 更新基线

允许范围内的工具补丁更新仍须记录实际版本。变更编译器系列、.NET SDK、依赖归档或数据库
fixture 版本时，明确修改已有声明与校验和，执行受影响的构建、数据库和客户端回归；
本地与 CI 同步更新。Godot 升级由业务项目管理，重复其构建和联网场景验证。
固定基线不代表已经完成安全漏洞评估，也不阻止经过回归的安全更新。
