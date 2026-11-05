# miniz's settings, and its names prefixed: a chip's ROM can have an older tinfl of its own, linked
# by absolute address, and the state's layout must be this one's
set(DFUKIT_MINIZ_DEFINITIONS
  MINIZ_NO_MALLOC
  MINIZ_NO_STDIO
  MINIZ_NO_TIME
  tinfl_decompress=dfukit_tinfl_decompress
  tinfl_decompress_mem_to_heap=dfukit_tinfl_decompress_mem_to_heap
  tinfl_decompress_mem_to_mem=dfukit_tinfl_decompress_mem_to_mem
  tinfl_decompress_mem_to_callback=dfukit_tinfl_decompress_mem_to_callback
)

# An ESP-IDF component when ESP-IDF builds it; a plain CMake project (with the tests) otherwise
if(ESP_PLATFORM)
  idf_component_register(
    SRCS
      src/dfukit_server.c
      src/dfukit_client.c
      src/dfukit_inflate.c
      lib/crc32/dfukit_crc32.c
      lib/miniz/miniz_tinfl.c
      backends/ram/dfukit_ram.c
      backends/esp-idf-ota/dfukit_esp_ota.c
    INCLUDE_DIRS src lib/crc32 lib/miniz backends/ram backends/esp-idf-ota
    REQUIRES app_update esp_timer bootloader_support
  )
  target_compile_definitions(${COMPONENT_LIB} PUBLIC ${DFUKIT_MINIZ_DEFINITIONS})
  set_source_files_properties(lib/miniz/miniz_tinfl.c PROPERTIES COMPILE_OPTIONS -w)
  return()
endif()

cmake_minimum_required(VERSION 3.22)
project(dfukit C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DFUKIT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dfukit_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dfukit_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dfukit_inflate.c
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/crc32/dfukit_crc32.c
  ${CMAKE_CURRENT_SOURCE_DIR}/lib/miniz/miniz_tinfl.c
)

add_library(dfukit STATIC ${DFUKIT_SOURCES})
target_include_directories(dfukit PUBLIC src lib/crc32 lib/miniz)
target_compile_definitions(dfukit PUBLIC ${DFUKIT_MINIZ_DEFINITIONS})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dfukit PRIVATE -Werror=implicit-function-declaration)
  target_compile_options(dfukit PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)
  set_source_files_properties(lib/miniz/miniz_tinfl.c PROPERTIES COMPILE_OPTIONS -w)
endif()

add_library(dfukit_ram STATIC backends/ram/dfukit_ram.c)
target_include_directories(dfukit_ram PUBLIC backends/ram)
target_link_libraries(dfukit_ram PUBLIC dfukit)
# Plain C99, as documented: what it does not have is an error
set_target_properties(dfukit dfukit_ram PROPERTIES C_EXTENSIONS OFF)

# The Python module (python/dfukit_native.cpp, nanobind): dfukit whole, with the ram backend. Tests
# run through it
option(DFUKIT_PYTHON "Build the Python module" ${PROJECT_IS_TOP_LEVEL})
option(DFUKIT_COVERAGE "Measure what the tests cover (tests/README.md)" OFF)
if(DFUKIT_PYTHON OR SKBUILD)
  enable_language(CXX)
  find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module
    OPTIONAL_COMPONENTS Development.SABIModule)
  execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE NANOBIND_DIR ERROR_QUIET)
  list(APPEND CMAKE_PREFIX_PATH "${NANOBIND_DIR}")
  find_package(nanobind CONFIG QUIET)
  if(nanobind_FOUND)
    nanobind_add_module(_dfukit STABLE_ABI NB_STATIC
      ${DFUKIT_SOURCES} backends/ram/dfukit_ram.c python/dfukit_native.cpp)
    target_include_directories(_dfukit PRIVATE src lib/crc32 lib/miniz backends/ram)
    target_compile_definitions(_dfukit PRIVATE ${DFUKIT_MINIZ_DEFINITIONS})
    if(DFUKIT_COVERAGE)
      target_compile_options(_dfukit PRIVATE --coverage -O0)
      target_link_options(_dfukit PRIVATE --coverage)
    endif()
    if(SKBUILD)
      install(TARGETS _dfukit LIBRARY DESTINATION .)
    endif()
  else()
    message(FATAL_ERROR "No nanobind: the tests need it. Install it, or -DDFUKIT_PYTHON=OFF")
  endif()
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT SKBUILD)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
endif()
