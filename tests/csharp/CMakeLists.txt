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
