if(ATLAS_DB_MYSQL)
  add_library(atlas_db_mysql STATIC
    mysql_client.cc
    mysql_database.cc
    mysql_account_session.cc
  )
  target_include_directories(atlas_db_mysql
    PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
  target_link_libraries(atlas_db_mysql
    PUBLIC
      atlas_db_iface
      atlas_entitydef
      atlas_foundation
      atlas_network
      OracleMySQL::Client
      atlas_compiler_options
  )

  # Plugin DLL — loaded at runtime by the database factory via DynamicLibrary.
  add_library(atlas_db_mysql_plugin SHARED mysql_database_plugin.cc)
  target_compile_definitions(atlas_db_mysql_plugin PRIVATE ATLAS_DB_BACKEND_EXPORTS)
  target_link_libraries(atlas_db_mysql_plugin PRIVATE atlas_db_mysql)
  set_target_properties(atlas_db_mysql_plugin PROPERTIES
    OUTPUT_NAME "atlas_db_mysql"
    FOLDER "Plugins"
    # Keep the plugin import library separate from the backend static library.
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/plugins"
  )
  atlas_deploy_mysql_runtime(atlas_db_mysql_plugin)
  if(UNIX)
    set_target_properties(atlas_db_mysql_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden)
  endif()
endif()
