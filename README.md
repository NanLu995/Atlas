# Atlas Engine

[Development, Godot and delivery workflow](docs/development.md) is the recommended route. Unity/UE samples and WSL are auxiliary workflows.

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
- Runtime-loaded Oracle MySQL 8.4 and explicitly selected SQLite database plugins.
- DBAppMgr range sharding, worker-rebuilt manager HA, and Reviver supervision for
  CellAppMgr, BaseAppMgr, and DBAppMgr.
- Tracy instrumentation, baseline stress drivers, and trace comparison tools.

The primary Oracle MySQL 8.4 backend is implemented. DB replication, shard auto-rebalance,
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
                   Oracle MySQL 8.4 / SQLite
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

`BgTaskManager` admits at most 4,096 tasks, counting queued work, running work and results
awaiting their main-thread callback. `AddTask` returns `RateLimited` at capacity and
`ReceiverGone` after `StopAccepting`; rejected submissions receive no completion callback.
Work returns `Result<void>` and each accepted task reports its result on the dispatcher thread.
`CancelPending` stops admission and reports `Cancelled` for work that has not started; it does
not interrupt running work. Exceptions at the existing callback boundary report `InternalError`.

Shutdown stops admission and normal simulation ticks while the dispatcher keeps processing
accepted work and replies. `ScriptEngine::Drain` / `Lifecycle.OnDrain` keep managed continuations
running without entity ticks or replication.
New script entity creation, space requests and teleports are rejected; duplicate creation
requests for an existing Real keep their confirmed acknowledgement.
BaseApp confirms session release and persistent logoff saves before
destroying entities; DBApp drains accepted operations and callbacks. The default deadline is
30 seconds. A known save failure exits unsuccessfully; expiry logs unconfirmed work and exits
with failure without blocking on worker destruction. A forced exit preserves only committed data.
Stop LoginApp first, then BaseApps, CellApps/managers, DBApps and machined, keeping storage and
control services available while their dependants drain. The cluster launcher waits 35 seconds
per shutdown group and reports nonzero exits or forced termination as failures.

Database admission is limited to 4,096 operations, including callbacks waiting for consumption.
Capacity rejection precedes execution (`RateLimited` / `kOverloaded`); draining rejects new work
as `ReceiverGone` / `kStopping`. `BeginDrain` and `PendingOperations` expose this lifecycle.
Checkout cleanup and automatic-load APIs return `Result` callbacks, including setters, so backend
errors cannot become empty successful results. Database plugins require `AtlasCreateDatabaseV6`.
Immediate reliable UDP sends obey the configured send window. Deferred bundles retain at most
256 KiB per channel and reliability class; full admission returns `WouldBlock` without appending.
RPC calls distinguish capacity rejection (`RateLimited`), duplicate requests (`AlreadyExists`)
and a stopping registry (`ReceiverGone`), without sending a rejected request.
Native login and authentication return busy or transport errors immediately when sending is
rejected, clear the unfinished attempt, and permit a fresh login without waiting for a timeout.
`atlas_tool watch` flushes its response before reporting success. A stdout write failure returns
a nonzero exit code and a stderr error instead of silently succeeding without a response.
LoginApp suspends the three-second client idle timer while an admitted login is processing,
then restores it on completion. Existing RPC and client deadlines still apply; disconnects cancel
the pending login. This does not add an overall server login deadline.

## Requirements

Windows:

- Visual Studio 2022 17.14 stable with MSVC 14.44 (patch updates allowed), with the Desktop development with C++ workload; managed projects use the .NET SDK CLI on every generator
- CMake 3.28+
- .NET 10 SDK
- Python 3.11.8+
- Git

Linux:

- GCC 13 on Ubuntu 24.04 LTS x86-64 (Clang is auxiliary)
- CMake 3.28+
- Ninja for the debug preset
- Pinned OpenSSL 3.5.9 SDK built with the command below (requires Perl and make)
- .NET SDK satisfying `global.json` (exactly 10.0.404);
  distribution defaults must pass the SDK check rather than replace this selection.
- Python 3.11.8+
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

The standalone OpenSSL SDK builder pins the official 3.5.9 archive and SHA-256,
builds static libraries, runs upstream tests, and preserves the license, authors and source
receipt under `build/openssl/3.5.9/Debug`. Use `tools\bin\build_openssl.bat --config Debug`
on Windows or `bash tools/bin/build_openssl.sh --config Debug` on Linux x64. Native Perl and
NASM are required on Windows; `--perl` / `--nasm` select existing tools. `--archive` supports
offline builds, and `--help` describes the isolated prefix and Release build options.
Prepare the SDK before configuring Atlas: Debug builds select Debug, other build types select
Release (an unset build type selects Debug). `-DOPENSSL_ROOT_DIR=<SDK directory>` overrides
the location. Missing or wrong-version SDKs fail configuration without a system-library fallback.
The builder requires Python 3.11.8+; output binaries retain OpenSSL notices in `licenses/OpenSSL`.
Building this SDK alone does not enable encryption on running listeners.

The native `TlsContext` / `TlsStream` transport primitive fixes TLS 1.3, requires an explicit
client trust file and target DNS/IP identity, and validates certificate subject alternative names.
Server setup rejects missing certificates or mismatched keys. Each stream bounds each ciphertext
direction to 64 KiB and reports backpressure; incomplete or tampered records never reach application
reads. The stream primitive is not connected to `TcpChannel`; public RUDP uses DTLS below.
`test_tls_stream` generates isolated test
certificates with the pinned SDK and does not install trust roots in the operating system.
`TlsTransport::kDatagram` reuses the certificate contract with DTLS 1.2 and preserves datagram
boundaries in bounded BIO queues. Ciphertext MTU is 1200 bytes; `DatagramPayloadMtu()` reports
the usable application size after handshake. Oversized writes and truncating reads are rejected.
OpenSSL rejects replayed or invalid authenticated records; valid later datagrams remain usable.
The retransmission timeout API is available for dispatcher timers.
`NetworkInterface.ConfigureDtls` now provides explicit native shared-UDP integration before socket
creation. The configured interface rejects plaintext, decrypts before RUDP parsing, and encrypts
RUDP headers, ACKs, fragments and retransmissions. Callers must wait for `IsRudpReady` before sending;
early sends return `WouldBlock`. The dispatcher drives handshake retransmissions, allows at most
128 unfinished handshakes and expires them after 10 seconds. Draining peers retain their own DTLS
session; closed sessions are not reused for a fresh RUDP channel. Socket backpressure retains one
pending ciphertext datagram alongside the bounded BIO queue.
Unknown peers first pass OpenSSL's stateless DTLS Cookie exchange.
The Cookie binds source IPv4 address, UDP port and a 60-second timestamp to a context-local random
HMAC key. Invalid, expired or cross-context Cookies do not admit a persistent handshake. Unverified
requests receive at most one challenge no larger than their incoming datagram; they consume no
persistent session slots. The 128-handshake limit applies after address verification.


