set shell := ["bash", "-cu"]

llvm_bin := env_var_or_default("LLVM_BIN", "")
build_dir := env_var_or_default("BUILD_DIR", "build")
sanitize_build_dir := env_var_or_default("SANITIZE_BUILD_DIR", "build-sanitize")
noria := build_dir + "/noria"

default:
    @just --list

configure:
    cmake -S . -B {{build_dir}}

build: configure
    cmake --build {{build_dir}}

test: build
    ctest --test-dir {{build_dir}} --output-on-failure

sanitize:
    cmake -S . -B {{sanitize_build_dir}} -DNORIA_ENABLE_SANITIZERS=ON
    cmake --build {{sanitize_build_dir}}
    ctest --test-dir {{sanitize_build_dir}} --output-on-failure

leak: build
    NORIA_RUN_LEAK_CHECKS=1 NORIA_REQUIRE_LEAK_CHECKS=1 ctest --test-dir {{build_dir}} --output-on-failure -L e2e

valgrind: build
    command -v valgrind >/dev/null
    NORIA_PREFIX="valgrind --leak-check=full --error-exitcode=1" ctest --test-dir {{build_dir}} --output-on-failure

fuzz_build_dir := env_var_or_default("FUZZ_BUILD_DIR", "build-fuzz")
fuzz_cxx := env_var_or_default("FUZZ_CXX", "clang++")
fuzz_cc := env_var_or_default("FUZZ_CC", "clang")

fuzz:
    cmake -S . -B {{fuzz_build_dir}} -DNORIA_BUILD_FUZZER=ON \
      -DCMAKE_CXX_COMPILER={{fuzz_cxx}} -DCMAKE_C_COMPILER={{fuzz_cc}}
    cmake --build {{fuzz_build_dir}} --target noria_compile_fuzzer
    {{fuzz_build_dir}}/noria_compile_fuzzer -max_total_time=120 -timeout=5 -max_len=65536 tests/fuzz/corpus

full: test sanitize leak

ctest: build
    ctest --test-dir {{build_dir}} --output-on-failure

tokens file="examples/basic/lexer_smoke.noria": build
    {{noria}} --emit-tokens {{file}}

ast file="examples/basic/factorial.noria": build
    {{noria}} --emit-ast {{file}}

ir file="examples/basic/factorial.noria" output="build/out.ll" opt="-O0": build
    {{noria}} {{opt}} {{file}} -o {{output}}

native file="examples/basic/factorial.noria" output="build/out" opt="-O2": build
    {{noria}} build {{opt}} {{file}} -o {{output}}

run file="examples/basic/factorial.noria" output="build/out" opt="-O2": build
    {{noria}} build {{opt}} {{file}} -o {{output}}
    rc=0; {{output}} || rc=$?; echo "exit code: ${rc}"

manual-native file="examples/basic/factorial.noria" output="build/out" opt="-O0": build
    {{noria}} {{opt}} {{file}} -o {{output}}.ll
    if [[ -n "{{llvm_bin}}" ]]; then llc="{{llvm_bin}}/llc"; else llc="$(command -v llc)"; fi; "${llc}" -filetype=obj {{output}}.ll -o {{output}}.o
    clang {{output}}.o -o {{output}}

format:
    if [[ -n "{{llvm_bin}}" ]]; then clang_format="{{llvm_bin}}/clang-format"; else clang_format="$(command -v clang-format)"; fi; "${clang_format}" -i include/noria/*.hpp src/*.cpp

help:
    @echo "Common recipes:"
    @echo "  just build"
    @echo "  just test"
    @echo "  just sanitize"
    @echo "  just leak"
    @echo "  just fuzz"
    @echo "  just full"
    @echo "  just valgrind"
    @echo "  just tokens examples/basic/lexer_smoke.noria"
    @echo "  just ast examples/basic/factorial.noria"
    @echo "  just ir examples/basic/factorial.noria build/factorial.ll -O2"
    @echo "  just native examples/basic/factorial.noria build/factorial -O2"
    @echo "  just run examples/basic/factorial.noria build/factorial -O2"
