cmake_minimum_required(VERSION 3.20)

project(noria_lang LANGUAGES CXX)

enable_testing()

option(NORIA_ENABLE_SANITIZERS "Build Noria with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(noria_configure_compile target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(NORIA_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
  elseif(NORIA_ENABLE_SANITIZERS)
    message(FATAL_ERROR "NORIA_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
endfunction()

function(noria_configure_executable target)
  noria_configure_compile(${target})
  if(NORIA_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

add_library(noria_core OBJECT
  src/AstClone.cpp
  src/AstPrinter.cpp
  src/AstVisitor.cpp
  src/codegen/Codegen.cpp
  src/codegen/CodegenBuiltins.cpp
  src/codegen/CodegenExpressions.cpp
  src/codegen/CodegenModule.cpp
  src/codegen/CodegenPlaces.cpp
  src/codegen/CodegenStatements.cpp
  src/codegen/CodegenStructs.cpp
  src/codegen/CodegenSupport.cpp
  src/CompilerCache.cpp
  src/Compiler.cpp
  src/Constraints.cpp
  src/Diagnostic.cpp
  src/IrEmitter.cpp
  src/internal/SpecializationSupport.cpp
  src/Lexer.cpp
  src/ModuleResolver.cpp
  src/monomorphize/Monomorphize.cpp
  src/monomorphize/SpecializationCache.cpp
  src/monomorphize/SpecializationClone.cpp
  src/monomorphize/SpecializationRewrite.cpp
  src/monomorphize/SpecializationTypes.cpp
  src/Parser.cpp
  src/typecheck/TypeChecker.cpp
  src/typecheck/TypeCheckerCalls.cpp
  src/typecheck/TypeCheckerDeclarations.cpp
  src/typecheck/TypeCheckerDriver.cpp
  src/typecheck/TypeCheckerExpressions.cpp
  src/typecheck/TypeCheckerPlaces.cpp
  src/typecheck/TypeCheckerStatements.cpp
  src/typecheck/TypeCheckerStructs.cpp
  src/typecheck/TypeCheckerSupport.cpp
  src/typecheck/TypeRelations.cpp
  src/Types.cpp
)

target_include_directories(noria_core PUBLIC include)
noria_configure_compile(noria_core)

add_executable(noria
  src/main.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(noria PRIVATE include)
noria_configure_executable(noria)

include(GNUInstallDirs)
install(TARGETS noria RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY stdlib/ DESTINATION ${CMAKE_INSTALL_DATADIR}/noria/stdlib)

add_executable(type_representation_test
  tests/type_representation_test.cpp
  src/Types.cpp
)

target_include_directories(type_representation_test PRIVATE include)
noria_configure_executable(type_representation_test)

add_executable(builtin_registry_test
  tests/builtin_registry_test.cpp
  src/Types.cpp
)

target_include_directories(builtin_registry_test PRIVATE include)
noria_configure_executable(builtin_registry_test)

add_executable(visitor_smoke_test
  tests/visitor_smoke_test.cpp
  src/AstPrinter.cpp
  src/AstVisitor.cpp
  src/Types.cpp
)

target_include_directories(visitor_smoke_test PRIVATE include)
noria_configure_executable(visitor_smoke_test)

add_executable(ast_clone_test
  tests/ast_clone_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(ast_clone_test PRIVATE include)
noria_configure_executable(ast_clone_test)

add_executable(lfu_cache_test
  tests/lfu_cache_test.cpp
)

target_include_directories(lfu_cache_test PRIVATE include)
noria_configure_executable(lfu_cache_test)

add_executable(hash_table_test
  tests/hash_table_test.cpp
)

target_include_directories(hash_table_test PRIVATE include)
noria_configure_executable(hash_table_test)

add_executable(compiler_cache_test
  tests/compiler_cache_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(compiler_cache_test PRIVATE include)
noria_configure_executable(compiler_cache_test)

add_executable(compiler_facade_test
  tests/compiler_facade_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(compiler_facade_test PRIVATE include)
noria_configure_executable(compiler_facade_test)

add_executable(module_resolver_test
  tests/module_resolver_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(module_resolver_test PRIVATE include)
noria_configure_executable(module_resolver_test)

add_executable(diagnostic_location_test
  tests/diagnostic_location_test.cpp
  src/Diagnostic.cpp
)

target_include_directories(diagnostic_location_test PRIVATE include)
noria_configure_executable(diagnostic_location_test)

add_executable(generics_test
  tests/generics_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(generics_test PRIVATE include)
noria_configure_executable(generics_test)

add_executable(constraints_test
  tests/constraints_test.cpp
  src/Constraints.cpp
  src/Types.cpp
)

target_include_directories(constraints_test PRIVATE include)
noria_configure_executable(constraints_test)

add_executable(semantic_tables_test
  tests/semantic_tables_test.cpp
  src/Types.cpp
)

target_include_directories(semantic_tables_test PRIVATE include)
noria_configure_executable(semantic_tables_test)

add_test(
  NAME examples
  COMMAND ${CMAKE_COMMAND} -E env "BUILD_DIR=${CMAKE_BINARY_DIR}"
          ${CMAKE_SOURCE_DIR}/tests/run_examples.sh
)
add_test(NAME type_representation COMMAND type_representation_test)
add_test(NAME builtin_registry COMMAND builtin_registry_test)
add_test(NAME visitor_smoke COMMAND visitor_smoke_test)
add_test(NAME ast_clone COMMAND ast_clone_test)
add_test(NAME lfu_cache COMMAND lfu_cache_test)
add_test(NAME hash_table COMMAND hash_table_test)
add_test(NAME compiler_cache COMMAND compiler_cache_test)
add_test(NAME compiler_facade COMMAND compiler_facade_test)
add_test(NAME module_resolver COMMAND module_resolver_test)
add_test(NAME diagnostic_location COMMAND diagnostic_location_test)
add_test(NAME generics COMMAND generics_test)
add_test(NAME constraints COMMAND constraints_test)
add_test(NAME semantic_tables COMMAND semantic_tables_test)

set_tests_properties(examples PROPERTIES LABELS "e2e")

set(NORIA_UNIT_TESTS
  type_representation
  builtin_registry
  visitor_smoke
  ast_clone
  lfu_cache
  hash_table
  compiler_cache
  compiler_facade
  module_resolver
  diagnostic_location
  generics
  constraints
  semantic_tables
)
set_tests_properties(${NORIA_UNIT_TESTS} PROPERTIES LABELS "unit")
