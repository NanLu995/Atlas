# tests/CMakeLists.txt

# Helper function to reduce test boilerplate
function(atlas_add_test)
  cmake_parse_arguments(ARG "" "NAME;LABEL" "SOURCES;DEPS" ${ARGN})

  if(NOT ARG_LABEL)
    set(ARG_LABEL "unit")
  endif()

  add_executable(${ARG_NAME} ${ARG_SOURCES})
  target_link_libraries(${ARG_NAME} PRIVATE GTest::gtest_main ${ARG_DEPS})
  target_include_directories(${ARG_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

  # A script handles empty DLL lists without invoking copy_if_different with no sources.
  if(WIN32)
    add_custom_command(TARGET ${ARG_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND}
              "-DTARGET_DLLS=$<TARGET_RUNTIME_DLLS:${ARG_NAME}>"
              "-DTARGET_DIR=$<TARGET_FILE_DIR:${ARG_NAME}>"
              -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDLLs.cmake"
      VERBATIM
    )
    # Runtime-loaded CLR libraries are absent from TARGET_RUNTIME_DLLS.
    atlas_deploy_clr_runtime_if_needed(${ARG_NAME})
  endif()
  atlas_deploy_db_plugins_if_needed(${ARG_NAME})

  add_test(NAME ${ARG_NAME} COMMAND ${ARG_NAME})
  set_tests_properties(${ARG_NAME} PROPERTIES LABELS "${ARG_LABEL}")

  # Output directory: ${ATLAS_BIN_ROOT} — derived from the build directory
  # name so a parallel build (e.g. build/mimalloc alongside build/debug)
  # keeps its test exes in its own bin/ tree rather than overwriting the
  # default's. All EXE/DLL artifacts share the flat bin/ layout described
  # in cmake/AtlasFolders.cmake.
  foreach(_cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_cfg}" _CFG)
    set_target_properties(${ARG_NAME} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${ATLAS_BIN_ROOT}"
    )
  endforeach()

  # Organize test targets into solution folders by label
  if(ARG_LABEL STREQUAL "integration")
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "Test/Integration")
  else()
    set_target_properties(${ARG_NAME} PROPERTIES FOLDER "Test/Unit")
  endif()
endfunction()

add_subdirectory(unit)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(test_tls_stream unit/test_tls_stream.cpp)
target_link_libraries(test_tls_stream PRIVATE GTest::gtest_main atlas_network atlas_server)
target_compile_definitions(test_tls_stream PRIVATE
  ATLAS_BUILD_NET_CLIENT=$<BOOL:${ATLAS_BUILD_NET_CLIENT}>)
if(ATLAS_BUILD_NET_CLIENT)
  target_link_libraries(test_tls_stream PRIVATE atlas_net_client_core)
endif()
atlas_set_output_dir("" test_tls_stream)
set(_openssl_program "${OPENSSL_ROOT_DIR}/bin/openssl")
if(WIN32)
  string(APPEND _openssl_program ".exe")
  add_custom_command(TARGET test_tls_stream POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DTARGET_DLLS=$<TARGET_RUNTIME_DLLS:test_tls_stream>"
      "-DTARGET_DIR=$<TARGET_FILE_DIR:test_tls_stream>"
      -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDLLs.cmake" VERBATIM)
endif()
add_test(NAME test_tls_stream COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_tls_stream.py"
  "${_openssl_program}" "$<TARGET_FILE:test_tls_stream>")
set_tests_properties(test_tls_stream PROPERTIES LABELS "unit" TIMEOUT 120)
add_test(NAME test_build_tool
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p test_build.py)
set_tests_properties(test_build_tool PROPERTIES LABELS "unit")
add_test(NAME test_client_sdk_tool
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p test_client_sdk.py)
set_tests_properties(test_client_sdk_tool PROPERTIES LABELS "unit")
add_test(NAME test_package_delivery_tool
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p test_package_delivery.py)
set_tests_properties(test_package_delivery_tool PROPERTIES LABELS "unit")

add_test(NAME test_database_tools
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p test_database_tools.py)
set_tests_properties(test_database_tools PROPERTIES LABELS "unit")

add_test(NAME test_mysql_sdk_tool
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p test_prepare_mysql.py)
set_tests_properties(test_mysql_sdk_tool PROPERTIES LABELS "unit")

add_test(NAME test_ha_verifiers
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p "test_verify_*_ha.py")
set_tests_properties(test_ha_verifiers PROPERTIES LABELS "unit")

add_test(NAME test_build_openssl
  COMMAND "${Python3_EXECUTABLE}" -m unittest discover
    -s "${CMAKE_CURRENT_SOURCE_DIR}/unit" -p test_build_openssl.py)
set_tests_properties(test_build_openssl PROPERTIES LABELS "unit")
add_subdirectory(integration)
if(TARGET atlas_tool AND TARGET machined AND TARGET atlas_baseappmgr)
  add_test(NAME test_atlas_tool_output
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/integration/test_atlas_tool_output.py"
      --bin-dir "$<TARGET_FILE_DIR:atlas_tool>")
  set_tests_properties(test_atlas_tool_output PROPERTIES LABELS "integration" TIMEOUT 120)
endif()
add_subdirectory(parity)

if(ATLAS_BUILD_CSHARP)
  add_subdirectory(csharp)
endif()
