# Phase 7: DBApp + 数据库层

**Status:** ✅ Oracle MySQL 8.4 后端可用；开发、联调与生产使用同一主线，
Oracle MySQL 8.4 支持多个 DBApp 共享同一个逻辑数据库。
**前置依赖:** Phase 5、Phase 6、脚本层 EntityDef 元数据（[`docs/scripting/`](../scripting/)）
**BigWorld 参考:** `server/dbapp/`, `lib/db_storage/idatabase.hpp`

## 目标

把 Phase 7 层面的数据库能力收敛成可持续演进的**数据库代理架构**：

- `DBApp` 作为数据库代理进程，隔离 `BaseApp` / `LoginApp` 与底层存储
- `IDatabase` 隔离上层业务与 Oracle MySQL 8.4 存储实现
- 实体 CRUD、`lookup_by_name`、`checkout / clear_checkout`、`auto_load`
  语义稳定
- 支撑当前登录主链路与 `checkin / checkout / relogin rollback` 场景

## 状态子阶段

| 子阶段 | 主题 | 状态 |
|---|---|---|
| P7.1 | 基线收敛 | ✅ |
| P7.3 | DBApp 集成补强（登录回滚 / BaseApp 故障路径） | ✅ |
| P7.4 | MySQL backend 正式实现（Oracle libmysqlclient + 线程池 + CI 契约） | ✅ |

## 设计

### 后端策略

Oracle MySQL 8.4 统一用于开发、联调、集成测试、部署及多 DBApp 共享存储验收；显式 SQLite 存档选择会被拒绝。
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
4. **数据库错误不得被当作实体不存在或触发存储回退**。

### 配置

`ServerConfig` 支持 `db_mysql_*` 系列字段。`database.type` 和 `--db-type` 仅接受 `mysql`，
`db_type` 默认值为 `mysql`。

## 已落地与剩余工作

### P7.3 — DBApp 集成补强 ✅

主链路（`AuthLogin`、`AbortCheckout`、`CheckinEntity`、BaseApp death
回收）已有集成与单测覆盖；MySQL 路径下 `WriteFlags::LogOff` 与
`auto_load` 也有回归测试；DBApp watcher 已注册。后续新增数据库行为时
继续补齐 MySQL 契约验证。

### P7.4 — MySQL backend ✅

`atlas_db_mysql` 插件使用固定的 Oracle MySQL 8.4.13 C API，共享客户端库，
`ATLAS_DB_MYSQL` 默认 ON。构建准备、开源交付条件和启动方式见
[Oracle MySQL](../oracle_mysql_CN.md)。MySQL 仅支持 Oracle 8.4 LTS。

数据模型保持 InnoDB、二进制 blob、事务 checkout、稳定操作去重、所有权版本和原子 ID 区间预留。
Deferred 模式每 worker 独立连接，按实体 hash 保序，`ProcessResults()` 在主线程交付回调。
自动重连关闭；连接出错后重新打开并核对逻辑库身份与格式，不将结果不确定的提交当作成功。
连接始终使用 TLS `VERIFY_IDENTITY`，要求显式 CA。

`MySQL Backend` CI 使用固定摘要的 Oracle 8.4.13 容器及临时证书，覆盖同步、线程池及真实 DBApp。
既有 MariaDB 验证记录仍只代表当时的后端，不能作为 Oracle 验收结论。

## 边界

**本文档负责：** DBApp 角色与边界、后端策略、`IDatabase` 语义、单 DBApp
实施路线、Oracle MySQL 8.4 后端范围。

**不负责：** 多 LoginApp / LoginAppMgr 设计、登录入口负载均衡、BaseApp 重复
登录状态机、CellApp / Space 持久化策略。

**不进入当前范围：** 多 DBApp / DBAppMgr / 分片（已归 Phase 15）/
Rendezvous Hash / MongoDB 主线 / 大规模 schema migration 框架 /
外部认证系统替代。
