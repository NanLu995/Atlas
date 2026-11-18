# Oracle MySQL 配置与开源交付

目标为 Oracle MySQL 8.4.13 LTS 及其 C API，不使用 Connector/C++。
开发、联调、集成测试和生产的实体存档统一使用 Oracle MySQL。
显式 SQLite 类型和已移除字段明确失败；已有文件不会删除或迁移。本次不新增策划配置系统。
Oracle 验收须单独记录，不能沿用历史 MariaDB 结果。

```bat
tools\bin\prepare_mysql.bat
```

Linux 使用 `bash tools/bin/prepare_mysql.sh`。工具支持 x86-64 Windows/Linux，
检查固定 SHA-256，在 `build/oracle-mysql/8.4.13/` 准备 SDK；`--cache` 可指定已有下载缓存。
同时保留对应 Oracle 源码归档，不移除现有 MariaDB 安装或数据库。

包哈希固定前已核验 Oracle 分离签名，发布密钥指纹为
`BCA43417C3B485DD128EC6D4B7B3B788A8D3785C`。来源为
[官方下载站](https://dev.mysql.com/downloads/mysql/8.4.html)及其 Oracle CDN。
重复运行校验已固定的包哈希，不同内容会拒绝。

本项目仅授权在完整对应源码随附的开源交付路线下引入 Community 客户端，适用 GPLv2
及 Universal FOSS Exception；不构成闭源分发授权。Atlas 保持 MIT，客户端保留自身许可。
分发时须保留官方 LICENSE、例外、版权声明及对应源码和构建材料；根目录 MIT 不能单独证明合规。
第三方源码及运行时义务仍须按实际部署产物核查。

参见 [8.4.13 许可](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.4.13/LICENSE)
及 [FOSS 例外](https://oss.oracle.com/licenses/universal-foss-exception/)。
准备归档不表示完整产品已获分发许可结论，也不表示认证、TLS、备份和失败处理已通过验收。

## 构建与运行时

常规构建前运行 SDK 准备工具。使用 Oracle 原样发布的共享 C API：Windows 为
`libmysql.dll` 及导入库，Linux 为 `libmysqlclient.so.24`，不链接 MariaDB 或 Connector/C++。
纯客户端跨平台 preset 关闭数据库插件；固定服务器 SDK 支持 Windows/Linux x86-64。

构建部署客户端所需的共享 OpenSSL 3.5.9：Windows 放在可执行文件旁，Linux 放在
`private/` 并沿用客户端 RPATH。Atlas 自身继续使用独立的固定静态 OpenSSL SDK。
客户端内嵌 zlib、Zstandard，保留在 Oracle LICENSE 和源码归档中。系统库和 Windows C++
运行库为环境前提，不从任意系统安装目录复制分发。

`bin/<构建名>/licenses/OracleMySQL/` 保留官方 LICENSE（含 FOSS 例外及第三方声明）、
INFO_SRC、INFO_BIN、包与源码校验信息，以及匹配的 MySQL/OpenSSL 源码归档。
交付时须一并保留，并提供 Atlas 和其他 FOSS 的完整对应源码与构建说明。该目录本身
不能授权闭源业务代码，也不代表未来完整产品的分发合规已获确认。服务端 GPL 与链接客户端
的 GPLv2 加 FOSS 例外须分别审查；仅自营部署不构成未来闭源分发授权，后者须单独确认许可。

## 连接与验证

配置 `database.type=mysql`、MySQL 8.4 服务端、专用账号、预期的 `mysql_database_id` 和
`mysql_ssl_ca`。CA 路径也可通过 `--db-mysql-ssl-ca` 或现有集群工具的
`ATLAS_DB_MYSQL_SSL_CA` 指定，主机必须与证书身份匹配。连接（包括 localhost）和恢复工具强制 TCP、TLS 1.2/1.3
及 VERIFY_IDENTITY；连接失败、错误凭据、不可信证书、身份不匹配或不兼容存档均明确拒绝，
不回退 SQLite。测试夹具服务端强制加密，使用 caching_sha2_password。

```bat
tools\bin\test_mysql.bat -- ctest --test-dir build/debug -C Debug -L mysql -V --output-on-failure
```

Linux 使用同名 `.sh`。`--docker` 使用夹具中固定补丁版本及摘要的 Oracle 8.4.13 镜像；
不带此参数时使用已准备的隔离 SDK 服务端。缺少运行库属于环境阻断，不自动安装或换库。
夹具新建独立目录、随机凭据、短期测试 CA 和证书，向命令传递连接环境；结束后仅关闭自身
服务端，不复用已有目录。保留数据和日志供排查，不分发测试 CA 私钥或备份。原生数据库
测试会重置测试库的框架表，只能使用专用夹具数据库。`mysql`/`mysqldump` 优先从固定 SDK
查找，拒绝 MariaDB 和其他版本系列。生产容量、远端 CI 和 HA 仍需单独验收。

离线检查使用 `ctest --test-dir build/debug -C Debug -L unit -LE mysql --output-on-failure --no-tests=error`。
连接配置校验仍可离线执行；带 `mysql` 标签的测试必须使用隔离夹具。
全部插件须配套重建至数据库 ABI V7；存储 schema 和 blob 格式保持不变。
