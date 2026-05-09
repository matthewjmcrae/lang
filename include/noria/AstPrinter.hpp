#pragma once

#include "noria/Ast.hpp"

#include <ostream>

namespace noria {

  void printAst(const ast::Module& module, std::ostream& out);

}

