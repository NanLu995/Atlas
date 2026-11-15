if(NOT TARGET atlas_net_client)
  message(FATAL_ERROR "Managed tests require ATLAS_BUILD_NET_CLIENT=ON; reconfigure with that option")
endif()

atlas_dotnet_project(
  NAME atlas_smoke_test_dll
  PROJECT_FILE Atlas.SmokeTest/Atlas.SmokeTest.csproj
  ASSEMBLY_NAME Atlas.SmokeTest.dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_runtime_test_dll
  PROJECT_FILE Atlas.RuntimeTest/Atlas.RuntimeTest.csproj
  ASSEMBLY_NAME Atlas.RuntimeTest.dll
  DEPENDS atlas_runtime_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_client_tests_dll
  PROJECT_FILE Atlas.Client.Tests/Atlas.Client.Tests.csproj
  ASSEMBLY_NAME Atlas.Client.Tests.dll
  DEPENDS atlas_shared_dll atlas_client_dll atlas_client_desktop_dll
)

atlas_dotnet_project(
  NAME atlas_rpc_fixture_base_dll
  PROJECT_FILE Atlas.RpcFixture/Base/Atlas.RpcFixture.Base.csproj
  ASSEMBLY_NAME Atlas.RpcFixture.Base.dll
  DEPENDS atlas_runtime_dll atlas_generators_def_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_rpc_fixture_client_dll
  PROJECT_FILE Atlas.RpcFixture/Client/Atlas.RpcFixture.Client.csproj
  ASSEMBLY_NAME Atlas.RpcFixture.Client.dll
  DEPENDS atlas_client_dll atlas_generators_def_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_rpc_fixture_cell_dll
  PROJECT_FILE Atlas.RpcFixture/Cell/Atlas.RpcFixture.Cell.csproj
  ASSEMBLY_NAME Atlas.RpcFixture.Cell.dll
  DEPENDS atlas_runtime_dll atlas_generators_def_dll
  DEPLOY
)

foreach(_fixture IN ITEMS base client cell)
  get_target_property(_fixture_output atlas_rpc_fixture_${_fixture}_dll DOTNET_ASSEMBLY)
  add_custom_command(OUTPUT "${_fixture_output}" APPEND
    DEPENDS Atlas.RpcFixture/Account.def Atlas.RpcFixture/Probe.def Atlas.RpcFixture/entity_ids.xml
  )
endforeach()

add_test(NAME test_entitydef_export
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../integration/test_entitydef_export.py"
    --dotnet "${DOTNET_EXECUTABLE}"
    --defdump "$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_tools_defdump,DOTNET_ASSEMBLY>>"
    --assembly "$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_rpc_fixture_base_dll,DOTNET_ASSEMBLY>>"
    --runtime-assembly "$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_runtime_dll,DOTNET_ASSEMBLY>>"
    --dbapp "$<TARGET_FILE:atlas_dbapp>")
set_tests_properties(test_entitydef_export PROPERTIES LABELS "integration" TIMEOUT 150)

if(WIN32)
  add_dependencies(test_login_flow atlas_tools_defdump atlas_rpc_fixture_base_dll_deploy)
  target_compile_definitions(test_login_flow PRIVATE
    "ATLAS_TEST_DOTNET=\"${DOTNET_EXECUTABLE}\""
    "ATLAS_TEST_DEFDUMP=\"$<GENEX_EVAL:$<TARGET_PROPERTY:atlas_tools_defdump,DOTNET_ASSEMBLY>>\"")
endif()

foreach(_suite IN ITEMS Runtime Generators Client)
  add_test(NAME test_csharp_${_suite}
    COMMAND "${DOTNET_EXECUTABLE}" test
      "${CMAKE_CURRENT_SOURCE_DIR}/Atlas.${_suite}.Tests/Atlas.${_suite}.Tests.csproj"
      --configuration "$<CONFIG>" --nologo --disable-build-servers
      -m:1 -p:UseSharedCompilation=false
  )
  set_tests_properties(test_csharp_${_suite} PROPERTIES
    LABELS "managed"
    RUN_SERIAL TRUE
    TIMEOUT 300
    ENVIRONMENT "ATLAS_BIN_DIR=${ATLAS_BIN_ROOT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
endforeach()
