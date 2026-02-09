//
// Small helper collector classes for AST traversal
//

#ifndef LLVM_REWRITE_GENERATORS_HELPERCOLLECTORS_H
#define LLVM_REWRITE_GENERATORS_HELPERCOLLECTORS_H

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceLocation.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace clang;

// Helper visitor to collect MaterializeTemporaryExpr nodes from an expression
class TemporaryCollector : public RecursiveASTVisitor<TemporaryCollector> {
private:
    std::vector<const MaterializeTemporaryExpr*> &temporaries;

public:
    explicit TemporaryCollector(std::vector<const MaterializeTemporaryExpr*> &temps)
        : temporaries(temps) {}

    bool VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *matTemp) {
        temporaries.push_back(matTemp);
        return true;
    }
};

// Helper visitor to collect variable references with their declaration locations
class VariableReferenceCollector : public RecursiveASTVisitor<VariableReferenceCollector> {
private:
    std::vector<std::pair<SourceRange, SourceLocation>> &references; // (reference location, decl location)
    const std::set<std::string> &variableNames;

public:
    VariableReferenceCollector(std::vector<std::pair<SourceRange, SourceLocation>> &refs,
                               const std::set<std::string> &varNames)
        : references(refs), variableNames(varNames) {}

    bool VisitDeclRefExpr(DeclRefExpr *declRef) {
        if (auto *varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
            std::string varName = varDecl->getNameAsString();
            if (variableNames.count(varName)) {
                SourceRange refRange = declRef->getSourceRange();
                SourceLocation declLoc = varDecl->getLocation();
                references.push_back({refRange, declLoc});
            }
        }
        return true;
    }
};

#endif // LLVM_REWRITE_GENERATORS_HELPERCOLLECTORS_H
