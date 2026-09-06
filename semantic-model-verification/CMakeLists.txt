cmake_minimum_required(VERSION 3.24)

add_library(semantic_test
    src/fixture_manifest.cpp
    src/normalized_model.cpp
    src/runner.cpp)
target_include_directories(semantic_test PUBLIC include)
target_link_libraries(semantic_test PUBLIC codeinsight_core)
target_compile_features(semantic_test PUBLIC cxx_std_20)
target_compile_options(semantic_test PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)

add_executable(semantic-test tools/semantic-test/main.cpp)
target_link_libraries(semantic-test PRIVATE semantic_test)
if(WIN32)
  add_custom_command(TARGET semantic-test POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_RUNTIME}" "$<TARGET_FILE_DIR:semantic-test>")
endif()

if(CODEINSIGHT_BUILD_TESTS)
  add_executable(semantic_test_framework_tests tests/framework/framework_tests.cpp)
  target_link_libraries(semantic_test_framework_tests PRIVATE semantic_test)
  target_compile_definitions(semantic_test_framework_tests PRIVATE
      SEMANTIC_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(WIN32)
    add_custom_command(TARGET semantic_test_framework_tests POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBCLANG_RUNTIME}" "$<TARGET_FILE_DIR:semantic_test_framework_tests>")
  endif()
  add_test(NAME semantic_test_framework_tests COMMAND semantic_test_framework_tests)
  add_test(NAME semantic_model_conformance_seed
      COMMAND semantic-test run --all --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
              --artifacts "${CMAKE_CURRENT_BINARY_DIR}/failure-artifacts")
  set_tests_properties(semantic_model_conformance_seed PROPERTIES LABELS "semantic;conformance")
endif()

install(TARGETS semantic-test RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)

if(CODEINSIGHT_BUILD_TESTS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME semantic_model_audit_regressions
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/reproduce_audit.py"
          --codeinsight "$<TARGET_FILE:codeinsight>"
          --cases "${CMAKE_CURRENT_SOURCE_DIR}/audits/cpp-reproductions.json"
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/audit-regressions")
  set_tests_properties(semantic_model_audit_regressions PROPERTIES LABELS "semantic;regression" TIMEOUT 240)
  add_test(NAME codeinsight_cli_reliability
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cli.py"
          --codeinsight "$<TARGET_FILE:codeinsight>"
          --output-dir "${CMAKE_CURRENT_BINARY_DIR}/cli-reliability")
  set_tests_properties(codeinsight_cli_reliability PROPERTIES LABELS "runtime;regression" TIMEOUT 120)
endif()
