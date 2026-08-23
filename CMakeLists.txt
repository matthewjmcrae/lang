cmake_minimum_required(VERSION 3.20)

project(noria_lang LANGUAGES CXX)

enable_testing()

option(NORIA_ENABLE_SANITIZERS "Build Noria with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(noria
  src/AstPrinter.cpp
  src/Codegen.cpp
  src/Diagnostic.cpp
  src/Lexer.cpp
  src/main.cpp
  src/Parser.cpp
  src/TypeChecker.cpp
  src/Types.cpp
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

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(noria PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NORIA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(noria PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(noria PRIVATE -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "NORIA_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
endif()

add_test(NAME examples COMMAND ${CMAKE_SOURCE_DIR}/tests/run_examples.sh)
add_test(NAME type_representation COMMAND type_representation_test)
add_test(NAME builtin_registry COMMAND builtin_registry_test)