LoginApp and BaseApp public RUDP accept `--external-dtls-certificate <chain.pem>` and
`--external-dtls-private-key <key.pem>` (JSON keys `external_dtls_certificate` and
`external_dtls_private_key`). Both files are required together; invalid credentials reject startup.
`--production true` / JSON `is_production: true` requires DTLS when a public listener is enabled.
Invalid production boolean values are rejected. Development plaintext is available when DTLS is
not selected and production is false. The isolated cluster launcher accepts `--dtls-certificate`
and `--dtls-private-key` and starts public processes in production mode.
SDK ABI 3.1 adds `AtlasNetConfigureDtls`; desktop C# calls
`LoginClient.ConfigureDtls(trustFile, serverIdentity)` before login. Login, authentication and
handoff wait for DTLS. Explicit configuration failure prevents plaintext login; cancellation and
disconnect clear pending sends. Build the CMake `_deploy` targets before copying matching SDK DLLs.
The desktop `AtlasClient` facade forwards the same `ConfigureDtls` method before `ConnectAsync`.
Real-cluster C# integration tests select encryption with all four environment variables:
`ATLAS_TEST_DTLS_CERTIFICATE`, `ATLAS_TEST_DTLS_PRIVATE_KEY`, `ATLAS_TEST_DTLS_CA` and
`ATLAS_TEST_DTLS_SERVER_IDENTITY`. Partial configuration rejects before launching the cluster;
`ATLAS_TEST_BASE_ASSEMBLY` supplies the default fixture assembly. These tests use MySQL by default.

RUDP rejects datagrams exceeding the channel MTU before buffering or processing ACKs. Fragments
require a sequence number, a nonempty payload no larger than `MaxUdpPayload()`, and a valid index
and count before their sequence is recorded. Reassembly keeps the existing group cap and timeout.
The optional `CompressionFilter` limits application payloads to 16 MiB on send and receive;
compressed packets must contain exactly one complete zlib stream with the declared output length.
Malformed, trailing and oversized input returns an error. This filter is not installed on public
listeners by default.
Public Base/Cell RPCs must belong to the target entity type as well as satisfy direction and
exposure checks. CellApp repeats this validation after forwarding; another entity type's exposed
component method cannot authorize dispatch on the target's component slot.
Cross-entity `AllClients` calls require the receiving CellApp to confirm the caller's active Real
Witness has already published the target's Enter and has not observed Leave. Missing authority,
another space, a Ghost caller or an unseen target is rejected; reply RPCs receive `ReceiverGone`.
Cross-Cell visibility authorization is not implemented; Ghost coordinates are not proof of access.
Own-entity calls retain their existing behavior.

Keep cluster TCP/RUDP and machined management ports on a trusted private network, restricted by
deployment firewall rules. Internal transport is not encrypted. Workers accept machined query,
notification, Watcher and shutdown control messages only on their current machined TCP connection.
CellApp binds registered BaseApp trust to the current channel instance, clears it on disconnect,
and rejects stale or other-channel client RPC/movement forwards. Process-instance notifications
from machined invalidate replaced peer channels. Administrative tools must forward worker commands
through machined; directly sending management messages to a worker is rejected.

Direct CMake remains supported:

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

Native tests also support `-DATLAS_BUILD_CSHARP=OFF -DATLAS_BUILD_NET_CLIENT=OFF` in an isolated
build directory. Managed host/hot-reload tests follow the C# build option; native SDK tests follow
the client-library option. TLS/DTLS transport tests remain enabled. The default build includes all
managed and SDK tests.

| Preset | Purpose |
|---|---|
| `debug` | Fast iteration with Ninja Multi-Config, Debug, MSVC `/Z7`, PCH, and tests. |
| `release` | Optimized build with tests disabled. |
| `hybrid` | RelWithDebInfo for optimized debugging. |
| `profile` | RelWithDebInfo with Tracy and profiling helpers; tests disabled. |
| `vs` | Visual Studio solution under `build/vs/`, Debug default, with tests (Windows). |

Sanitizer presets are available where supported: `asan`, `asan-msvc`, `tsan`, and `ubsan`.

### Auxiliary: Linux build from Windows through WSL

Verify the Linux build against the same Windows clone — no second checkout, no manual sync. Source is read in place from `/mnt/<drive>/...`; build artefacts land in `~/atlas-builds/<preset>` inside the WSL filesystem so cross-FS writes don't cap compile speed. `sccache` is picked up automatically when present, mirroring CI.

Use an already configured WSL environment; enabling/installing WSL requires separate approval. Ubuntu 26.04 does not substitute for Ubuntu 24.04 acceptance.

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

On Linux or Git Bash, the cluster wrapper starts an isolated development stack with explicit business assemblies and keeps it alive until interrupted:

```bash
tools/bin/run_cluster.sh --base-assembly <your-base-assembly>
```

Scripted cluster validation:

```bash
tools/bin/test_cluster.sh --base-assembly <your-base-assembly>
# Windows
tools\bin\test_cluster.bat --base-assembly <your-base-assembly>
```

