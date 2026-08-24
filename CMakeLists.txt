cmake_minimum_required(VERSION 3.20)

project(noria_lang LANGUAGES CXX)

enable_testing()

option(NORIA_ENABLE_SANITIZERS "Build Noria with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(noria_core OBJECT
  src/AstPrinter.cpp
  src/Codegen.cpp
  src/Compiler.cpp
  src/Constraints.cpp
  src/Diagnostic.cpp
  src/IrEmitter.cpp
  src/Lexer.cpp
  src/ModuleResolver.cpp
  src/Monomorphize.cpp
  src/Parser.cpp
  src/TypeChecker.cpp
  src/Types.cpp
)

target_include_directories(noria_core PUBLIC include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(noria_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NORIA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(noria_core PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "NORIA_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
endif()

add_executable(noria
  src/main.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(noria PRIVATE include)

add_executable(type_representation_test
  tests/type_representation_test.cpp
  src/Types.cpp
)

target_include_directories(type_representation_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(type_representation_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(builtin_registry_test
  tests/builtin_registry_test.cpp
  src/Types.cpp
)

target_include_directories(builtin_registry_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(builtin_registry_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(visitor_smoke_test
  tests/visitor_smoke_test.cpp
  src/AstPrinter.cpp
  src/Types.cpp
)

target_include_directories(visitor_smoke_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(visitor_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(compiler_facade_test
  tests/compiler_facade_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(compiler_facade_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(compiler_facade_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(module_resolver_test
  tests/module_resolver_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(module_resolver_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(module_resolver_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(diagnostic_location_test
  tests/diagnostic_location_test.cpp
  src/Diagnostic.cpp
)

target_include_directories(diagnostic_location_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(diagnostic_location_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NORIA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_options(noria PRIVATE -fsanitize=address,undefined)
    target_link_options(compiler_facade_test PRIVATE -fsanitize=address,undefined)
    target_link_options(module_resolver_test PRIVATE -fsanitize=address,undefined)
  endif()
endif()

add_executable(generics_test
  tests/generics_test.cpp
  $<TARGET_OBJECTS:noria_core>
)

target_include_directories(generics_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(generics_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NORIA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_options(generics_test PRIVATE -fsanitize=address,undefined)
  endif()
endif()

add_executable(constraints_test
  tests/constraints_test.cpp
  src/Constraints.cpp
  src/Types.cpp
)

target_include_directories(constraints_test PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(constraints_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NORIA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_link_options(constraints_test PRIVATE -fsanitize=address,undefined)
  endif()
endif()

add_test(NAME examples COMMAND ${CMAKE_SOURCE_DIR}/tests/run_examples.sh)
add_test(NAME type_representation COMMAND type_representation_test)
add_test(NAME builtin_registry COMMAND builtin_registry_test)
add_test(NAME visitor_smoke COMMAND visitor_smoke_test)
add_test(NAME compiler_facade COMMAND compiler_facade_test)
add_test(NAME module_resolver COMMAND module_resolver_test)
add_test(NAME diagnostic_location COMMAND diagnostic_location_test)
add_test(NAME generics COMMAND generics_test)
add_test(NAME constraints COMMAND constraints_test)
