# Atlas Engine

> Status: Current project entry point. For detailed phase status, see
> [`docs/roadmap/README.md`](docs/roadmap/README.md).
>
Atlas is a distributed MMO server framework built with **C++20** and **C# (.NET)**. It uses a BigWorld-style split between login, base, cell, database, and manager processes, while gameplay logic runs in managed code through embedded CoreCLR.

**[中文文档](README_CN.md)**

## Why Atlas Exists

Atlas is aimed at large, long-running online worlds where the server must scale across processes, keep simulation authority on the server, and still let gameplay teams work in C#. The C++ core owns networking, process coordination, persistence, spatial simulation, and profiling; the C# layer owns entity behavior, shared gameplay contracts, generated client/server types, and sample game logic.

## Current Capabilities

- Multi-process cluster layout with service discovery through `machined`.
- Base/Cell entity model with mailbox RPC and generated C# entity code.
- CellApp spatial simulation with witness-based AoI, ghost entities, BSP partitioning, and offload coordination.
- Embedded .NET runtime for server-side gameplay scripting.
- C# client runtime with desktop and Unity integration surfaces, plus the UE MVP
  C++ plugin/client path.
- Runtime-loaded MySQL/MariaDB and explicitly selected SQLite database plugins.
- DBAppMgr range sharding, worker-rebuilt manager HA, and Reviver supervision for
  CellAppMgr, BaseAppMgr, and DBAppMgr.
- Tracy instrumentation, baseline stress drivers, and trace comparison tools.

The primary MySQL/MariaDB backend is implemented. DB replication, shard auto-rebalance,
and full session resume remain out of the current production baseline.

## MVP Unity Demo

The MVP sample under `samples/mvp/` demonstrates the full loop from login to
Base/Cell handoff, AoI-driven entity streaming, Unity client rendering,
server-authoritative movement with local prediction, MovementCommand-backed
dash, projectile damage, NPC AI, HP replication, UI Toolkit login/HUD, and
local cluster tooling.

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

Open `samples/mvp/UnityClient` in Unity Hub and hit Play. Its client code is split into `Assets/Scripts/Runtime` and `Assets/Scripts/Editor` asmdefs, with runtime modules for app, camera, world, views, UI, input, projectiles, and debug overlays. To build a standalone player, run `tools\bin\build_mvp_unity.bat`; it resolves the Unity editor from `ProjectSettings/ProjectVersion.txt`, or use `--unity` / `UNITY_EXE` to override the editor path.

![Atlas MVP Unity demo](docs/mvp/mvp_unity.gif)

## Architecture

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

| Process | Responsibility |
|---|---|
| `machined` | Local service registry, heartbeat target, and Birth/Death notification hub. |
| `Reviver` | Supervises manager processes and restarts them through worker-rebuild recovery. |
| `LoginApp` | Client-facing login gateway. |
| `BaseAppMgr` | BaseApp registration, selection, and cluster coordination. |
| `BaseApp` | Base entity ownership, client proxy state, mailbox routing, persistence handoff, and scripting. |
| `CellAppMgr` | CellApp registration, space geometry, BSP partitioning, and offload coordination. |
| `CellApp` | Spatial entities, movement, witness updates, AoI, ghosting, and scripting. |
| `DBAppMgr` | DBApp registration, range shard table ownership, and shard-table recovery. |
| `DBApp` | Asynchronous entity persistence through configured database backends. |

Server processes share a small framework hierarchy:

```text
ServerApp
├── ManagerApp     daemon and manager processes without CoreCLR
└── ScriptApp      ServerApp plus embedded CoreCLR
    └── EntityApp  ScriptApp plus entity definitions and background tasks
```

`ServerApp` provides configuration, machined registration, watcher metrics, tick callbacks, and signal dispatch. `BaseApp` and `CellApp` extend it through `EntityApp` so C# gameplay can run on top of the C++ server core.

## Requirements

Windows:

- Visual Studio 2026 (recommended) or Visual Studio 2022 17.14+, with the Desktop development with C++ workload; managed projects use the .NET SDK CLI on every generator
- CMake 3.28+
- .NET 10 SDK
- Python 3.9+
- Git