`test_cluster` also accepts `--base-assembly <script.dll>` to load a test script in
BaseApp. Pass an explicit assembly; a missing assembly fails before any cluster process starts.
The `atlas_rpc_fixture_base_dll_deploy` and
`atlas_rpc_fixture_client_dll_deploy` CMake targets build test-only generated RPC peers.
`--cell-assembly <script.dll>` adds one CellAppMgr and one CellApp; the matching
`atlas_rpc_fixture_cell_dll_deploy` target builds the Cell peer. Missing assemblies fail
before process startup. `--baseapps 2` starts a second BaseApp for handoff verification;
run manifests and READY output record the actual BaseApp count and endpoints.
`--cellapps 2` requires `--cell-assembly` and adds a second CellApp for recovery verification;
the database run manifest and READY output record the actual worker counts.
`--log-level debug` enables existing server diagnostics for this isolated run; C# fixtures accept
the same override through `ATLAS_TEST_CLUSTER_LOG_LEVEL`. The default remains `info`.
The test launcher's stdin accepts `kill-baseapp <internal-port>` for crash injection and confirms
termination with `KILLED`. It only terminates its own live BaseApp children.
`shutdown-baseapp <internal-port>` requests normal shutdown through machined and confirms
`SHUTDOWN` only after a zero process exit; expiry and a nonzero exit fail the test launcher.
`restart-baseapp <internal-port>` restarts a stopped owned BaseApp with its original arguments,
confirms `RESTARTED`, and retains earlier logs under the run directory. Confirmation means
process creation; tests separately wait for registration and check that old sessions cannot bind.
`kill-process <name>` and `restart-process <name>` provide the same crash/restart controls for
owned CellApps, DBApp and machined. Unknown names, already stopped targets and restarting a live
process are rejected. Restart reuses the original arguments and preserves logs; tests must wait
for registration and verify recovery separately.

For the standalone generated-mailbox test, set `ATLAS_BIN_DIR` to the absolute isolated
binary directory and use an empty test database through `ATLAS_DB_MYSQL_*`. The existing
C# cluster fixture starts and stops the processes; no Godot installation is required:

```bash
dotnet test tests/csharp/Atlas.Client.IntegrationTests --filter FullyQualifiedName~RpcMailboxRealTests
dotnet test tests/csharp/Atlas.Client.IntegrationTests --filter FullyQualifiedName~SessionRecoveryRealTests
```

This covers Client→Base and Client→Cell replies, Base→Cell and Cell→Base calls,
Base/Cell→Client notifications, asynchronous ordering, business errors, one-way calls,
timeout, cancellation, late replies and cleanup. Build all three RPC fixture targets first.

Manager recovery uses reports from surviving workers. CellAppMgr accepts space geometry only
from the registered CellApp address and channel, rejects condemned channels and trailing BSP
bytes, and requires the reporter to own a leaf and every leaf owner to be registered. A rejected
report leaves the current topology unchanged. The DBAppMgr HA verifier requires the recovery
window to be explicitly closed before declaring recovery healthy. CTest's `test_ha_verifiers`
runs the existing CellAppMgr and BaseAppMgr verifier unit tests together with the DBAppMgr checks;
these checks alone do not establish process crash or communication interruption coverage.
CellAppMgr retains the highest valid geometry while leaf owners re-register. Every owner must
report the same BSP, geometry version and nonzero freeze epoch on its current registered channel.
Reports are stable only when all expected local cells exist, are unfrozen at the same epoch and
have initialized SpaceData. The manager restores that version, epoch and broadcast baseline;
an unchanged topology does not create a new version. Missing or older confirmations keep the
space waiting; unstable or conflicting reports pause it. Interrupted topology changes are not
automatically completed: resolve worker state before restarting a blocked manager.
Each manager retains at most 1024 recovery records and
16 MiB of geometry/owner buffers. Duplicate reports do not accumulate; an equal-version geometry
conflict, lost source/owner, or exhausted byte budget pauses that space without falling back.
Paused and waiting spaces reject space creation/reuse, host resolution and topology publication; other
healthy spaces continue. Exhausting record slots closes admission for unknown spaces, even after
buffers drain, because their rejected version watermarks could not be retained. Resolve the worker
state and restart the manager to clear a blocked recovery; newer reports do not clear a conflict.
Existing worker ticks and cached entity routes continue while manager topology decisions are paused.
Watch `cellappmgr/ha/recovery_pending`, `recovery_bytes`, `recovery_blocked` and
`recovery_admission_closed`. The HA verifier rejects pending, blocked or unavailable recovery state.
BSP decoding allows at most 8191 nodes and depth 64; malformed or over-limit trees are rejected.
Machined registry queries use 64-bit request IDs and explicit success or failure results.
Concurrent queries do not replace each other; late and duplicate replies are ignored. At most
128 queries may wait per client, and exhausted IDs are never reused. Query timeout, disconnect,
send failure or overload does not mean an empty registry. Reviver does not count such failures
as missing processes or use them to authorize a cold start; a newer target generation also
invalidates an older audit. `atlas_tool list` exits with an error if a query fails. Rebuild
machined, servers and tools together for machined registration protocol 3 and the new query layout.
A disconnected machined watcher is not a Manager health failure. After registration reconnects,
Reviver requires a fresh successful audit that contains the target before using registry-based
health or absence checks; local process liveness and direct heartbeats remain active. TCP error/hangup events close
the failed connection so workers can re-register; closing during dispatch stops the remaining bundle.
BaseApp re-registers with its existing app ID on BaseAppMgr birth, clears the binding on the
matching death notification, and accepts registration acknowledgements only on its current channel.
CellApp also retains its app ID across manager death or disconnect and sends it on re-registration,
so worker identities do not depend on their return order.
Manager health probes and replies use unreliable datagrams with nonce matching. Their existing
periods, timeouts and failure thresholds handle loss; late replies do not prove current health.
This keeps liveness checks independent of a reliable channel's sequence state after recreation.

Reviver restarts an observed target only after its previous local process has exited. It pins local
process identity before termination and checks exit again before launching; unreadable liveness is
treated conservatively as still alive. A remote or unconfirmed target becomes `restart_blocked`
under `reviver/<manager>/status` and `/restart_blocked`, requiring operator isolation and recovery.
Automatic supervision requires a loopback target on the same host; configuring the same manager
for cold start on multiple hosts is unsupported. No cross-host fencing or automatic relocation is provided.
Linux termination uses a stable pidfd instead of a PID checked earlier; unavailable pidfd support
prevents automatic termination. Windows uses a retained process handle.

