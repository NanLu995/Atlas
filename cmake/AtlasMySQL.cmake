if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR
   (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  message(FATAL_ERROR "Pinned Oracle client supports Windows/Linux x86-64; "
      "use ATLAS_DB_MYSQL=OFF for client-only builds")
endif()
set(_mysql_version "8.4.13")
if(WIN32)
  set(_mysql_platform windows)
  set(_mysql_package "mysql-${_mysql_version}-winx64")
else()
  set(_mysql_platform linux)
  set(_mysql_package "mysql-${_mysql_version}-linux-glibc2.28-x86_64-minimal")
endif()
set(ATLAS_MYSQL_ROOT
    "${CMAKE_SOURCE_DIR}/build/oracle-mysql/${_mysql_version}/${_mysql_platform}/${_mysql_package}")
if(NOT EXISTS "${ATLAS_MYSQL_ROOT}/.atlas-verified.json")
  message(FATAL_ERROR "Oracle MySQL ${_mysql_version} SDK missing; "
      "run tools/bin/prepare_mysql.bat (Windows) or tools/bin/prepare_mysql.sh (Linux)")
endif()
file(STRINGS "${ATLAS_MYSQL_ROOT}/include/mysql_version.h" _mysql_header_version
     REGEX "^#define MYSQL_SERVER_VERSION[ \t]+\"8\\.4\\.13\"")
if(NOT _mysql_header_version)
  message(FATAL_ERROR "Oracle MySQL SDK version mismatch; run the pinned prepare_mysql tool")
endif()
add_library(OracleMySQL::Client SHARED IMPORTED GLOBAL)
set_target_properties(OracleMySQL::Client PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ATLAS_MYSQL_ROOT}/include")
if(WIN32)
  set_target_properties(OracleMySQL::Client PROPERTIES
      INTERFACE_COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN"
      IMPORTED_IMPLIB "${ATLAS_MYSQL_ROOT}/lib/libmysql.lib"
      IMPORTED_LOCATION "${ATLAS_MYSQL_ROOT}/lib/libmysql.dll")
  set(ATLAS_MYSQL_RUNTIME_FILES
      "${ATLAS_MYSQL_ROOT}/lib/libmysql.dll"
      "${ATLAS_MYSQL_ROOT}/bin/libssl-3-x64.dll"
      "${ATLAS_MYSQL_ROOT}/bin/libcrypto-3-x64.dll")
else()
  configure_file("${ATLAS_MYSQL_ROOT}/lib/libmysqlclient.so.24"
      "${ATLAS_BIN_ROOT}/libmysqlclient.so.24" COPYONLY)
  foreach(_name IN ITEMS libssl.so.3 libcrypto.so.3)
    configure_file("${ATLAS_MYSQL_ROOT}/lib/private/${_name}"
        "${ATLAS_BIN_ROOT}/private/${_name}" COPYONLY)
  endforeach()
  set_target_properties(OracleMySQL::Client PROPERTIES
      IMPORTED_LOCATION "${ATLAS_BIN_ROOT}/libmysqlclient.so.24")
endif()
set(_mysql_licenses "${ATLAS_BIN_ROOT}/licenses/OracleMySQL")
file(MAKE_DIRECTORY "${_mysql_licenses}")
foreach(_file IN ITEMS LICENSE README docs/INFO_SRC docs/INFO_BIN)
  get_filename_component(_name "${_file}" NAME)
  configure_file("${ATLAS_MYSQL_ROOT}/${_file}" "${_mysql_licenses}/${_name}" COPYONLY)
endforeach()
configure_file("${ATLAS_MYSQL_ROOT}/.atlas-verified.json"
    "${_mysql_licenses}/atlas-source.json" COPYONLY)
file(READ "${ATLAS_MYSQL_ROOT}/.atlas-verified.json" _mysql_manifest)
string(JSON _mysql_source GET "${_mysql_manifest}" source_archive)
if(NOT EXISTS "${_mysql_source}")
  message(FATAL_ERROR "Corresponding MySQL source missing; rerun prepare_mysql before FOSS delivery")
endif()
configure_file("${_mysql_source}" "${_mysql_licenses}/mysql-${_mysql_version}.tar.gz" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/build/openssl/3.5.9/openssl-3.5.9.tar.gz"
    "${_mysql_licenses}/openssl-3.5.9.tar.gz" COPYONLY)