Linux:

- GCC 13+ or Clang with C++20 support
- CMake 3.28+
- Ninja for the debug preset
- OpenSSL 3.0+ Crypto development files (`libssl-dev` on Ubuntu)
- .NET SDK satisfying `global.json` (10.0.202 or a later stable feature band);
  Ubuntu 26.04's 10.0.1xx SDK does not satisfy this requirement.
- Python 3.9+
- Git

The Windows build wrapper loads the MSVC environment automatically. Ninja is provisioned automatically for the Windows debug preset when it is not already available.

CMake is resolved from PATH first, then from the selected Visual Studio installation.

Authentication rejects empty usernames and passwords. Session keys use the operating
system secure random source; entropy failures reject login. Public transport encryption
and server-side password derivation are not yet implemented.

## Build

Use the wrappers for normal development:

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

Common options:

```bash
tools\bin\build.bat debug --clean
tools\bin\build.bat debug --config-only
tools\bin\build.bat debug --build-only

tools\bin\build.bat debug --build-dir build/verify-windows
```

Direct CMake remains supported:

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

| Preset | Purpose |
|---|---|
| `debug` | Fast iteration with Ninja Multi-Config, Debug, MSVC `/Z7`, PCH, and tests. |
| `release` | Optimized build with tests disabled. |
| `hybrid` | RelWithDebInfo for optimized debugging. |
| `profile` | RelWithDebInfo with Tracy and profiling helpers; tests disabled. |
| `vs` | Visual Studio solution under `build/vs/`, Debug default, with tests (Windows). |

Sanitizer presets are available where supported: `asan`, `asan-msvc`, `tsan`, and `ubsan`.

### Linux build and test from Windows (WSL)

Verify the Linux build against the same Windows clone — no second checkout, no manual sync. Source is read in place from `/mnt/<drive>/...`; build artefacts land in `~/atlas-builds/<preset>` inside the WSL filesystem so cross-FS writes don't cap compile speed. `sccache` is picked up automatically when present, mirroring CI.

One-time prerequisite (elevated PowerShell): `wsl --install -d Ubuntu-26.04`.

```bash
# One-time toolchain install (g++-13, ninja, .NET 10, sccache, clang-format, ...)
tools\bin\setup_linux.bat

# Build (mirrors build.bat semantics)
tools\bin\build_linux.bat
tools\bin\build_linux.bat release
tools\bin\build_linux.bat --clean

# Build + ctest
tools\bin\build_linux.bat --with-tests
tools\bin\build_linux.bat --with-tests --label "unit|integration"
```

Override the build dir with `ATLAS_LINUX_BUILD_DIR` if you want it elsewhere.

`--with-tests` runs native unit, integration, and managed suites by default;
`--label` restricts the selection.

## Test

```bash
ctest --test-dir build/debug -C Debug --label-regex unit --output-on-failure
ctest --test-dir build/debug -C Debug --label-regex integration --output-on-failure
ctest --test-dir build/debug -C Debug --label-regex managed --output-on-failure
ctest --test-dir build/debug -C Debug -R test_math --output-on-failure
```

C# tests:

The `managed` label runs Runtime, Generators, and Client tests serially and resolves
native libraries from the matching build output. Use the same build directory for
building and testing; isolated builds must not load DLLs from another preset.

Windows process tests use server executables and the deployed runtime beside the test
binary; CTest supplies the runtime configuration. Missing artifacts fail the test.
Building a target that uses the database factory also builds every enabled database
plugin on both platforms; Windows deploys those DLLs beside the executable.
The login, CellApp and Manager/Reviver process harnesses currently run only on Windows;
their explicit Linux skips do not validate Linux process recovery.

`AtomicReplaceFile` replaces a same-volume path without promising crash-durable storage.
On Windows, existing targets use `ReplaceFileW`, preserving target metadata and allowing readers
that share deletion. Close all source handles first; blocked access returns an error without retry.
Missing targets use `MoveFileExW`. Linux uses rename semantics.