Common Watchers expose `app/process_instance`, `machined/connected` and
`machined/registration_generation` for process correlation. `network/pending_sends` counts
the internal network's pending sends. `app/shutdown/state` reports `running`, `draining`,
`failed`, `timed_out` or `complete`; inspect `pending_operations` and `remaining_ms` under
the same prefix. A failure can coexist with unfinished work; `complete` requires draining
and deregistration. Values are local observations, not a database reachability probe.
BaseApp, CellApp and LoginApp expose `rpc/pending`, `replies`, `pending_limit`, `reply_limit`,
`oldest_pending_ms` and `stopping`. The oldest age is zero with no waiters and is computed
when queried. These aggregate paths do not grow with entities or requests and contain no payloads.
`app/ready` and `app/not_ready_reason` report local prerequisites for new routable work:
manager registration, EntityID availability, known database routes, queue capacity and recovery
gates, as applicable to each role. An empty reason means ready. Shutdown always reports
`shutting_down`. Recovery or a paused space can report a degraded Manager even while unaffected
spaces continue serving. These fields do not change admission, send probes, guarantee remote
database reachability or promise that a particular account, entity or space request will succeed.
BaseAppMgr queries do not consume overload admission slots. RPC failures at debug level include
reply/request IDs, error code and elapsed milliseconds; correlate these with process logs and
the existing managed RPC trace ID. They omit RPC payloads and error text.
DBApp exposes `pending_operations`, `operation_limit`, `pending_write_requests`,
`account_session_requests`, `process_retirements` and `allocator_recoveries` under `dbapp/`.
CellApp reports `pending_offloads`, `uncertain_offloads` and `pending_teleports`; uncertain offloads
remain paused under the existing migration contract. BaseApp reports `incoming_handoffs`,
`session_revocations` and `pending_persist`. BaseApp and LoginApp also expose
`external_pending_sends` under their role prefix. Counts may overlap across lifecycle stages;
they are not a count of unique user requests. Existing per-space recovery Watchers remain available.

For repeatable framework recovery acceptance, supply an isolated MySQL database through
`ATLAS_DB_MYSQL_*` or `--database-config`, build the RPC Base/Cell fixtures, then run
`tools\bin\verify_framework_ha.bat --build framework-verify --scenario all` on Windows, or
`bash tools/bin/verify_framework_ha.sh --build framework-verify --scenario all` on Linux.
`managers` runs three Manager restarts and overlapping faults; `workers` runs DBApp, both CellApps
and machined crash/restart cycles. The tool reuses the existing HA verifiers and launcher, checks
one two-leaf space and two Real probes, and records per-fault logs under `.tmp/framework-ha-*`.
Initial and final diagnostic JSON snapshots verify nine processes are registered and locally
ready, with nonnegative queue counts within their reported limits.
It rejects SQLite and missing MySQL settings. It creates no database and deletes no data; use an
empty test database, since DBApp initializes its schema. BaseApp session/crash tests remain in
the existing `ClientHandoffRealTests` suite. This local topology does not verify cross-host fencing.

## Database

Prepare the pinned Oracle client before building with `tools\bin\prepare_mysql.bat`
(Linux: `bash tools/bin/prepare_mysql.sh`). See [Oracle MySQL setup and licensing](docs/oracle_mysql.md).
MySQL connections require `mysql_ssl_ca` (`--db-mysql-ssl-ca`) and always verify the CA chain
and the configured host identity. There is no plaintext or unverified mode.


Atlas loads SQLite and Oracle MySQL 8.4 plugins through the database factory. Oracle MySQL 8.4
is the default for development, integration, deployment and cluster acceptance, with multiple
DBApps sharing one logical database. `ATLAS_DB_MYSQL` defaults to ON and builds the plugin
using the pinned Oracle libmysqlclient C API. SQLite requires explicit selection and is limited to single-DBApp
lightweight development, quick smoke checks and independent tests, with WAL and busy-timeout support.
Use `database.type` in JSON or `--db-type` with `sqlite` or `mysql`.
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
Concurrent MySQL startup serializes schema initialization with a database-scoped advisory lock.
Both backends initialize only empty databases. Existing databases must have the matching
`schema_version` (MySQL 4, SQLite 3); missing or incompatible metadata rejects startup without
overwriting the version or upgrading the schema. Rebuild matching processes and use a new
isolated database; no old-format migration is provided. SQLite initializes its schema and
metadata in one transaction. Interrupted MySQL DDL may leave an incomplete database, which
must be inspected rather than automatically repaired or stamped as compatible.
Startup also checks the SHA-256 `storage_schema_digest`. It covers registered entity fields,
component inheritance and slots, structs, nested containers and their ordering; RPC declarations
are excluded. Saves currently contain full Base/Cell snapshots, including fields without
`persistent`, so those fields are checked too. A missing or different digest rejects startup
without replacing the stored digest. Changes to registered data schemas require a new database.
`blob_format_version` must also match (currently 2). Each entity snapshot has an explicit version,
Base/Cell section lengths and a SHA-256 checksum; truncated, altered, trailing and unversioned
data are rejected. New accounts use an encoded empty snapshot. Explicit saves and logoff saves
use the same envelope and include the last Cell backup. SHA-256 here detects corruption; it is
not a replacement for transport authentication.
ATDF definition files use version 4 with a whole-file SHA-256 checksum. DBApp rejects missing,
empty or corrupt definitions before opening storage. Login authentication, entity checkout and
writes carry the full entity-definition digest, which DBApp checks before database access;
BaseApp also refuses checkout completion if definitions changed while it was waiting.
Inline synchronized components carry their type name and flattened fields in the entity slot
descriptor instead of requiring a global component ID. Their fields and nested types participate
in both storage and definition validation; local components remain outside snapshot persistence.
Account-session payload version 2 stores recovery credential digests, controlled entity storage
identity and expiry. Grant issuance, consumption with rotation, and revocation share the ownership/version and
operation receipt transaction. An expired grant or a replay with a new operation ID is rejected;
a retry of the same confirmed operation returns its recorded result. Digest comparison uses
the existing OpenSSL constant-time primitive. Issuance expires after ten minutes using MySQL server time or the SQLite
host clock; clients cannot supply the expiry. Release or control ownership changes revoke grants.
A crash recovery claim additionally requires confirmed retirement of the old process and current
checkouts of both the account and persistent controlled entity in the claiming process. Missing
retirement or stale checkout versions refuse the claim; temporary entities cannot be claimed.
The native SDK ABI 3.2 adds `AtlasNetGetRecoveryCredential`, `AtlasNetResume` and
`AtlasNetLogout`; desktop C# exposes `GetRecoveryCredential()`, `ResumeAsync()` and
`LogoutAsync()`. After a transport loss, keep the credential in memory, call resume against
LoginApp, then authenticate against the returned BaseApp as with ordinary login.
LoginApp must have a nonzero `--internal-port` matching its registered cluster endpoint;
resume requests from an unregistered source address or process instance are refused. The
`test_cluster` launcher assigns this port explicitly. Its public DTLS port remains separate. A successful
resume consumes and rotates the credential before returning a new connection key. A live
owner reuses its entity; a confirmed retired owner restores the persistent identity through
checkout and the existing snapshot loader. Runtime EntityIDs may change. A temporary entity
cannot recover after its process dies. A valid checkout held elsewhere is not stolen.
When a controlled temporary entity is first saved, its confirmed DBID is bound to a new
recovery grant and delivered to the connected client. Save confirmation alone does not confirm
delivery of that grant; retain the latest credential received by the SDK.
Logout success means database revocation was confirmed; timeout or transport loss reports an
unconfirmed result. `Disconnect(Logout)` queues that same exchange; keep polling for its result.
Credentials expire ten minutes after issuance; fetching a cached credential does not renew it.
Do not log credentials or persist them in ordinary configuration. Password reauthentication,
logout and a change of controlled entity invalidate the previous credential.
Protocol version 2, storage schemas MySQL 4 / SQLite 3 and database plugin ABI V5 require matching rebuilt
processes and an isolated new database; no migration is provided.
`SessionRecoveryRealTests` requires MySQL and covers live and crashed owners, transferred control,
persistent target restoration, temporary target refusal, credential rotation and logout revocation.
SQLite smoke tests do not replace it.

