# Development, Godot and delivery

Use the wrappers in `tools/bin`. Their Python implementations use the standard library.
The main route is Windows x86-64 or Ubuntu 24.04 LTS x86-64, Oracle MySQL 8.4 LTS,
and a desktop Godot .NET client. Run from the Atlas repository root.

## Environment and build

Windows uses stable VS 2022 17.14 with MSVC 14.44; patches within these series are allowed.
Linux uses GCC 13. Python 3.11.8+, CMake 3.28+ and Ninja 1.11+ are required.
The .NET SDK is selected exactly by `global.json`; .NET 10 runtime servicing patches are allowed.
Godot is owned by the consuming project: its editor and Godot.NET.Sdk must be compatible.
Atlas imposes no Godot patch-version check. A different engine version needs the same smoke tests.
Passing version checks establishes prerequisites, not full platform acceptance.

```bat
tools\bin\build_openssl.bat --config Debug
tools\bin\prepare_mysql.bat
tools\bin\build.bat debug
```

On Ubuntu use `bash tools/bin/<name>.sh` with the same arguments. Install prerequisites
explicitly; the build never changes system settings. `setup_linux` installs system packages
and is an administrator tool, not an automatic build step. WSL/other distributions are auxiliary.
`--build-dir build/<unique-name>` isolates a build. Use a fresh name when changing toolchains.
Debug is daily development; Release is delivery; Profile and Sanitizers are diagnostic builds.
Keep one configuration per build directory. CI and local builds share the SDK declaration.

## Isolated database and checks

```bat
tools\bin\test_mysql.bat -- ctest --test-dir build/debug -C Debug --output-on-failure --no-tests=error
```

The fixture starts only its own pinned MySQL server, creates a dedicated database account,
provides TLS settings to its child command and shuts its server down afterwards. It retains
its isolated data/log directory for diagnosis; do not distribute its keys or backups.
Linux CI uses the pinned Docker fixture. This does not connect to an existing development database.
See [Oracle setup and license boundaries](oracle_mysql.md). Fixture-only RELOAD and trusted function creation support authentication-cache and fault-trigger tests; they are not production account requirements.

For an existing isolated developer database, supply `--database-config <local.json>` or
`ATLAS_DB_MYSQL_*`. Configuration fields take priority over environment defaults;
`--db-backend` must agree with the configuration. Passwords belong in process environment,
not committed JSON. CA, host identity and logical database ID are mandatory. Missing settings
or failed connections stop startup without SQLite fallback.

SQLite is explicit single-DBApp smoke coverage: `--db-backend sqlite` for cluster tools,
or `ATLAS_TEST_DB_BACKEND=sqlite` for managed real-cluster tests. Its results do not validate MySQL.
Quick checks are the relevant unit/tool tests; formal acceptance includes database contracts,
managed tests, cluster readiness/shutdown, Godot round trips and formatting checks.

## Development cluster and Godot

Build your Base and optional Cell assemblies, then select them explicitly:

```bat
tools\bin\test_mysql.bat -- python tools/run_cluster.py --base-assembly <Base.dll> --cell-assembly <Cell.dll>
```

With an already configured isolated database, use `tools\bin\run_cluster.bat` directly.
The launcher prints `READY` with the allocated login port and log directory; Ctrl+C stops
only its own processes. It does not load stress/MVP business assemblies by default.
Entity definitions are exported from the selected Base assembly into this run's directory.
Supply existing resources and certificates explicitly; do not copy an old runtime directory.

```bat
tools\bin\client_sdk.bat export --build-dir build/debug --base-assembly <Base.dll> --client-assembly <Client.dll> --output .tmp/sdk-debug
tools\bin\client_sdk.bat verify --bundle .tmp/sdk-debug
tools\bin\client_sdk.bat install --bundle .tmp/sdk-debug --project <GodotProject>
```

Choose a new export directory for each update. For the existing RPC verification scene use
`--fixture` and the matching RpcFixture Base/Cell assemblies. Installation refuses obsolete
native/managed libraries and lists their exact paths; removal requires separate approval.
Do not install while the client/editor has the SDK loaded. A failed installation restores
previous files. Keep the exported entity definition with the server's matching business assembly.

The SDK contains Shared contracts, Client entities/RPC, Desktop/ClrHost glue, the native
network client, its runtime dependencies and license/source material. RpcFixture is test-only.
Its manifest records hashes, ABI, entity-definition digest and source/build provenance.
Checksums do not replace the existing runtime ABI and protocol checks.

Godot hosts a `ClientSession` and `LoginClient`; poll the client and drain the Atlas loop
from the main loop. Await `LoginAsync` before entity/RPC operations. Use the generated business
client proxies. Disconnect on session exit and dispose clients and the loop on scene teardown.
Reuse `Verification/Automated/AtlasClientRoundTrip.tscn` in the existing Godot project for
authentication, RPC and disconnect coverage. Its README lists required test environment variables.
For failures inspect SDK verification first, then the READY port, CA/identity, DBApp logs,
entity-definition digest and generated RPC contract. Never log credentials.

## Delivery

Prepare Release OpenSSL, build the Release preset and export a Release SDK. Configure a
separate Release verification directory with `ATLAS_BUILD_TESTS=ON`; the delivery tool builds
it and runs CTest before creating a new archive:

```bat
tools\bin\build.bat release --build-dir build/release-check --with-tests
tools\bin\test_mysql.bat -- python tools/package_delivery.py --build-dir build/release --verification-build build/release-check --sdk .tmp/sdk-release --base-assembly bin/release/<Base.dll> --output .tmp/atlas-delivery.zip
```

The archive includes matching server/client files, definitions, engineering source, notices and provenance.
It does not collect Unity/UE projects or art assets. SDKs exported with `--fixture` mark the archive
as verification-only; they are not game product releases.
Oracle libraries retain the existing complete-source FOSS delivery boundary; this is not
authorization for proprietary redistribution. Packaging is not a legal clearance or proof of
Godot export/AOT, mobile, Web, capacity or HA acceptance.
Record commands and results separately for Windows/Linux: passed, code failure, environment
blocked, not run, or existing platform-specific skip. Ubuntu 26.04 cannot substitute for 24.04.

## Updating the baseline

Record actual tool versions when using allowed patch updates. A new compiler series,
.NET SDK, dependency archive or database fixture revision requires an explicit change to
its existing declaration and hashes, followed by affected build/database/client regressions.
Update local and CI selection together. Godot upgrades remain in the consuming project and
must repeat its build and round-trip scene. A fixed baseline is not a security assessment.