```bash
dotnet test tests/csharp/Atlas.Runtime.Tests/Atlas.Runtime.Tests.csproj --configuration Debug
dotnet test tests/csharp/Atlas.Generators.Tests/Atlas.Generators.Tests.csproj --configuration Debug
dotnet test tests/csharp/Atlas.Client.Tests/Atlas.Client.Tests.csproj --configuration Debug
```

Before committing C++ changes, run unit tests and clang-format on changed C++ files:

```bash
clang-format --dry-run --Werror <changed files>
```

## Run a Local Cluster

A development cluster starts in this order:

```text
machined → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

On Linux or Git Bash, the cluster wrapper starts the same stack through the world-stress driver with zero clients and keeps it alive until interrupted:

```bash
tools/bin/run_cluster.sh
```

Scripted cluster validation:

```bash
tools/bin/test_cluster.sh
# Windows
tools\bin\test_cluster.bat
```

## Database

Atlas loads SQLite and MySQL/MariaDB plugins through the database factory. MySQL/MariaDB
is the default for development, integration, deployment and cluster acceptance, with multiple
DBApps sharing one logical database. `ATLAS_DB_MYSQL` defaults to ON and builds the plugin
using MariaDB Connector/C. SQLite requires explicit selection and is limited to single-DBApp
lightweight development, quick smoke checks and independent tests, with WAL and busy-timeout support.
Use `database.type` in JSON or `--db-type` with `sqlite` or `mysql` (also for MariaDB).
The XML database backend has been removed. Explicit `xml` or other unsupported backend
types fail configuration validation and factory creation, even if an old plugin remains
beside the executable; there is no fallback. Existing data files are not removed or migrated.
General XML parsing, serialization, configuration data and entity definition support remain.

Entity IDs are reserved atomically before being issued. MySQL locks the counter row
and waits for commit confirmation; failures never fall back to ID 1. Crashes may leave
unused ranges. A missing initialized counter is an error, including after restart.
Backups must retain the counter and its initialization metadata.

DBApp shard cursor recovery includes existing entities, durable operation receipt results and
checkout version history within the half-open shard range. Deleting an entity does not remove
its retained receipt or lease history, so those confirmed identities remain below the recovered
cursor after restart. Backups must retain all three sources; this does not provide recovery
from arbitrarily removed metadata or make raw backend deletion an ID reclamation operation.
Concurrent MySQL startup serializes EntityID counter initialization on the existing schema
metadata row before reading its initialization marker and creating the counter.

Transaction commit failures report an uncertain outcome rather than success. SQLite
batch callbacks wait for the transaction result. Checkout distinguishes missing entities
from database errors. Logoff writes with `kLogOff` and releases checkout in that write;
BaseApp retains the entity if saving fails.

BaseApp writes carry a random 128-bit operation ID, retained across routing retries and
echoed in replies. MySQL commits a SHA-256 request digest and successful result in
`atlas_write_receipts` with the data modification. Matching replays return the original
DBID after DBApp restarts; changed requests with the same ID fail. Write or receipt
failures roll back both. Request hashing uses the platform SHA-256 implementation. Receipts have no
automatic expiry and must be backed up with entity data.

Shared MySQL checkout versions increase transactionally and survive deletion and restart
in `atlas_checkout_versions`, which backups must also retain. Exhaustion fails without
wrapping. Writes, checkins and standalone deletes validate the expected owner and version;
DBApp also checks the source channel. Checked-out rows reject writes without a version.
Checkin clears local ownership only after database confirmation. Cancellation confirms the
original lease is inactive; unknown cancellations fail, and successful cached replies recheck database
ownership. Old callbacks and successful write replays cannot release a newer owner's lease.
`CheckinEntityAck` echoes the request ID, entity and complete owner. BaseApp retains a pending
checkin across failed sends, lost replies and DBApp route changes, and matches the reply source
and complete request. It makes at most three attempts, at least one second apart; exhaustion
logs an error and keeps that database identity fenced. A late matching success can complete it.
DBApp confirms success after conditional release or a database read proving the original lease
is inactive. Database read failures and a still-active original lease cannot confirm release;
duplicate and old checkins never release a newer lease. `baseapp/pending_checkin_count` includes
stalled work. This confirmation runs while BaseApp's event loop is active; shutdown draining
remains separate work.

The first shared MySQL save creates the entity, acquires checkout and stores its receipt
in one transaction. The confirmed reply supplies the ownership version. Replays return
the original DBID and version. The digest binds the creator address, AppID and runtime
entity; another creator cannot reuse the same operation ID.

While a save or logoff is pending, new saves are rejected with a warning. Disconnect
logoff waits for a successful save; competing force-logoff requests fail explicitly.
An uncertain commit, disconnection or reply timeout retains the original operation and
payload for up to three retries. Error replies and failed sends delay retries by at least
one second. An unresolved entity remains fenced against new persistence and logoff
destruction. Late success can confirm it; exhaustion logs an error and requires a process
restart if confirmation never arrives. Ordinary failures before uncertainty complete as failures.

The SQLite backend stores increasing checkout versions and supports conditional release
against the complete owner. Its limit is `INT64_MAX`; exhaustion and invalid stored
versions fail. Version history survives deletion and restart in `atlas_checkout_versions`
and must be retained in backups. Failed commits do not return an acquired version or a
successful release. Versioned writes, logoff and standalone deletes validate every owner
field in the same transaction as the existing entity's modification.

For every backend, DBApp binds checkout ownership to the source channel and waits for
conditional database release before removing its local lease. Cancellation resolves the
original operation receipt, including through another DBApp, and conditionally releases its lease.
An already inactive original lease confirms cancellation without changing a newer holder.
Missing receipts or mismatched cancellations fail; resolution never acquires a lease.
Old release callbacks cannot clear a newer lease.
Each server startup generates a nonzero process instance using the OS secure random source;
entropy failure rejects startup. Registration, discovery, death notifications and checkout
messages carry this identity. DBApp rejects missing process identities. Address and AppID reuse
cannot make an old death notification clear a new process's ownership. Retirement records
in `atlas_checkout_processes` atomically fence later acquisitions and first saves, including requests
queued in another DBApp. Backups must retain this table; records do not expire. Failed or uncertain
retirement retains local ownership and retries while DBApp runs. Direct backend callers may use
instance zero only when process death tracking is unnecessary. The updated wire protocol and
entity schema require matching rebuilt processes and a new database; no old-format migration is provided.
Cached successful checkout replies recheck database ownership, and changed requests cannot reuse
the same cached result. Named checkout cancellation uses the resolved database ID.

Durable operation receipts cover `WriteEntity`, `DeleteEntity`, and `CheckoutEntity` on
both backends. SQLite stores the SHA-256 request digest and result in `atlas_write_receipts`
in the modification's transaction.
Backups must retain these receipts. Receipts do not expire automatically.
Request digests use [Windows CNG](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthash)
or the system [OpenSSL EVP API](https://docs.openssl.org/3.5/man3/EVP_DigestInit/).
OpenSSL 3.0+ uses [Apache-2.0](https://openssl-library.org/source/license/index.html);
the Linux setup and CI install its distribution development package.
Write replays return the original DBID and any originally issued lease without executing
again or releasing a newer owner. Checkout retries retain their random operation ID across
DBApp changes and confirm the original entity and ownership version. Database receipts reject
released or superseded leases with `checkout_lease_expired`; retries never reacquire them.
DBApp can reject a conflicting holder earlier using its local checkout state. Named checkout retries
remain bound to the original entity even after it is renamed.
Reusing an operation ID for another kind, target, holder, or payload is rejected.
Unreadable receipts report an uncertain outcome instead of being treated as absent.
SQLite DBApp first saves persist the entity and its lease in the same
transaction; replies expose the owner
only after persistence succeeds. Invalid creators and exhausted lease versions fail
without creating an entity. Plugins without a durable operation implementation return
`write_operation_not_supported` or `checkout_operation_not_supported`; unsupported checkout
resolution returns `checkout_resolution_not_supported`.
Account auto-creation uses the unique entity type and account name as its identity.
An identifier conflict or uncertain commit triggers a fresh credential lookup, without
another create attempt. Only a confirmed account with matching nonempty credentials can
authenticate; an absent account or failed lookup returns an internal error.
Checkout requests also reject changed targets or holders while pending, preserving the
original request and its cancellation state. Replies carry the operation ID and a typed
database error; BaseApp matches both the expected source and operation ID. Uncertain replies
are not cached. Login and native database loads retry the same operation and holder, at most
three times with at least one second between retries. Native loads use explicit waiting records,
and persistence is rejected while loading. Request ID wraparound skips outstanding records.
After uncertainty or a failed load rollback, BaseApp waits for cancellation confirmation.
Abort requests include the original target and holder; acknowledgements match the source and
operation ID. Three unsuccessful cancellation retries leave a stalled record and retain any
resolved account fence instead of expiring it. A matching late success can restart resolution.
`baseapp/pending_entity_checkout_count` and `baseapp/stalled_checkout_count` expose waiting work.
Unconfirmed cancellation requires later database confirmation or process restart; it does not
promise to undo remote work that already ran.
Death cleanup still lacks process incarnation fencing.
Shared-storage failover has not completed acceptance.
Use newly initialized databases and rebuild all participating processes; old formats and
mixed process versions have no compatibility layer. Startup format validation remains incomplete.

The MySQL CI job runs backend and real DBApp checkout network tests with a temporary
database. Local shared-storage tests need all `ATLAS_TEST_MYSQL_*` settings; otherwise
required MySQL cases fail with an environment-blocked message. SQLite success never substitutes
for MySQL acceptance. The general Windows/Linux CI jobs explicitly use `ATLAS_DB_MYSQL=OFF`
and SQLite smoke scope; the separate MySQL job exercises the database/network contracts.
The lost-checkout-reply case starts two real DBApp subprocesses. After suppressing delivery
of a successful reply, it forcibly terminates the original DBApp and retries through the other
process, including duplicate cancellation and expired-lease checks. This verifies that failure
boundary; it does not establish full HA acceptance.
The backend fault test relays the connection without changing TLS negotiation. An independent
database connection observes the committed entity and its receipt before the relay drops the
return data. The caller must report `kCommitUnknown`; retrying through another backend instance,
including after restart, must return the original DBID and lease without creating another entity.

Example DBApp configuration:

```json
{
  "database": {
    "type": "mysql",
    "mysql_host": "127.0.0.1",
    "mysql_port": 3306,
    "mysql_user": "<database-user>",
    "mysql_database": "<isolated-development-database>"
  }
}
```


Set `ATLAS_DB_MYSQL_PASSWORD` in the launching process environment using your secret store
or a masked prompt; do not put credentials in commands, checked-in configs or logs.
DBApp reads this variable when the configured password is empty. Host, user and database
come from the configuration or CLI. A missing database/user/password, invalid connection
settings, unavailable plugin or failed connection stops startup; no SQLite fallback occurs.
The tools also accept `ATLAS_DB_MYSQL_HOST`, `PORT`, `USER`, `PASSWORD`, `DATABASE` and
`POOL_SIZE` (each with the `ATLAS_DB_MYSQL_` prefix) or `--database-config <local-json>`.
They pass the password through the child environment, excluding it from generated JSON.

```bat
rem MySQL is the default; use an existing isolated development database and local config.
tools\bin\test_cluster.bat --database-config <local-dbapp-json>
tools\bin\run_login_stress.bat --database-config <local-dbapp-json>
tools\bin\run_world_stress.bat --database-config <local-dbapp-json>
rem Explicit single-DBApp quick smoke; this is not MySQL acceptance.
tools\bin\test_cluster.bat --db-backend sqlite
bin\debug\atlas_dbapp.exe --type dbapp --db-type sqlite --db-sqlite-path .tmp/sqlite-smoke/store.sqlite3
```

Linux uses the matching `tools/bin/*.sh` wrappers and `bin/debug/atlas_dbapp` with the same
arguments. SQLite-only builds may explicitly set `-DATLAS_DB_MYSQL=OFF`; rebuild the selected
output directory before running. The real-cluster C# fixture defaults to MySQL; an explicit
`ATLAS_TEST_DB_BACKEND=sqlite` selects smoke coverage. Tools emit `database-run.json` with the
backend, topology and scope, and include the backend in console/READY output. A smoke result
is marked `sqlite_smoke_only`, never a capacity or shared-storage acceptance result.

SQLite uses an OS-held lock beside the canonical local database path for the connection's
lifetime. A second Atlas opener of the same store fails before schema/data access; process exit
releases the lock, while the `.atlas-lock` file remains. Use a local filesystem, do not remove
or replace an active lock file, and stop the owner before external database maintenance.
Separate SQLite files remain independent; shared files, replication and production HA are
unsupported. SQLite use for design/configuration data is separate from entity persistence;
this project does not add a configuration system or connect it to `IDatabase`.

## C# Gameplay and Client Runtime

Managed code lives under the C# project tree and is built with the native server when `ATLAS_BUILD_CSHARP=ON`. `Atlas.Shared` carries shared contracts, `Atlas.Runtime` binds server gameplay to the engine, `Atlas.Client` powers client-side entity state, and `Atlas.Generators.Def` turns entity definitions into typed code.

Sample gameplay projects cover base, client, stress, and the Unity MVP scenario.

Unity integration stages the Atlas managed client assemblies and the native `atlas_net_client` plugin for a Unity project. The MVP wrapper also stages `Atlas.Mvp.Client.dll`; its build wrapper runs the same staging step before invoking Unity batchmode.

```bash
tools/bin/setup_unity_client.sh --unity-project <path>
tools/bin/setup_mvp_unity.sh
tools/bin/build_mvp_unity.sh
# Windows
tools\bin\setup_unity_client.bat --unity-project <path>
tools\bin\setup_mvp_unity.bat
tools\bin\build_mvp_unity.bat
```

Packaged MVP players can run the native movement predictor self-test without a
cluster:

```bash
./out/mvp-unity/windows/AtlasMvp.exe -batchmode -nographics -atlas-predictor-self-test -logFile out/mvp-unity/predictor-self-test.log
```

## Properties and RPC

Entities are declared in `entity_defs/<Name>.def`. The generator turns each definition into a typed `partial` class on both server and client; gameplay code only fills in implementations.

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

Property `scope` controls who sees a write: `all_clients`, `own_client`, `other_clients`, `cell_public`, `cell_public_and_own`, `base`, `base_and_client`. `persistent="true"` opts a field into DBApp persistence; `reliable="true"` is retained in the descriptor, while property deltas currently all use the reliable channel.

Server side — assigning to a generated property emits a delta to every observer in scope; `Client.<Method>(...)` is a typed stub that fans out to the matching client RPC.

```csharp
[Entity("Avatar")]
public partial class Avatar : ServerEntity
{
    public partial void UseItem(int itemId)        // exposed="own_client"
    {
        Hp -= 5;                                    // synced to all_clients
        Client.ShowDamage(50, EntityId);            // server → owning client
    }
}
```

Client side — `OnXxxChanged(old, new)` is the generated change hook; client-method partials are invoked when the server pushes an RPC.

```csharp
[Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    partial void OnHpChanged(int oldValue, int newValue) { /* react to sync */ }

    public partial void ShowDamage(int amount, uint attackerId) { /* handle push */ }
}
```

Containers (`list[T]`, `dict[K,V]`, `list[list[int32]]`, …) and user `<struct>` types replicate field- or op-level deltas through the same setter path; see `entity_defs/StressAvatar.def` and `samples/stress/` for the full coverage matrix.

## Profiling and Stress

The `profile` preset enables Tracy instrumentation and profiling helpers. The default baseline runs 200 clients for 120 seconds and writes captures under `.tmp/prof/baseline/`.

```bash
tools\bin\build.bat profile
tools\bin\run_baseline_profile.bat
```

```bash
tools/bin/build.sh profile
tools/bin/run_baseline_profile.sh
```

Compare CellApp captures:

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

For deeper context, start with the profiling, scripting, gameplay, stress-test, Unity, and coding-style docs under `docs/`.

## License

Atlas is released under the [MIT License](LICENSE).