MySQL pool connections recheck the storage schema, blob version and snapshot digest whenever
they are opened after startup. A mismatch rejects operations without initializing or updating
that storage; restoring the expected database allows a later connection attempt.

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
stalled work. Shutdown keeps BaseApp's event loop active until these confirmations finish
or the shutdown deadline expires.

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
or the pinned [OpenSSL EVP API](https://docs.openssl.org/3.5/man3/EVP_DigestInit/).
OpenSSL 3.0+ uses [Apache-2.0](https://openssl-library.org/source/license/index.html);
the pinned SDK and output binaries retain its license and source receipt.
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
MySQL and SQLite also persist account session ownership independently of entity checkout.
Versioned session operations and their receipts commit together; changed operation payloads,
retired processes and superseded ownership cannot reuse a previous grant. A lost commit reply
returns `CommitUnknown`; corrupt records fail instead of becoming offline sessions. An account
with an active or pending session cannot be deleted. Released session versions survive entity
deletion. DBApp validates the registered BaseApp instance, configured account type and shard,
and limits outstanding session database requests to 1024. The database API supports local moves
and prepare/accept/client-confirmation transitions. Login confirms a session grant before binding;
relogin and logoff confirm its release before reusing or destroying its entity. Cross-BaseApp
handoff changes client control ownership while retaining the account entity's storage and checkout.
The internal handoff receiver validates registered source/target instances, entity definitions,
and the database preparation record before accepting an unbound proxy. A hashed credential
only binds the client after the database confirms the target as active owner. Incoming handoffs
are limited to 1024; after 20 seconds without completion the receiver confirms abort or release
before freeing its reservation. Database failures retain the reservation and retry cleanup at
20-second intervals. Rejected replays of an unconfirmed operation retain its stable operation ID;
an ownership conflict needs a newer database version before that operation can be discarded.
Before preparing login, BaseApp reads the account session and asks its recorded owner to revoke
it when active. Revocation checks the registered requester and owner process instances, account
and ownership version; success is returned after database confirmation. An unavailable owner,
changed version or uncertain commit rejects this login instead of assuming the session is offline.
Login checks and incoming revocations each have a limit of 1024 outstanding requests. Cancelled
login checks cannot continue into checkout; cancelling a wait does not undo a completed revocation.
Database plugins must export `AtlasCreateDatabaseV6`; rebuild both plugins and the server.
The loader rejects earlier plugin interfaces instead of invoking an incompatible vtable.
Birth and death notifications are accepted only on the current machined channel. `MachinedClient`
tracks peer process instances even for death-only subscriptions: a new instance at the same
address gets a fresh internal RUDP channel, and late deaths of the old instance are ignored.
BaseApp session queries match the DBApp channel, requesting process, account and operation. Cancelling
an already issued mutation stops waiting and reports `CommitUnknown`; it does not undo a grant.
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
    "mysql_database": "<isolated-development-database>",
    "mysql_database_id": "<32-lowercase-hex-logical-database-id>",
    "mysql_ssl_ca": "<trusted-database-ca.pem>"
  }
}
```


Set `ATLAS_DB_MYSQL_PASSWORD` in the launching process environment using your secret store
or a masked prompt; do not put credentials in commands, checked-in configs or logs.
DBApp reads this variable when the configured password is empty. Host, user and database
come from the configuration or CLI. A missing database/user/password, invalid connection
settings, unavailable plugin or failed connection stops startup; no SQLite fallback occurs.
The tools also accept `ATLAS_DB_MYSQL_HOST`, `PORT`, `USER`, `PASSWORD`, `DATABASE`, `DATABASE_ID` and
`POOL_SIZE` and `SSL_CA` (each with the `ATLAS_DB_MYSQL_` prefix) or `--database-config <local-json>`.
They pass the password through the child environment, excluding it from generated JSON.

`mysql_database_id` is required: generate it once with `tools\bin\database_recovery.bat new-id`
(or the matching `.sh` wrapper), then keep the same 32 lowercase hexadecimal characters in all
DBApps sharing that logical database. DBApp also accepts `--db-mysql-database-id`. Initialization
writes it only to an empty database; startup and every new pool connection reject missing or
mismatched identity. Copy this identity with backups and replication; do not generate a new one
when restoring or moving an endpoint. It identifies the logical data, not the physical server,
and does not prove replication freshness or fence an old primary.

Ordinary startup refuses empty MySQL databases. For the first creation only, explicitly set
`database.mysql_initialize` to JSON `true` or use `--db-mysql-initialize true`; reset it to false
after initialization. The launch tools accept `ATLAS_DB_MYSQL_INITIALIZE=true|false` and default
to false. Keep it false for production startup, restore and endpoint switching so a wrong empty
database cannot be claimed as new storage. Existing databases are validated even when initialization
is enabled; this flag never overwrites identity or upgrades metadata.

The development recovery tool uses the pinned Oracle `mysql`/`mysqldump` 8.4 SDK or
explicitly selected matching Oracle tools. Supply `--client` and `--dump-tool` paths if absent from PATH.
Stop all DBApps and other writers before backup or restore; `--quiesced` asserts this precondition.
It is not an online backup coordinator. Use a new private backup directory and an existing empty
target database. The source and target configs must name the same logical identity.

```bat
tools\bin\database_recovery.bat backup --database-config <source-json> --directory .tmp/db-backup --quiesced
tools\bin\database_recovery.bat restore --database-config <empty-target-json> --directory .tmp/db-backup --quiesced
tools\bin\database_recovery.bat verify --database-config <target-json> --directory .tmp/db-backup
```

Linux uses `bash tools/bin/database_recovery.sh` with the same arguments. The tool retains all
seven framework tables, including checkout versions, process retirement, operation receipts,
session credential digests and ID watermarks. It records dump SHA-256 and consistent-snapshot
row fingerprints, refuses nonempty targets and never deletes partial restores. Failure leaves
the target isolated for inspection; use another empty target for a new attempt. Only import trusted
backup files. Dumps contain account records and entity data: keep them private and out of Git.
Verification compares stored bytes and metadata; normal entity loading still validates snapshot
contents and entity definitions. Do not admit traffic until native startup and verification pass.

Production backups and replication remain database-managed. Before switching an endpoint,
fence the old primary and all writers, confirm the promoted database identity and replication
position, then restart or reconnect DBApps and verify readiness. The framework does not select a
primary, reconcile divergent databases, erase checkout ownership or retry unconfirmed writes.
Restore only successfully saved state; a backup cannot reconstruct subsequent transactions.

```bat
rem MySQL is the default; use an existing isolated development database and local config.
tools\bin\test_cluster.bat --database-config <local-dbapp-json> --base-assembly <your-base-assembly>
tools\bin\run_login_stress.bat --database-config <local-dbapp-json>
tools\bin\run_world_stress.bat --database-config <local-dbapp-json>
rem Explicit single-DBApp quick smoke; this is not MySQL acceptance.
tools\bin\test_cluster.bat --db-backend sqlite --base-assembly <your-base-assembly>
dotnet build/debug/csharp/Atlas.Tools.DefDump.dll --assembly <your-base-assembly> --out .tmp/sqlite-smoke/entity_defs.bin
bin\debug\atlas_dbapp.exe --type dbapp --db-type sqlite --db-sqlite-path .tmp/sqlite-smoke/store.sqlite3 --entitydef-bin-path .tmp/sqlite-smoke/entity_defs.bin
```

Linux uses the matching `tools/bin/*.sh` wrappers and `bin/debug/atlas_dbapp` with the same
arguments. SQLite-only builds may explicitly set `-DATLAS_DB_MYSQL=OFF`; rebuild the selected
output directory before running. The real-cluster C# fixture defaults to MySQL; an explicit
`ATLAS_TEST_DB_BACKEND=sqlite` selects smoke coverage. Tools emit `database-run.json` with the
backend, topology and scope, and include the backend in console/READY output. A smoke result
is marked `sqlite_smoke_only`, never a capacity or shared-storage acceptance result.

DBApp requires a nonempty ATDF definition file with a nonzero entity-definition digest before
opening the database. The cluster tools export it from their selected Base assembly into the run
directory using the matching build's DefDump. `test_cluster` requires `--base-assembly`;
login/world stress use their stress Base assembly by default and accept an explicit override.
The client digest comes from that export, never a generated source file in another build's `obj`.
DefDump checks the generated offline-export contract before loading an assembly, then suppresses
generated native registration only inside its own process. Older generated assemblies require a
rebuild. Export only trusted assemblies: other user-written module initializers can still run.

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

Entity RPC request and reply headers use 64-bit request IDs. Generated IDs are not reused
within the server process; exhaustion rejects new calls. Native ABI version 11 requires
rebuilding the runtime, server and generated callers together; old RPC headers are unsupported.
The CLR host checks Bootstrap and server lifecycle ABI versions before callback/lifecycle binding.
Managed server and desktop initialization also check the loaded engine DLL's ABI before
registration. A missing version entry or a mismatch stops initialization with an explicit error.

Base scripts can await `PersistToDbAsync(ct)` as `AtlasTask<RpcReply<long>>`; success contains
the database-confirmed DBID. A definite database rejection fails the reply; an unconfirmed
commit or DBApp disconnect returns `OutcomeUnknown` while native recovery keeps the write
fenced. Cancellation and timeout stop waiting without undoing an issued write.
`PersistToDb()` only submits a request and throws when local admission fails.

Cell scripts use `AddProximity(range, userArg)` and override `OnProximityEnter` /
`OnProximityLeave`. Events run through the existing main-thread queue after native range
updates return, so callbacks may cancel controllers safely. Execution rechecks the original
entity and its lifecycle; destroyed, Ghost or cancelled entities receive no event. Invalid
or negative radii return zero; zero radius is supported. Cancel a controller with
`CancelController(id)`. Queued callback exceptions are logged through the existing queue.

`StartTimer` accepts only finite, positive intervals and returns zero on rejection. Timer
notifications run synchronously on the main thread; script exceptions are logged without
leaving a native CLR error pending. Ghost, destroyed and lifecycle-cancelled entities do not
run timer callbacks. Hot reload cancels the old instance's native timers before unloading;
scripts recreate timers in `OnInit(isReload: true)`.

The optional EDR SDK exposes both named and inline components through
`AtlasEdrEntityComponentAtSlot`; inline components have type ID zero and a slot-owned schema.
Each EDR context accepts one load attempt. Failure clears partial definitions; retry with a
new context. After success the schema remains read-only and returned views live until destroy.
Server `NativeApi` calls must run on the main tick thread; the thread guard asserts this in Debug,
and Release callers must follow the same contract. Input spans are borrowed for the call;
async persistence copies the snapshot before returning. Native snapshot consumers copy managed
output before the next serialization callback or runtime reset can release its pinned buffer.
`Time.DeltaTime` reads the owning BaseApp/CellApp's `GameClock.FrameDelta` in seconds, including
its time scale; it is zero before the first tick, while paused, and after a clock reset.
`Time.ServerTime` is monotonic time in seconds with an unspecified epoch, not a UTC timestamp.
`SpaceMaster.Register` is a BaseApp operation; unsupported providers log an explicit error.
Login credentials must have nonempty usernames and credentials without embedded NUL bytes.
The desktop client rejects these inputs before native string conversion; native clients,
login message parsers and DBApp also reject them before account lookup or creation.
DBApp derives account credentials with Argon2id v19 (64 MiB, 3 iterations, 4 lanes,
16-byte random salt, 32-byte output) on background workers. Both database backends store
the resulting PHC record; raw legacy records and unsupported parameters cannot authenticate.
No credential migration is provided. The existing login credential argument remains the input;
clients do not send the server's stored PHC record. Authentication allows two active requests
through lookup, derivation and commit confirmation, plus 128 queued requests. A full queue returns
`ServerBusy`; shutdown rejects waiters with `ServerNotReady` and drains in-flight work.
`dbapp/auth_active` and `dbapp/auth_queued` expose these counts. Password storage hardening is
independent of transport encryption; configure the public DTLS listeners described above.
Pending RPC registrations reject duplicate keys without replacing the original waiter.
Replies must arrive on the registered channel; duplicate, late and wrong-channel replies
cannot complete that waiter. Cancellation stops waiting and prevents generated calls that
were already cancelled before sending; it does not undo remote work already executed.
CellApp registers managed RPC waits in its native registry. Cell-to-Base and Ghost-to-Real
calls bind the actual channel before sending, and report unavailable targets or send failures
to the waiter. Local Base/Cell replies use the same native reply handles as remote calls.
Completion, channel loss, entity destruction and registry shutdown invalidate those handles;
late script completions cannot dereference an expired channel or complete a second call.
ClientSession dispatches entity replies through its own waiting registry, used by generated
client mailboxes. Reset and Desktop transport disconnection complete pending calls with
ReceiverGone. Server C# calls and BaseApp relays share the native process's 64-bit ID
allocator, which survives script reloads and rejects exhaustion. BaseApp replaces
Client-to-Cell request IDs and restores the original client ID on replies.
Relay waits expire after 10 seconds. Client or Cell channel loss, malformed replies and
send failures remove relay state; missing Cell routes reject reply-style calls immediately.
One-way calls retain the existing bounded no-Cell queue.
Each native registry accepts at most 4096 outgoing waits and 4096 reply contexts;
each managed registry accepts at most 4096 waits. Capacity is released on completion,
cancellation or cleanup. Registration beyond the limit fails without replacing a waiter.
BaseApp and CellApp close the RPC registry before script teardown during finalization;
closed registries reject new requests, including registrations from cleanup callbacks.
Generated one-way mailboxes and component send methods return `RpcSendResult`: `Queued`,
`InvalidTarget`, `Overloaded`, `Stopping`, `SendFailed`, or `PartiallyQueued`.
`Queued` confirms local dispatch or transport admission, not remote execution. Broadcast
results cover all attempted BaseApp and Ghost channels. `PartiallyQueued` means at least
one channel accepted the call and another failed; the framework does not retry it.
An empty `OtherClients` recipient set succeeds without sending. Independent client SDK
RPC sends propagate transport errors instead of reporting success.
Standalone Godot/desktop hosts can pass a `ClientSession` to `LoginClient`; it wires
the session's Base/Cell send handlers and routes replies and disconnects to that session.
Use a separate session for each simultaneous connection and register generated client
definitions with `DefBootstrap.RegisterInto(session)`. The parameterless constructor
uses `DefaultSession` for a single connection. Dispose detaches its send handlers.
`ClientSession.CellReadyReceived` exposes the server's entity-specific route-ready notice;
it is a notification, not a guarantee that a later call cannot fail. Native delivery
accepts replies and state notifications only from the bound BaseApp channel, and rejects
them while disconnect is pending.
Transport loss completes the native disconnect on the next poll and reports reason 2
once; pending login/auth callbacks receive failure, and session RPC waits complete as
`ReceiverGone`. Unity's network manager also clears session waits on logout and destruction.
Native callbacks may disconnect or destroy their context: cleanup waits until the
active native call unwinds, further callbacks are suppressed on destroy, and recursive
`AtlasNetPoll` returns `BUSY`. A queued disconnect rejects new sends before cleanup.
Generated reply-style mailboxes always subscribe to the calling entity's lifetime token,
in addition to an explicit cancellation token. Entity destruction and script reload cancel
those waits even when the caller omits `ct`; cleanup detaches both token registrations.
Reply-style dispatch reports synchronous script exceptions and asynchronous reply
serialization failures as `RemoteException`, consuming the native reply handle.

## Entity migration boundaries

Await `GiveClientToAsync(destinationId)` for local client handoff. It confirms the database
ownership change before moving the binding and returns `RpcReply<bool>`. An uncertain commit
returns `OutcomeUnknown`; the source retains its binding with input frozen until revocation
confirms the recorded operation's result. Cancelling the script wait does not undo the transfer.
Local client handoff rejects a destroying entity, an occupied destination, or a destination
with a prepared session. Rejection preserves both bindings. Clearing a session also revokes
its retired keys; destroying entities cannot authenticate. `CurrentCell` only changes routing
and drains queued calls for a registered source channel and a non-stale, consistent epoch.
Space-host replies must match the teleport's entity, destination space and captured entity epoch.
An entity can have only one pending teleport lookup; exhausted migration epochs or lookup IDs
reject new requests. Lookup IDs are not reused within a process.

Cell migration acknowledgements include the attempted epoch and must match the destination
channel. Timeout or channel loss leaves the source as a passive Ghost with its rollback
snapshot; only a matching explicit rejection restores the source Real. A late matching success
can still finish the transfer. There is no automatic takeover when the result is unknown.
Ghost deletion, position, delta, snapshot and next-Real hints require the current Real address
and bound channel; messages from a previous owner or replaced channel are ignored.
Ghost deletion, position, delta and snapshot messages also match the current entity epoch,
so returning to the same host does not make an earlier migration's data valid again.
Ghost creation carries the entity migration epoch. Ownership notifications carry the previous
host and a newer epoch, and must arrive on the new host's registered channel. They can precede
the old host's next-Real hint; late hints and repeated ownership notifications cannot rewind it.
Each Ghost buffers up to 16 future ownership transitions and drains them when the preceding
owner arrives, preserving the per-entity ownership chain across channels. Overflow is logged
and refused; missing predecessors never authorize a guessed owner. Promotion clears this queue,
and queued transitions from condemned channels cannot take ownership.
Explicit migration rejection republishes the retained source ownership epoch to its ghosts.
These Ghost and space-bootstrap wire layouts require rebuilding all servers together.
BaseApp space bootstrap explicitly reuses an existing primary Cell or waits for concurrent
creation of the same space. A conflicting space-master type is rejected; ordinary create
requests still reject duplicates. Creation callbacks match the Manager channel and space,
complete once, and fail on send errors, disconnect, or a five-second timeout. Both ends bound
pending space creation at 1,024 requests; unavailable or exhausted request IDs fail explicitly.
Invalid controller state rejects arrival; failed rollback restoration retains the Ghost and
snapshot instead of activating a partially restored Real.
An unconfirmed Offload send also leaves a passive source Ghost and snapshot; a local send
error does not prove that no fragments reached the destination. Same-process teleports retain
the source until target restoration succeeds. Rejection restores the source in its original
space and pose; failed rollback retains its Ghost and snapshot, and never counts as success.
The receiving process keeps the latest result per entity, including after destruction, so a
duplicate cannot recreate a destroyed entity. These in-memory receipts are bounded at 65,536
distinct entity IDs per process; reaching the bound rejects new IDs with `overloaded`, while
existing IDs can continue migrating. Receipts are not durable restart recovery. Rebuild all
CellApps together for the new acknowledgement layout; old layouts are unsupported.

For a remote proxy, await `GiveClientToAsync(new ClientHandoffTarget(entityId, baseIp,
internalPort, processInstance))`. Supply the registered BaseApp's internal IPv4 value
(`Address::Ip()`), port and current process identity; no global entity directory is provided.
`TryGetClientHandoffTarget(out target)` describes a live local proxy using its registered
BaseApp instance; it returns false before registration or while the entity is being destroyed.
The Godot `AtlasClientRoundTrip` verification scene also supports a handoff mode: start the
fixture cluster with `--baseapps 2` and set `ATLAS_GODOT_TEST_HANDOFF=1` for the Godot process.
Set `ATLAS_RPC_FIXTURE_DIR` to the same empty, isolated local directory for the fixture cluster
and Godot. Each fixture BaseApp publishes a Probe endpoint there; the test selects the other
process explicitly, independently of login allocation. Fixture Cell notification records verify
that a revoked client cannot change state. These files contain no credentials and are test-only.
Both fixture BaseApps create their Probes in the same space, exercising concurrent bootstrap.
It checks target identity rejection, switching, target RPC and revocation after relogin.
The source freezes input, prepares the database session, waits for target acceptance, then
redirects the client to the target's registered external endpoint. Only target authentication
confirmation releases the source binding. Rejection restores input only after a confirmed
database abort; an unknown outcome keeps the source frozen. The account entity and its checkout
stay at the source, and neither source nor target entities are implicitly destroyed.

The native SDK (`atlas_net_client`, C ABI `0x03020000`) supports this switch for Godot and other
DLL hosts. It clears old entities, space data and RPC waits, rejects sends during `Handoff`,
verifies the target channel, entity and process identity, and delivers `EntityTransferred`
before `CellReady`. A failed or
timed-out authentication disconnects the client; reconnect through login rather than assuming
the old connection is still authoritative. Initial authentication advertises handoff support;
the standalone `atlas_client` CLI does not support remote switching and is rejected before
preparation. Rebuild all SDKs and servers together for the authentication layout and native API.
Remote script waits allow 40 seconds; cancelling that wait does not undo remote execution.
RUDP interfaces do not share bound endpoints; binding an endpoint already owned by another
RUDP interface fails with `AddressInUse`, including explicit client source-port bindings.
TCP frames and UDP/RUDP datagrams use connection protocol version 2, prefixed by `ATLS` and
a little-endian 16-bit version. TCP then carries its 32-bit payload length; UDP/RUDP retain
their existing payload/header after the prefix. Incompatible TCP peers are disconnected;
incompatible or truncated datagrams are dropped before a peer channel is allocated. RUDP MTU
and fragmentation include the six prefix bytes. Servers, SDKs and protocol tools must be rebuilt
together; unversioned traffic has no compatibility path. Mesh gossip retains its separate version.

## Profiling and Stress

The `profile` preset enables Tracy instrumentation and profiling helpers. The default baseline runs 200 clients for 120 seconds and writes captures under `.tmp/prof/baseline/`.

Linux builds keep Tracy's shared library and loader symlink beside the server so
managed profiling resolves the same runtime as native code, without `LD_LIBRARY_PATH` overrides.

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
