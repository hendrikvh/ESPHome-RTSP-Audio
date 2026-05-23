cmake_minimum_required(VERSION 3.16)
project(rtsp_audio_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

set(RTSP_AUDIO_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/rtsp_audio)

add_executable(rtsp_audio_tests
  test_session_timeout.cpp
  test_dc_blocker.cpp
)
target_include_directories(rtsp_audio_tests PRIVATE ${RTSP_AUDIO_SRC_DIR})
target_link_libraries(rtsp_audio_tests PRIVATE GTest::gtest_main)

gtest_discover_tests(rtsp_audio_tests)
