include_guard(GLOBAL)

set(_openssl_config Release)
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_openssl_config Debug)
endif()
set(OPENSSL_ROOT_DIR "${CMAKE_SOURCE_DIR}/build/openssl/3.5.9/${_openssl_config}"
    CACHE PATH "Pinned OpenSSL SDK built by tools/build_openssl.py")
set(_openssl_crypto lib/libcrypto.a)
set(_openssl_ssl lib/libssl.a)
if(WIN32)
  set(_openssl_crypto lib/libcrypto.lib)
  set(_openssl_ssl lib/libssl.lib)
endif()
foreach(_file IN ITEMS atlas-source.json include/openssl/opensslv.h
    "${_openssl_crypto}" "${_openssl_ssl}" share/licenses/OpenSSL/LICENSE.txt
    share/licenses/OpenSSL/AUTHORS.md)
  if(NOT EXISTS "${OPENSSL_ROOT_DIR}/${_file}")
    message(FATAL_ERROR "Pinned OpenSSL SDK missing ${OPENSSL_ROOT_DIR}/${_file}. "
      "Run tools/bin/build_openssl.bat --config ${_openssl_config} on Windows or "
      "bash tools/bin/build_openssl.sh --config ${_openssl_config} on Linux.")
  endif()
endforeach()

file(STRINGS "${OPENSSL_ROOT_DIR}/include/openssl/opensslv.h" _openssl_version
    REGEX "^# *define OPENSSL_VERSION_STR ")
if(NOT _openssl_version MATCHES "\"3\\.5\\.9\"")
  message(FATAL_ERROR "Atlas requires the pinned OpenSSL 3.5.9 SDK: ${OPENSSL_ROOT_DIR}")
endif()
set(OPENSSL_FOUND TRUE)
set(OPENSSL_VERSION "3.5.9")
set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include" CACHE PATH "OpenSSL headers" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT_DIR}/${_openssl_crypto}"
    CACHE FILEPATH "OpenSSL Crypto" FORCE)
set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT_DIR}/${_openssl_ssl}"
    CACHE FILEPATH "OpenSSL SSL" FORCE)
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_ROOT_DIR}/${_openssl_crypto}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_ROOT_DIR}/include")
if(WIN32)
  set_property(TARGET OpenSSL::Crypto PROPERTY INTERFACE_LINK_LIBRARIES "ws2_32;crypt32")
else()
  set_property(TARGET OpenSSL::Crypto PROPERTY INTERFACE_LINK_LIBRARIES
      "Threads::Threads;${CMAKE_DL_LIBS}")
endif()
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_ROOT_DIR}/${_openssl_ssl}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_ROOT_DIR}/include"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)

file(MAKE_DIRECTORY "${ATLAS_BIN_ROOT}/licenses/OpenSSL")
foreach(_file IN ITEMS LICENSE.txt AUTHORS.md)
  configure_file("${OPENSSL_ROOT_DIR}/share/licenses/OpenSSL/${_file}"
    "${ATLAS_BIN_ROOT}/licenses/OpenSSL/${_file}" COPYONLY)
endforeach()
configure_file("${OPENSSL_ROOT_DIR}/atlas-source.json"
  "${ATLAS_BIN_ROOT}/licenses/OpenSSL/atlas-source.json" COPYONLY)
