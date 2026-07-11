cmake_minimum_required(VERSION 3.22)
project(minicam_hal LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINICAM_ENABLE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(MINICAM_FETCH_GTEST "Fetch GoogleTest when a system package is unavailable" ON)

add_library(minicam_hal
  hal/bounded_task_queue.cpp
  hal/camera_device_session.cpp
  hal/camera_hal_runtime.cpp
  hal/dma_buf_allocator.cpp
  hal/epoll_event_loop.cpp
  hal/frame_metrics.cpp
  hal/in_flight_request_tracker.cpp
  hal/latency_tracker.cpp
  hal/mock_driver_adapter.cpp
  hal/task_executor.cpp
  hal/v4l2_driver_adapter.cpp
)

target_include_directories(minicam_hal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(minicam_hal PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${MINICAM_ENABLE_WARNINGS_AS_ERRORS}>:-Werror>
)

add_executable(minicam_cli
  app/buffer_tracker.cpp
  app/cli_result_collector.cpp
  app/image_writer.cpp
  app/minicam_cli.cpp
)
target_link_libraries(minicam_cli PRIVATE minicam_hal)
target_compile_options(minicam_cli PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${MINICAM_ENABLE_WARNINGS_AS_ERRORS}>:-Werror>
)

enable_testing()

find_package(GTest QUIET)
if(NOT GTest_FOUND AND MINICAM_FETCH_GTEST)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(NOT TARGET GTest::gtest_main)
  message(FATAL_ERROR "GoogleTest is required. Install GTest or configure with MINICAM_FETCH_GTEST=ON.")
endif()

include(GoogleTest)

function(add_minicam_gtest target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE minicam_hal GTest::gtest_main)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${MINICAM_ENABLE_WARNINGS_AS_ERRORS}>:-Werror>
  )
  gtest_discover_tests(${target})
endfunction()

add_minicam_gtest(bounded_task_queue_test
  tests/bounded_task_queue_test.cpp
)
add_minicam_gtest(camera_device_session_test
  tests/camera_device_session_test.cpp
)
add_minicam_gtest(dma_buf_allocator_test
  tests/dma_buf_allocator_test.cpp
)
add_minicam_gtest(in_flight_request_tracker_test
  tests/in_flight_request_tracker_test.cpp
)
add_minicam_gtest(task_executor_test
  tests/task_executor_test.cpp
)
