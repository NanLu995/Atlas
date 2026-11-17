# Oracle MySQL setup and FOSS delivery

The target is Oracle MySQL 8.4.13 LTS with its C API, not Connector/C++.
SQLite remains an explicitly selected single-DBApp development and smoke backend.
Oracle database acceptance must be recorded separately from historical MariaDB results.

```bat
tools\bin\prepare_mysql.bat
```

Linux uses `bash tools/bin/prepare_mysql.sh`. The tool supports x86-64 Windows/Linux,
checks pinned SHA-256 values and prepares an SDK under `build/oracle-mysql/8.4.13/`.
`--cache` selects an existing download cache. It also retains the matching Oracle source
archive; existing MariaDB installations and databases are not removed.

Pins were established after verifying Oracle detached signatures with release key fingerprint
`BCA43417C3B485DD128EC6D4B7B3B788A8D3785C`. Sources are the
[official download site](https://dev.mysql.com/downloads/mysql/8.4.html) and its Oracle CDN.
Reproduction checks the committed package hashes; a different package is rejected.

This project has authorized the Community client only for FOSS delivery with complete
corresponding source, under GPLv2 and the Universal FOSS Exception. This is not permission
for proprietary distribution. Atlas keeps its MIT license; the client retains its own license.
Preserve the official LICENSE, exception, copyright notices and corresponding source and
build materials when distributing. A root MIT file alone does not establish compliance.
Third-party source and runtime obligations must also be checked against the actual deployment.

See the [8.4.13 license](https://raw.githubusercontent.com/mysql/mysql-server/mysql-8.4.13/LICENSE)
and [Universal FOSS Exception](https://oss.oracle.com/licenses/universal-foss-exception/).
Preparing an archive does not declare the complete product distribution legally cleared or
database authentication, TLS, backup and failure handling tested.

## Build and runtime

Run SDK preparation before the normal build wrapper. It uses Oracle's unmodified shared
C API: Windows `libmysql.dll` and its import library, Linux `libmysqlclient.so.24`.
No MariaDB driver or Connector/C++ is linked. Client-only cross-platform presets disable
the database plugin; the pinned server SDK supports Windows/Linux x86-64.

The build deploys the client's shared OpenSSL 3.5.9 dependencies: Windows alongside the
executables, Linux in `private/` with the original client RPATH. Atlas still uses its own
pinned static OpenSSL SDK. zlib and Zstandard are bundled in the client and covered by the
Oracle LICENSE and source archive. OS libraries and the Windows C++ runtime are prerequisites,
not copied from arbitrary system installations.

`bin/<build>/licenses/OracleMySQL/` retains the official LICENSE (including the FOSS exception
and bundled notices), INFO_SRC, INFO_BIN, package/source hashes and matching MySQL/OpenSSL
source archives. Keep this material with delivery, along with complete corresponding Atlas
and other FOSS source and build instructions. This directory alone does not license closed
application code or certify a future product's entire distribution. The server's GPL license
is separate from the linked client's GPLv2 plus FOSS exception. Private self-operated deployment
is not permission for later proprietary distribution; that route requires a separate review.

## Connection and validation

Use `database.type=mysql`, a MySQL 8.4 server, a dedicated account, the expected
`mysql_database_id` and `mysql_ssl_ca`. The CA path is also available as `--db-mysql-ssl-ca`
or `ATLAS_DB_MYSQL_SSL_CA` in the existing cluster tools. The host must match the certificate.
TCP (including localhost), TLS 1.2/1.3 and VERIFY_IDENTITY are mandatory, including recovery tools; unavailable servers,
wrong credentials, trust/identity failures and incompatible storage reject startup without
SQLite fallback. Server test fixtures enforce encrypted transport and caching_sha2_password.

```bat
tools\bin\test_mysql.bat -- ctest --test-dir build/debug -C Debug -R "^(test_mysql_database|test_dbapp_checkout_cleanup)$" -V --output-on-failure
```

Linux uses the matching `.sh` wrapper. `--docker` uses the exact Oracle 8.4.13 image and digest
pinned in the fixture; without it the existing isolated SDK server is used. Missing runtime
libraries are environment failures, never an instruction to install or substitute a database.
The fixture creates a fresh directory, generated credentials, a short-lived test CA and
certificate, exports test connection settings and stops only its own server after the command.
It never reuses an existing directory. Data and logs remain for inspection; do not distribute
test CA keys or backups. Use only the dedicated fixture database: native database tests reset
its Atlas tables. `mysql`/`mysqldump` discovery prefers the pinned SDK and rejects MariaDB or
other version series. Production capacity, remote CI and HA are separate acceptance steps.
