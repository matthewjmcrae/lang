Monomorphism is used to resolve generics at compile time.

The objective is to return a MonomorphizationResult which contains a map from strings to vector of types, which gives the specialized type arguments for every function declaration.
 results are stored in [[SpecializationCache]] 

The monomorphisation algortihm is as follows

collectPendingSpecializations([[TypeChecker.cpp]])
If(none left) return

linkNewSpeiecliazations()
expandPendingSpecializations
ensureExpansionLimit()
typecheck via [[TypeChecker.cpp]]

loop 


return