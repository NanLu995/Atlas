add_library(atlas_platform_config INTERFACE)
target_include_directories(atlas_platform_config INTERFACE "${CMAKE_SOURCE_DIR}/src/lib")

add_library(atlas_platform STATIC
  crash_handler.cc
  filesystem.cc
  io_poller.cc
  io_poller_select.cc
  password_hash.cc
  signal_handler.cc
  threading.cc
)

if(WIN32)
  target_sources(atlas_platform PRIVATE
    child_process_windows.cc
    crash_handler_windows.cc
    dynamic_library_windows.cc
    environment_windows.cc
    filesystem_windows.cc
    io_poller_wsapoll.cc
    process_launcher_windows.cc
    secure_random_windows.cc
    sha256_windows.cc
    threading_windows.cc
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(atlas_platform PRIVATE
    child_process_linux.cc
    crash_handler_linux.cc
    dynamic_library_linux.cc
    environment_linux.cc
    filesystem_linux.cc
    io_poller_epoll.cc
    threading_linux.cc
    process_launcher_linux.cc
    secure_random_linux.cc
    sha256_linux.cc
  )
endif()

if(ATLAS_USE_IOURING AND UNIX)
  target_sources(atlas_platform PRIVATE io_poller_io_uring.cc)
endif()

target_include_directories(atlas_platform PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")
target_link_libraries(atlas_platform
  PUBLIC atlas_foundation atlas_compiler_options
)

include(AtlasOpenSSL)
target_link_libraries(atlas_platform PUBLIC OpenSSL::Crypto)

if(WIN32)
  target_link_libraries(atlas_platform PUBLIC ws2_32 dbghelp bcrypt)
elseif(UNIX)
  target_link_libraries(atlas_platform PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()
