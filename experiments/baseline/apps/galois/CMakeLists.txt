cmake_minimum_required(VERSION 3.13)

project(baseline-galois C CXX)

set(GALOIS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../external/Galois")
set(GALOIS_ENABLE_DIST ON CACHE BOOL "Enable distributed features")
add_subdirectory("${GALOIS}" galois EXCLUDE_FROM_ALL)
find_package(LLVM REQUIRED CONFIG)

add_executable(cc cc.cpp)
target_include_directories(cc PRIVATE
  "${GALOIS}/lonestar/analytics/distributed/connected-components")
target_link_libraries(cc Galois::shmem LLVMSupport distbench)

add_executable(cc_shm cc_shm.cpp)
target_link_libraries(cc_shm Galois::shmem LLVMSupport lonestar)
