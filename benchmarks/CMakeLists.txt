find_package(Threads REQUIRED)

add_executable(stm_benchmark
    stm_benchmark.cpp
)

target_link_libraries(stm_benchmark PRIVATE
    mylib
    Threads::Threads
)

target_include_directories(stm_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/TierAlloc
)

# Benchmark binaries are deliberately independent from the test-only hooks and
# VERIFY mode.  Keep -march=native opt-in so recorded results remain portable.
target_compile_definitions(stm_benchmark PRIVATE
    NDEBUG
    STM_WW_VERIFY_LOGIC_MODE=0
    STM_WW_TEST_HOOKS=0
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(stm_benchmark PRIVATE -O3)
endif()

option(STM_BENCHMARK_NATIVE "Enable -march=native for stm_benchmark" OFF)
if(STM_BENCHMARK_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(stm_benchmark PRIVATE -march=native)
endif()
