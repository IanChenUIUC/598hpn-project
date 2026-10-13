cmake_minimum_required(VERSION 3.13)

project(baseline-grape C CXX)

set(GRAPE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../external/libgrape-lite")
set(BUILD_LIBGRAPELITE_DOCS OFF CACHE BOOL "Build libgrape-lite documentation")
set(BUILD_LIBGRAPELITE_TESTS OFF CACHE BOOL "Build libgrape-lite test cases")
add_subdirectory("${GRAPE}" grape EXCLUDE_FROM_ALL)

find_package(MPI REQUIRED)
include("${GRAPE}/cmake/FindGlog.cmake")
include("${GRAPE}/cmake/FindGFlags.cmake")

add_executable(cc cc.cc)
target_include_directories(cc PRIVATE
  "${GRAPE}"
  "${GRAPE}/examples/analytical_apps")
target_include_directories(cc SYSTEM PRIVATE
  "${GRAPE}/thirdparty"
  ${MPI_CXX_INCLUDE_PATH}
  ${GLOG_INCLUDE_DIRS}
  ${GFLAGS_INCLUDE_DIRS})
target_link_libraries(cc grape-lite ${MPI_CXX_LIBRARIES} ${GLOG_LIBRARIES}
  ${GFLAGS_LIBRARIES} ${CMAKE_DL_LIBS})
