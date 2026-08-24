The ast is the syntax tree representation of noria.
Nodes have x different primary types:
1. Expressions
2. Statements 
3. Parameter
4. TypeParameter
5. Struct Fields (public/private members)
6. structDecls (struct declaration)
7. Functions
8. Imported Name
9. Imported Decle
10. and Module (which is the entire AST) which contains 

[[astVisitor]] handles double dispatch at node call sites using the visitor design pattern. which isused in printing and codegen when traversing the AST.

[[astMutator]] handles the visitor pattern for handling generics ([[monomorphism]]) and modifies the ast during this process.


