#pragma once

#include <array>
#include <string>
#include <string_view>

namespace noria::runtime {

  inline std::string targetTriple() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "arm64-apple-macosx";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "x86_64-apple-macosx";
#elif defined(__linux__) && defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#elif defined(__linux__) && defined(__x86_64__)
    return "x86_64-unknown-linux-gnu";
#else
    return "";
#endif
  }

  inline std::string targetDataLayout() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "e-m:o-i64:64-i128:128-n32:64-S128";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "e-m:o-i64:64-m:x86-64:32-f80:128-n8:16:32:64-S128";
#elif defined(__linux__) && defined(__aarch64__)
    return "e-m:e-i64:64-i128:128-n32:64-S128";
#elif defined(__linux__) && defined(__x86_64__)
    return "e-m:e-i64:64-f80:128-n8:16:32:64-S128";
#else
    return "";
#endif
  }

  constexpr std::array<std::string_view, 11> runtimeDeclarations = {
      "declare i32 @printf(ptr, ...)\n",
      "declare i32 @puts(ptr)\n",
      "declare i32 @putchar(i32)\n",
      "declare double @llvm.sqrt.f64(double)\n",
      "declare double @llvm.pow.f64(double, double)\n",
      "declare i64 @strlen(ptr)\n",
      "declare ptr @malloc(i64)\n",
      "declare ptr @realloc(ptr, i64)\n",
      "declare void @free(ptr)\n",
      "declare ptr @strcpy(ptr, ptr)\n",
      "declare ptr @strcat(ptr, ptr)\n",
  };

  constexpr std::array<std::string_view, 1> runtimeGlobals = {
      "@.fmt.float = private unnamed_addr constant [4 x i8] c\"%g\\0A\\00\"\n",
  };

  constexpr std::string_view runtimeDefinitions =
      "define void @noria_print_int(i32 %value) {\n"
      "entry:\n"
      "  %is_zero = icmp eq i32 %value, 0\n"
      "  br i1 %is_zero, label %zero, label %check_sign\n"
      "zero:\n"
      "  call i32 @putchar(i32 48)\n"
      "  call i32 @putchar(i32 10)\n"
      "  ret void\n"
      "check_sign:\n"
      "  %is_neg = icmp slt i32 %value, 0\n"
      "  br i1 %is_neg, label %negate, label %digits\n"
      "negate:\n"
      "  call i32 @putchar(i32 45)\n"
      "  %abs = sub i32 0, %value\n"
      "  br label %digits\n"
      "digits:\n"
      "  %n = phi i32 [ %value, %check_sign ], [ %abs, %negate ]\n"
      "  %v = alloca i32\n"
      "  store i32 %n, ptr %v\n"
      "  %pos = alloca i32\n"
      "  store i32 0, ptr %pos\n"
      "  %buf = alloca [12 x i8]\n"
      "  br label %extract\n"
      "extract:\n"
      "  %cur = load i32, ptr %v\n"
      "  %done = icmp eq i32 %cur, 0\n"
      "  br i1 %done, label %print, label %push\n"
      "push:\n"
      "  %digit = urem i32 %cur, 10\n"
      "  %p = load i32, ptr %pos\n"
      "  %ch = add i32 %digit, 48\n"
      "  %slot = getelementptr [12 x i8], ptr %buf, i32 0, i32 %p\n"
      "  %byte = trunc i32 %ch to i8\n"
      "  store i8 %byte, ptr %slot\n"
      "  %pnext = add i32 %p, 1\n"
      "  store i32 %pnext, ptr %pos\n"
      "  %next = udiv i32 %cur, 10\n"
      "  store i32 %next, ptr %v\n"
      "  br label %extract\n"
      "print:\n"
      "  %count = load i32, ptr %pos\n"
      "  %idx = alloca i32\n"
      "  store i32 %count, ptr %idx\n"
      "  br label %print_loop\n"
      "print_loop:\n"
      "  %i = load i32, ptr %idx\n"
      "  %more = icmp ugt i32 %i, 0\n"
      "  br i1 %more, label %print_one, label %newline\n"
      "print_one:\n"
      "  %i1 = sub i32 %i, 1\n"
      "  %cp = getelementptr [12 x i8], ptr %buf, i32 0, i32 %i1\n"
      "  %loaded = load i8, ptr %cp\n"
      "  %c = zext i8 %loaded to i32\n"
      "  call i32 @putchar(i32 %c)\n"
      "  store i32 %i1, ptr %idx\n"
      "  br label %print_loop\n"
      "newline:\n"
      "  call i32 @putchar(i32 10)\n"
      "  ret void\n"
      "}\n\n";

  inline std::string_view runtimeTrapDefinition() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "define private void @\"__noria.rt.trap\"(ptr %msg) {\n"
           "entry:\n"
           "  %len = call i64 @strlen(ptr %msg)\n"
           "  call void asm sideeffect \"svc #0x80\", "
           "\"{x16},{x0},{x1},{x2},~{memory}\" (i64 536870916, i64 2, ptr %msg, i64 %len)\n"
           "  call void asm sideeffect \"svc #0x80\", \"{x16},{x0},~{memory}\" (i64 536870913, "
           "i64 70)\n"
           "  unreachable\n"
           "}\n\n";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "define private void @\"__noria.rt.trap\"(ptr %msg) {\n"
           "entry:\n"
           "  %len = call i64 @strlen(ptr %msg)\n"
           "  call void asm sideeffect \"syscall\", "
           "\"{ax},{di},{si},{dx},~{rcx},~{r11},~{memory}\" (i64 536870916, i64 2, ptr %msg, i64 "
           "%len)\n"
           "  call void asm sideeffect \"syscall\", \"{ax},{di},~{rcx},~{r11},~{memory}\" (i64 "
           "536870913, i64 70)\n"
           "  unreachable\n"
           "}\n\n";
#elif defined(__linux__) && defined(__aarch64__)
    return "define private void @\"__noria.rt.trap\"(ptr %msg) {\n"
           "entry:\n"
           "  %len = call i64 @strlen(ptr %msg)\n"
           "  call void asm sideeffect \"svc #0\", \"{x8},{x0},{x1},{x2},~{memory}\" (i64 64, "
           "i64 2, ptr %msg, i64 %len)\n"
           "  call void asm sideeffect \"svc #0\", \"{x8},{x0},~{memory}\" (i64 93, i64 70)\n"
           "  unreachable\n"
           "}\n\n";
#elif defined(__linux__) && defined(__x86_64__)
    return "define private void @\"__noria.rt.trap\"(ptr %msg) {\n"
           "entry:\n"
           "  %len = call i64 @strlen(ptr %msg)\n"
           "  call void asm sideeffect \"syscall\", "
           "\"{ax},{di},{si},{dx},~{rcx},~{r11},~{memory}\" (i64 1, i64 2, ptr %msg, i64 %len)\n"
           "  call void asm sideeffect \"syscall\", \"{ax},{di},~{rcx},~{r11},~{memory}\" (i64 60, "
           "i64 "
           "70)\n"
           "  unreachable\n"
           "}\n\n";
#else
    return "";
#endif
  }

} // namespace noria::runtime
