#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "ast.h"
#include "codegen.h"

// Run optimization passes on the AST based on the current opt_level.
// At -O1: constant folding, dead code elimination, strength reduction.
// Returns the (possibly modified) AST root.
ASTNode *optimize(ASTNode *program, OptLevel level);

#endif // OPTIMIZER_H
