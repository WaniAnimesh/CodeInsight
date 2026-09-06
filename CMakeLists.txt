cmake_minimum_required(VERSION 3.24)
project(CodeInsight VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CODEINSIGHT_BUILD_TESTS "Build CodeInsight tests" ON)
option(CODEINSIGHT_BUILD_ANALYSIS "Build the optional dead-code analysis consumer" ON)
option(CODEINSIGHT_BUILD_VERIFICATION "Build the independent semantic-model conformance project" ON)
set(CODEINSIGHT_MINIMUM_CLANG_MAJOR 20 CACHE STRING "Minimum supported Clang major version")

set(_codeinsight_default_clang_root "C:/Program Files/LLVM")
if(WIN32)
  file(GLOB _codeinsight_llvm_roots LIST_DIRECTORIES true "C:/Program Files/LLVM/*")
  list(SORT _codeinsight_llvm_roots COMPARE NATURAL ORDER DESCENDING)
  foreach(_candidate IN LISTS _codeinsight_llvm_roots)
    if(EXISTS "${_candidate}/include/clang-c/Index.h" AND EXISTS "${_candidate}/lib/libclang.lib")
      set(_codeinsight_default_clang_root "${_candidate}")
      break()
    endif()
  endforeach()
endif()
set(CODEINSIGHT_CLANG_ROOT "${_codeinsight_default_clang_root}" CACHE PATH "LLVM/Clang installation root")

find_program(CODEINSIGHT_CLANG_EXECUTABLE NAMES clang clang.exe
  HINTS "${CODEINSIGHT_CLANG_ROOT}/bin" REQUIRED NO_DEFAULT_PATH)
execute_process(
  COMMAND "${CODEINSIGHT_CLANG_EXECUTABLE}" --version
  OUTPUT_VARIABLE CODEINSIGHT_CLANG_VERSION_TEXT
  ERROR_VARIABLE CODEINSIGHT_CLANG_VERSION_ERROR
  RESULT_VARIABLE CODEINSIGHT_CLANG_VERSION_RESULT
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT CODEINSIGHT_CLANG_VERSION_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to query ${CODEINSIGHT_CLANG_EXECUTABLE}: ${CODEINSIGHT_CLANG_VERSION_ERROR}")
endif()
string(REGEX MATCH "clang version ([0-9]+)" _codeinsight_clang_match "${CODEINSIGHT_CLANG_VERSION_TEXT}")
if(NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "Unable to determine Clang major version from: ${CODEINSIGHT_CLANG_VERSION_TEXT}")
endif()
set(CODEINSIGHT_CLANG_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "clang version [^\r\n]+" CODEINSIGHT_CLANG_VERSION_SUMMARY "${CODEINSIGHT_CLANG_VERSION_TEXT}")
if(CODEINSIGHT_CLANG_MAJOR LESS CODEINSIGHT_MINIMUM_CLANG_MAJOR)
  message(FATAL_ERROR
    "CodeInsight requires Clang ${CODEINSIGHT_MINIMUM_CLANG_MAJOR}+; found ${CODEINSIGHT_CLANG_VERSION_TEXT}. "
    "Set CODEINSIGHT_CLANG_ROOT to a compatible LLVM installation.")
endif()
message(STATUS "CodeInsight semantic frontend: ${CODEINSIGHT_CLANG_VERSION_SUMMARY}")

find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS "${CODEINSIGHT_CLANG_ROOT}/include" REQUIRED)
find_library(LIBCLANG_LIBRARY NAMES libclang HINTS "${CODEINSIGHT_CLANG_ROOT}/lib" REQUIRED)
if(WIN32)
  find_file(LIBCLANG_RUNTIME NAMES libclang.dll HINTS "${CODEINSIGHT_CLANG_ROOT}/bin" REQUIRED NO_DEFAULT_PATH)
endif()

add_library(codeinsight_core
    src/call_resolution.cpp
    src/core.cpp
    src/compilation_database.cpp
    src/export.cpp
    src/libclang_frontend.cpp
    src/model.cpp
    src/persistence.cpp
    src/incremental.cpp)
target_include_directories(codeinsight_core PUBLIC include PRIVATE "${LIBCLANG_INCLUDE_DIR}")
target_link_libraries(codeinsight_core PUBLIC "${LIBCLANG_LIBRARY}")
target_compile_definitions(codeinsight_core PRIVATE
  CODEINSIGHT_CONFIGURED_CLANG_VERSION="${CODEINSIGHT_CLANG_VERSION_SUMMARY}")
if(WIN32)
  target_link_libraries(codeinsight_core PUBLIC winsqlite3)
  target_compile_definitions(codeinsight_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_compile_options(codeinsight_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)

if(CODEINSIGHT_BUILD_ANALYSIS)
  add_library(codeinsight_analysis src/analysis.cpp)
  target_link_libraries(codeinsight_analysis PUBLIC codeinsight_core)
  target_compile_options(codeinsight_analysis PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
  install(TARGETS codeinsight_analysis ARCHIVE DESTINATION lib)
endif()

add_executable(codeinsight src/main.cpp)
target_link_libraries(codeinsight PRIVATE codeinsight_core)
if(CODEINSIGHT_BUILD_ANALYSIS)
  target_link_libraries(codeinsight PRIVATE codeinsight_analysis)
  target_compile_definitions(codeinsight PRIVATE CODEINSIGHT_WITH_ANALYSIS)
endif()
if(WIN32)
  add_custom_command(TARGET codeinsight POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_RUNTIME}" "$<TARGET_FILE_DIR:codeinsight>")
endif()

install(TARGETS codeinsight RUNTIME DESTINATION bin)
install(TARGETS codeinsight_core ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
if(WIN32)
  install(FILES "${LIBCLANG_RUNTIME}" DESTINATION bin)
endif()

if(CODEINSIGHT_BUILD_TESTS)
  enable_testing()
  add_executable(codeinsight_call_resolution_tests tests/call_resolution_tests.cpp)
  target_link_libraries(codeinsight_call_resolution_tests PRIVATE codeinsight_core)
  if(WIN32)
    add_custom_command(TARGET codeinsight_call_resolution_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_RUNTIME}" "$<TARGET_FILE_DIR:codeinsight_call_resolution_tests>")
  endif()
  add_test(NAME codeinsight_call_resolution_tests COMMAND codeinsight_call_resolution_tests)
endif()

if(CODEINSIGHT_BUILD_TESTS AND CODEINSIGHT_BUILD_ANALYSIS)
  add_executable(codeinsight_tests tests/tests.cpp)
  target_link_libraries(codeinsight_tests PRIVATE codeinsight_analysis)
  target_compile_definitions(codeinsight_tests PRIVATE CODEINSIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(WIN32)
    add_custom_command(TARGET codeinsight_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_RUNTIME}" "$<TARGET_FILE_DIR:codeinsight_tests>")
  endif()
  add_test(NAME codeinsight_tests COMMAND codeinsight_tests)
endif()

if(CODEINSIGHT_BUILD_VERIFICATION)
  add_subdirectory(semantic-model-verification)
endif()
