find_package(Qt6 REQUIRED COMPONENTS Test)

set(TEST_SUPPORT_SOURCES
  support/fake_network_reply.cpp
  support/fake_network_access_manager.cpp
  support/local_ws_server.cpp
)

set(TEST_SUPPORT_HEADERS
  support/fake_network_reply.h
  support/fake_network_access_manager.h
  support/local_ws_server.h
)

set(DANMAKU_CORE_SOURCES
  ${CMAKE_SOURCE_DIR}/src/danmaku/danmaku-codec.cpp
  ${CMAKE_SOURCE_DIR}/src/danmaku/danmaku-parser.cpp
  ${CMAKE_SOURCE_DIR}/src/danmaku/open-live-client.cpp
  ${CMAKE_SOURCE_DIR}/src/danmaku-ws.cpp
  ${CMAKE_SOURCE_DIR}/src/config-manager.cpp
)

set(DANMAKU_CORE_HEADERS
  ${CMAKE_SOURCE_DIR}/src/danmaku/danmaku-packet.h
  ${CMAKE_SOURCE_DIR}/src/danmaku/danmaku-codec.h
  ${CMAKE_SOURCE_DIR}/src/danmaku/danmaku-parser.h
  ${CMAKE_SOURCE_DIR}/src/danmaku/open-live-client.h
  ${CMAKE_SOURCE_DIR}/src/danmaku-ws.h
  ${CMAKE_SOURCE_DIR}/src/config-manager.h
)

function(add_danmaku_test test_name)
  add_executable(${test_name}
    ${test_name}.cpp
    ${TEST_SUPPORT_SOURCES}
    ${TEST_SUPPORT_HEADERS}
    ${DANMAKU_CORE_SOURCES}
    ${DANMAKU_CORE_HEADERS}
  )

  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${NLOHMANN_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIRS}
    ${LIBBROTLI_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
  )

  target_link_libraries(${test_name} PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
    Threads::Threads
    ZLIB::ZLIB
    ${OPENSSL_LIBRARIES}
    ${LIBBROTLI_LIBRARIES}
  )

  target_compile_definitions(${test_name} PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
  )

  add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_danmaku_test(test_open_live_client)
add_danmaku_test(test_danmaku_ws_lifecycle)
add_danmaku_test(test_danmaku_ws_failover)
