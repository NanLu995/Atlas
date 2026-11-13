# Phase 7: DBApp + 数据库层

**Status:** ✅ SQLite / MySQL/MariaDB 后端可用；SQLite 用于单 DBApp 开发和测试，
MySQL/MariaDB 支持多个 DBApp 共享同一个逻辑数据库。
**前置依赖:** Phase 5、Phase 6、脚本层 EntityDef 元数据（[`docs/scripting/`](../scripting/)）
**BigWorld 参考:** `server/dbapp/`, `lib/db_storage/idatabase.hpp`

## 目标

把 Phase 7 层面的数据库能力收敛成可持续演进的**单 DBApp 架构**：

- `DBApp` 作为唯一数据库代理进程，隔离 `BaseApp` / `LoginApp` 与底层存储
- `IDatabase` 屏蔽 SQLite / MySQL/MariaDB 后端差异
- 实体 CRUD、`lookup_by_name`、`checkout / clear_checkout`、`auto_load`
  语义稳定
- 支撑当前登录主链路与 `checkin / checkout / relogin rollback` 场景

## 状态子阶段

| 子阶段 | 主题 | 状态 |
|---|---|---|
| P7.1 | 基线收敛 | ✅ |
| P7.2 | SQLite backend，显式单 DBApp 开发/测试 | ✅ |
| P7.3 | DBApp 集成补强（登录回滚 / BaseApp 故障路径） | ✅ |
| P7.4 | MySQL backend 正式实现（MariaDB Connector/C + 线程池 + CI 真机验证） | ✅ |

## 设计

### 后端策略

MySQL/MariaDB 默认用于主要开发、联调、部署及多 DBApp 共享存储验收；SQLite 显式用于单 DBApp 冒烟及独立测试。
XML 数据库后端已移除，显式选择 `xml` 时配置及工厂均报错，不提供后端回退或数据迁移。
通用 XML 解析、配置及实体定义支持不受影响，既有数据文件保留。
BaseApp checkout 绑定每次启动的安全随机进程实例身份。死亡回收在事务中退役该身份并清理
对应租约，阻止旧实例的延迟获取及首次保存；重复通知不影响同地址的新实例。
未确认的退役保留本地所有权并重试，`atlas_checkout_processes` 须随实体数据一起备份。

数据模型统一：`blob + 少量索引列 + checkout 状态列`
（`dbid / type_id / identifier / password_hash / blob / auto_load /
checked_out / checkout_addr / checkout_app_id / checkout_eid`）。

### 接口与协议

**`IDatabase` 主接口（稳定）：** `put_entity / get_entity / del_entity /
lookup_by_name / checkout_entity / checkout_entity_by_name / clear_checkout
/ clear_checkouts_for_address / mark_checkout_cleared /
get_auto_load_entities / set_auto_load / set_deferred_mode /
process_results`。回调统一在主线程 `process_results()` 中交付。

**DBApp 消息面（稳定）：** `WriteEntity[Ack] / CheckoutEntity[Ack] /
CheckinEntity[Ack] / DeleteEntity[Ack] / LookupEntity[Ack] / AbortCheckout[Ack]
/ AuthLogin`。

**登录边界：** DBApp 只负责账号查找 / 认证读取 / 账号自创建；不负责
客户端连接、BaseApp 分配、重复登录协商。

### 工业级实施原则

1. **统一异步契约** — 结果一律通过 `process_results()` 回主线程，后端
   不允许行为分裂。
2. **Checkout 必须有 owner 语义** — 表达 owner / `Checking` / `Confirmed` /
   BaseApp 死亡回收。
3. **Fail-safe 清理** — 覆盖 `AbortCheckout`、`WriteFlags::LogOff`、
   `CheckinEntity`、BaseApp 死亡、DBApp 关闭。
4. **后端切换不影响上层协议**。

### 配置

`ServerConfig` 支持 `db_sqlite_path / wal / busy_timeout_ms / foreign_keys`、
`db_mysql_*` 系列字段。`database.type` 和 `--db-type` 仅接受 `sqlite` 或 `mysql`，
MariaDB 也使用 `mysql`；`db_type` 默认值为 `mysql`。

## 已落地与剩余工作

### P7.3 — DBApp 集成补强 ✅

主链路（`AuthLogin`、`AbortCheckout`、`CheckinEntity`、BaseApp death
回收）已有集成与单测覆盖；SQLite / MySQL 路径下 `WriteFlags::LogOff` 与
`auto_load` 也有回归测试；DBApp watcher 已注册。后续新增数据库行为时
继续补齐多后端契约一致性验证。

### P7.4 — MySQL backend ✅

`atlas_db_mysql` 插件基于 **MariaDB Connector/C**（`FetchContent`，`ATLAS_DB_MYSQL`
默认 ON；MySQL 8 与 MariaDB 协议兼容）。数据模型与语义对齐 SQLite：InnoDB
建表、`X'..'` 十六进制字面量二进制绑定、事务化 checkout（条件 UPDATE +
affected-rows 竞争检测）、dbid range、id counter。

`deferred` 模式（DBApp 驱动方式）下走**工作线程池**：每 worker 独立连接、按实体
hash 保序、结果经 `ProcessResults()` 回主线程，DB 往返不阻塞 tick 循环；连接
`MYSQL_OPT_RECONNECT` 自愈掉线。非 deferred 走 bootstrap 连接内联。`SupportsMultiDbapp()`
返回 true（事务 checkout 在共享 DB 下安全，为 Phase 15 铺路）。

验证：`test_mysql_database` 契约测试镜像 SQLite 的 9 项断言，`MySQL Backend`
工作流起 MariaDB 服务容器真机跑通（同步内联与线程池两条路径均覆盖）。

## 边界

**本文档负责：** DBApp 角色与边界、后端策略、`IDatabase` 语义、单 DBApp
实施路线、SQLite/MySQL/MariaDB 后端范围。

**不负责：** 多 LoginApp / LoginAppMgr 设计、登录入口负载均衡、BaseApp 重复
登录状态机、CellApp / Space 持久化策略。

**不进入当前范围：** 多 DBApp / DBAppMgr / 分片（已归 Phase 15）/
Rendezvous Hash / MongoDB 主线 / 大规模 schema migration 框架 /
外部认证系统替代。
