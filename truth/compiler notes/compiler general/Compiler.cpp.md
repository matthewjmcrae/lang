Facade design pattern to act as a black box for the compiler subsystem.


Handles all steps of compilation at once with one method called CompileSource(). Which returns a struct PipelineOutput containing tokens, a module, and LLVM IR. It contains fields for all 3 to provide a uniform interface for different parts of testing.

Optional parameter compile options is used for passing the stdlib

The stopAfter parameter is used for testing

The compiler gets tokens from 
[[Lexer.cpp]]

turns the tokens into a module (AST) with [[Parser.cpp]]

The tokens and module are passed into a helper function called compileParsedModule which finishes the remainder of compilation.

With imports being resolved if necessary. with resolveImports()

[[TypeChecker.cpp]] is used to check the module from the parser,

then [[monomorphism]] handles specializations of generics which are passed to [[Codegen]] which then generates LLVM IR output

