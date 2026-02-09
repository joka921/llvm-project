//
// Collector for local variable declarations in coroutines
//

#ifndef LLVM_LOCALVARIABLECOLLECTOR_H
#define LLVM_LOCALVARIABLECOLLECTOR_H

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include "../infrastructure/ASTHelpers.h"
#include "../structures/CoroutineStructures.h"

using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

class LocalVariableCollector : public RecursiveASTVisitor<LocalVariableCollector> {
private:
    std::set<LocalVariable> &variables;
    const SourceManager &sourceManager;
    ASTContext &astContext;
    std::map<std::string, std::vector<SourceLocation>> variableNameLocations;

public:
    LocalVariableCollector(std::set<LocalVariable> &vars, const SourceManager &SM, ASTContext &ctx)
        : variables(vars), sourceManager(SM), astContext(ctx) {
    }

    bool hasVariableNameCollisions() const {
        for (const auto &pair : variableNameLocations) {
            if (pair.second.size() > 1) {
                return true;
            }
        }
        return false;
    }

    void reportCollisions(DiagnosticsEngine &diags) const {
        for (const auto &pair : variableNameLocations) {
            if (pair.second.size() > 1) {
                const std::string &varName = pair.first;
                const std::vector<SourceLocation> &locations = pair.second;

                // Report error for the first location
                unsigned diagID = diags.getCustomDiagID(DiagnosticsEngine::Error,
                    "variable name '%0' appears multiple times in coroutine scopes");
                diags.Report(locations[0], diagID) << varName;

                // Report notes for subsequent locations
                unsigned noteID = diags.getCustomDiagID(DiagnosticsEngine::Note,
                    "previous declaration of '%0' here");
                for (size_t i = 1; i < locations.size(); ++i) {
                    diags.Report(locations[i], noteID) << varName;
                }
            }
        }
    }

    std::string getFullyQualifiedTypeName(QualType type) {
        // Get the canonical type (resolves typedefs, auto, etc.)
        QualType canonicalType = type.getCanonicalType();

        // Use a printing policy that produces fully qualified names
        PrintingPolicy policy(astContext.getLangOpts());
        policy.SuppressScope = false; // Include scope information
        policy.SuppressTagKeyword = false; // Keep 'struct', 'class', etc.
        policy.SuppressUnwrittenScope = false; // Include all scopes
        policy.FullyQualifiedName = true; // Force fully qualified names
        policy.PrintCanonicalTypes = true; // Print canonical types

        return canonicalType.getAsString(policy);
    }

    std::pair<std::string, bool> getStorageAndReferenceTypes(QualType type, const Expr* initializer) {
        // Get the canonical type (resolves typedefs, auto, etc.)
        QualType canonicalType = type.getCanonicalType();

        // Use a printing policy that produces fully qualified names
        PrintingPolicy policy(astContext.getLangOpts());
        policy.SuppressScope = false; // Include scope information
        policy.SuppressTagKeyword = false; // Keep 'struct', 'class', etc.
        policy.SuppressUnwrittenScope = false; // Include all scopes
        policy.FullyQualifiedName = true; // Force fully qualified names
        policy.PrintCanonicalTypes = true; // Print canonical types

        std::string storageType;
        std::string referenceType;

        bool isOwning = true;
        if (canonicalType->isReferenceType()) {
            // Variable has reference type (e.g., int& z or auto&& v)
            referenceType = canonicalType.getAsString(policy); // The exact reference type

            isOwning = isPrValue(initializer);
        } else {
            // Variable has object type (e.g., int x or const int y)
            // Storage type is the object type itself
            referenceType = canonicalType.getAsString(policy) + "&";
        }

        return {referenceType, isOwning};
    }

    bool VisitDeclStmt(DeclStmt *declStmt) {
        for (auto *decl: declStmt->decls()) {
            if (auto *varDecl = dyn_cast<VarDecl>(decl)) {
                if (!varDecl->getType()->isFunctionType()) {
                    LocalVariable var;
                    var.name = varDecl->getNameAsString();
                    var.type = getFullyQualifiedTypeName(varDecl->getType());

                    // Get both storage and reference types
                    auto [referenceType, isOwning] = getStorageAndReferenceTypes(varDecl->getType(), varDecl->getInit() );
                    var.isOwning = isOwning;
                    var.referenceType = referenceType;
                    var.isReference = varDecl->getType().getCanonicalType()->isReferenceType();
                    var.location = varDecl->getLocation();
                    var.priority = sourceManager.getFileOffset(var.location); // Use file offset as priority

                    if (!var.name.empty()) {
                        // Track variable name for collision detection
                        variableNameLocations[var.name].push_back(var.location);

                        variables.insert(var);
                    }
                }
            }
        }
        return true;
    }
};

#endif //LLVM_LOCALVARIABLECOLLECTOR_H
