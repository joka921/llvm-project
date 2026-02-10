//
// CoroutineRewriter - Implementation
//

#include "CoroutineRewriter.h"
#include "CoroutineBodyRewriter.h"
#include "infrastructure/ASTHelpers.h"
#include "infrastructure/ReplacementApplicator.h"
#include "collectors/LocalVariableCollector.h"
#include "codegen/MacroCodeGenerator.h"
#include "clang/Lex/Lexer.h"

#include <sstream>
#include <string>

bool CoroutineRewriter::containsCoroutineKeywords(const Stmt *stmt) {
        if (!stmt) return false;

        if (isa<CoawaitExpr>(stmt) || isa<CoyieldExpr>(stmt) || isa<CoreturnStmt>(stmt)) {
            return true;
        }

        for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it) {
            if (const Stmt *child = *it) {
                if (containsCoroutineKeywords(child)) {
                    return true;
                }
            }
        }
        return false;
    }

bool CoroutineRewriter::containsTryCatchBlocks(const Stmt *stmt) {
        if (!stmt) return false;

        if (isa<CXXTryStmt>(stmt)) {
            return true;
        }

        for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it) {
            if (const Stmt *child = *it) {
                if (containsTryCatchBlocks(child)) {
                    return true;
                }
            }
        }
        return false;
    }

bool CoroutineRewriter::collectLocalVariables(const Stmt *body, std::set<LocalVariable> &variables) {
        LocalVariableCollector collector(variables, sourceManager, *astContext);
        collector.TraverseStmt(const_cast<Stmt *>(body));

        // Check for variable name collisions
        if (collector.hasVariableNameCollisions()) {
            DiagnosticsEngine &diags = astContext->getDiagnostics();
            collector.reportCollisions(diags);
            return false; // Collision detected, cannot rewrite
        }

        return true; // No collisions, safe to rewrite
    }

void CoroutineRewriter::collectFunctionParameters(const FunctionDecl *funcDecl, std::vector<FunctionParameter> &parameters) {
        REWRITE_LOG() << "  DEBUG: Collecting function parameters\n";

        for (const auto *param: funcDecl->parameters()) {
            FunctionParameter fp;
            fp.name = param->getNameAsString();
            fp.qualType = param->getType();

            // Get the type as string for debugging/logging
            PrintingPolicy policy(astContext->getLangOpts());
            fp.type = fp.qualType.getAsString(policy);

            parameters.push_back(fp);

            REWRITE_LOG() << "    Found parameter: " << fp.type << " " << fp.name << "\n";
        }

        REWRITE_LOG() << "  Found " << parameters.size() << " function parameters\n";
    }

void CoroutineRewriter::collectMemberFunctionInfo(const FunctionDecl *funcDecl, CoroutineInfo &coro) {
        if (const auto *methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
            coro.isMemberFunction = true;
            coro.isConstMemberFunction = methodDecl->isConst();

            if (const auto *recordDecl = methodDecl->getParent()) {
                coro.className = recordDecl->getQualifiedNameAsString();
            }

            REWRITE_LOG() << "  DEBUG: Member function detected\n";
            REWRITE_LOG() << "    Class: " << coro.className << "\n";
            REWRITE_LOG() << "    Is const: " << (coro.isConstMemberFunction ? "yes" : "no") << "\n";
        } else {
            coro.isMemberFunction = false;
            REWRITE_LOG() << "  DEBUG: Free function (not a member function)\n";
        }
    }

std::string CoroutineRewriter::replaceLastTemplateArgWithHandle(const std::string &returnType) {
        REWRITE_LOG() << "    DEBUG: replaceLastTemplateArgWithHandle input: '" << returnType << "'\n";

        // Find the template arguments by looking for < and >
        size_t openAngle = returnType.find('<');
        if (openAngle == std::string::npos) {
            REWRITE_LOG() << "    DEBUG: No template arguments found, returning unchanged\n";
            return returnType;
        }

        // Find the matching closing angle bracket
        size_t closeAngle = returnType.rfind('>');
        if (closeAngle == std::string::npos || closeAngle <= openAngle) {
            REWRITE_LOG() << "    DEBUG: Invalid template syntax, returning unchanged\n";
            return returnType;
        }

        // Extract the template arguments part
        std::string templateArgs = returnType.substr(openAngle + 1, closeAngle - openAngle - 1);
        REWRITE_LOG() << "    DEBUG: Template arguments: '" << templateArgs << "'\n";

        // Parse template arguments (simple comma splitting, ignoring nested templates for now)
        std::vector<std::string> args;
        size_t start = 0;
        int depth = 0;
        for (size_t i = 0; i <= templateArgs.length(); ++i) {
            if (i == templateArgs.length() || (templateArgs[i] == ',' && depth == 0)) {
                if (i > start) {
                    std::string arg = templateArgs.substr(start, i - start);
                    // Trim whitespace
                    size_t first = arg.find_first_not_of(" \t");
                    size_t last = arg.find_last_not_of(" \t");
                    if (first != std::string::npos && last != std::string::npos) {
                        arg = arg.substr(first, last - first + 1);
                    }
                    args.push_back(arg);
                }
                start = i + 1;
            } else if (templateArgs[i] == '<') {
                depth++;
            } else if (templateArgs[i] == '>') {
                depth--;
            }
        }

        REWRITE_LOG() << "    DEBUG: Parsed " << args.size() << " template arguments:\n";
        for (size_t i = 0; i < args.size(); ++i) {
            REWRITE_LOG() << "      [" << i << "]: '" << args[i] << "'\n";
        }

        if (args.empty()) {
            REWRITE_LOG() << "    DEBUG: No template arguments found after parsing\n";
            return returnType;
        }

        // Replace the last argument with Handle
        REWRITE_LOG() << "    DEBUG: Replacing last argument '" << args.back() << "' with 'Handle'\n";
        args.back() = "Handle";

        // Reconstruct the type
        std::string prefix = returnType.substr(0, openAngle + 1);
        std::string suffix = returnType.substr(closeAngle);

        std::string newTemplateArgs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) newTemplateArgs += ", ";
            newTemplateArgs += args[i];
        }

        std::string result = prefix + newTemplateArgs + suffix;
        REWRITE_LOG() << "    DEBUG: replaceLastTemplateArgWithHandle output: '" << result << "'\n";
        return result;
    }

SourceLocation CoroutineRewriter::findStructInsertionPoint(const FunctionDecl *funcDecl) {
        REWRITE_LOG() << "DEBUG: findStructInsertionPoint called for function: " << funcDecl->getQualifiedNameAsString()
                << "\n";

        const Stmt *bodyStmt = funcDecl->getBody();
        if (!bodyStmt) {
            //REWRITE_LOG() << "DEBUG: Function has no body\n";
            return SourceLocation();
        }

        //REWRITE_LOG() << "DEBUG: Function has body, type: " << bodyStmt->getStmtClassName() << "\n";

        // Handle CoroutineBodyStmt wrapper
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            //REWRITE_LOG() << "DEBUG: Body is CoroutineBodyStmt, getting inner body\n";
            bodyStmt = coroBody->getBody();
            if (!bodyStmt) {
                //REWRITE_LOG() << "DEBUG: CoroutineBodyStmt has no inner body\n";
                return SourceLocation();
            }
            //REWRITE_LOG() << "DEBUG: Inner body type: " << bodyStmt->getStmtClassName() << "\n";
        }

        if (auto *body = dyn_cast<CompoundStmt>(bodyStmt)) {
            //REWRITE_LOG() << "DEBUG: Body is CompoundStmt\n";
            SourceLocation lbracLoc = body->getLBracLoc();
            //REWRITE_LOG() << "DEBUG: getLBracLoc() returned location, checking validity\n";

            if (lbracLoc.isValid()) {
                //REWRITE_LOG() << "DEBUG: lbracLoc is valid, calling Lexer::getLocForEndOfToken\n";
                SourceLocation endLoc = Lexer::getLocForEndOfToken(lbracLoc, 0, sourceManager, langOptions);
                if (endLoc.isValid()) {
                    //REWRITE_LOG() << "DEBUG: Successfully found insertion point\n";
                    return endLoc;
                } else {
                    //REWRITE_LOG() << "DEBUG: Lexer::getLocForEndOfToken returned invalid location\n";
                }
            } else {
                REWRITE_LOG() << "DEBUG: lbracLoc is invalid\n";
            }
        } else {
            REWRITE_LOG() << "DEBUG: Final body is not CompoundStmt, type: " << bodyStmt->getStmtClassName() << "\n";
        }

        REWRITE_LOG() << "DEBUG: Returning invalid SourceLocation\n";
        return SourceLocation();
    }

SourceLocation CoroutineRewriter::findPreFunctionInsertionPoint(const FunctionDecl *funcDecl) {
        SourceLocation loc = funcDecl->getSourceRange().getBegin();
        // For template functions, go to the template declaration start
        if (const FunctionTemplateDecl *tmplDecl = funcDecl->getDescribedFunctionTemplate()) {
            loc = tmplDecl->getSourceRange().getBegin();
        }
        return loc;
    }

std::string CoroutineRewriter::generateCoroImplStruct(const CoroutineInfo &coro) {
        // Extract the return type from the coroutine function
        std::string returnType = "auto"; // Default fallback
        if (coro.function) {
            QualType retType = coro.function->getReturnType();

            // Use canonical type to handle implicitly defaulted template arguments
            QualType canonicalType = retType.getCanonicalType();

            PrintingPolicy policy(astContext->getLangOpts());
            policy.SuppressScope = false;
            policy.PrintCanonicalTypes = true; // Print canonical types to see defaulted args

            std::string originalReturnType = canonicalType.getAsString(policy);
            REWRITE_LOG() << "  DEBUG: Original canonical return type: " << originalReturnType << "\n";

            // Also print the non-canonical version for comparison
            std::string nonCanonicalType = retType.getAsString(policy);
            REWRITE_LOG() << "  DEBUG: Non-canonical return type: " << nonCanonicalType << "\n";

            // Replace last template argument with HANDLE (work on canonical type)
            returnType = replaceLastTemplateArgWithHandle(originalReturnType);
            REWRITE_LOG() << "  DEBUG: Modified return type: " << returnType << "\n";
        }
        std::string structCode = "\n  // _coro_storage and CoroImpl assumed to be available in global namespace\n";

        /*
        // Commented out - assumed to be available globally
        structCode += "  template<typename T>\n";
        structCode += "  struct _coro_storage {\n";
        structCode += "    alignas(T) char buffer[sizeof(T)];\n";
        structCode += "    bool constructed = false;\n";
        structCode += "\n";
        structCode += "    template<typename... Args>\n";
        structCode += "    void construct(Args&&... args) {\n";
        structCode += "      new(buffer) T(std::forward<Args>(args)...);\n";
        structCode += "      constructed = true;\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    void destroy() {\n";
        structCode += "      if (constructed) {\n";
        structCode += "        reinterpret_cast<T*>(buffer)->~T();\n";
        structCode += "        constructed = false;\n";
        structCode += "      }\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    T& get() {\n";
        structCode += "      return *reinterpret_cast<T*>(buffer);\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    const T& get() const {\n";
        structCode += "      return *reinterpret_cast<const T*>(buffer);\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    ~_coro_storage() {\n";
        structCode += "      destroy();\n";
        structCode += "    }\n";
        structCode += "  };\n";
        structCode += "\n";
        */

        // Generate the data structure
        structCode += "  struct _detail_coro_impl {\n";

        // Debug: Print all variables that will be added to the struct
        REWRITE_LOG() << "  DEBUG: generateCoroImplStruct - Processing " << coro.localVariables.size() <<
                " variables:\n";
        for (const auto &var: coro.localVariables) {
            REWRITE_LOG() << "    Variable: " << var.type << " " << var.name << "\n";
        }

        // Add __self member for member functions (must be first)
        if (coro.isMemberFunction) {
            structCode += "    // Member function 'this' pointer\n";
            std::string selfType;
            if (coro.isConstMemberFunction) {
                selfType = "const " + coro.className + "*";
            } else {
                selfType = coro.className + "*";
            }
            structCode += "    " + selfType + " __self;\n\n";
            REWRITE_LOG() << "    Added __self member to struct: " << selfType << " __self\n";
        }

        // Add function parameters
        if (!coro.parameters.empty()) {
            structCode += "    // Function parameters\n";
            for (const auto &param: coro.parameters) {
                std::string paramType = "decltype(" + param.name + ")";
                structCode += "    " + paramType + " " + param.name + ";\n";
                REWRITE_LOG() << "    Added parameter to struct: " << paramType << " " << param.name << "\n";
            }
            structCode += "\n";
        }

        // Add all local variables (including ranged-for variables)
        // Use declaration location to deduplicate and handle shadowing
        if (coro.localVariables.empty()) {
            structCode += "    // No local variables found in this coroutine\n";
        } else {
            structCode += "    // Local variables (including ranged-for loop variables)\n";
            std::set<SourceLocation> addedDeclLocations; // Track which declarations we've already added

            for (const auto &var: coro.localVariables) {
                // Skip if we've already added this exact variable declaration
                if (addedDeclLocations.count(var.location) > 0) {
                    REWRITE_LOG() << "    Skipping true duplicate variable: " << var.name
                                 << " at " << var.location.printToString(sourceManager) << "\n";
                    continue;
                }
                addedDeclLocations.insert(var.location);

                // Get the member name from the pre-built mapping in CoroutineInfo
                std::string memberName = var.name; // Default to original name
                auto it = coro.declLocationToMemberName.find(var.location);
                if (it != coro.declLocationToMemberName.end()) {
                    memberName = it->second;
                }

                structCode += "    _coro_storage<" + var.referenceType + ", " + (var.isOwning
                            ? std::string{"true"}
                            : std::string{"false"}) + "> " + memberName +
                        ";\n";

                REWRITE_LOG() << "    Added variable: " << memberName << " (original name: " << var.name
                             << ", location: " << var.location.printToString(sourceManager) << ")\n";
            }
        }

        // Add storage for subexpression temporaries
        // Collect all unique temporaries from all coroutine statements
        std::set<std::string> addedTemporaries; // Track which temporaries we've already added
        bool hasTemporaries = false;
        for (const auto &coroStmt : coro.coroutineStatements) {
            if (!coroStmt.temporaries.empty()) {
                hasTemporaries = true;
                break;
            }
        }

        if (hasTemporaries) {
            structCode += "\n    // Subexpression temporaries\n";
            for (const auto &coroStmt : coro.coroutineStatements) {
                for (const auto &temp : coroStmt.temporaries) {
                    // Only add each temporary once
                    if (addedTemporaries.find(temp.tempVarName) == addedTemporaries.end()) {
                        addedTemporaries.insert(temp.tempVarName);

                        // Get the canonical type string for the temporary
                        std::string typeStr = typeAsString(temp.type, *astContext);

                        // All subexpression temporaries are owning (they store the actual object)
                        // and we access them by reference
                        structCode += "    _coro_storage<" + typeStr + "&, true> " + temp.tempVarName + ";\n";

                        REWRITE_LOG() << "    Added temporary to struct: _coro_storage<" << typeStr << "&, true> " << temp.tempVarName << "\n";
                    }
                }
            }
        }

        // Add yield buffer for temporaries if needed
        // Add exception handling infrastructure
        if (!coro.tryCatchBlocks.empty()) {
            structCode += "\n    // Exception handling infrastructure\n";
            structCode += "    std::vector<size_t> activeTryBlocks;\n";

            // Add handleException function
            structCode += "\n    void handleException(std::exception_ptr eptr, size_t& nextState, std::function<void()> resume) {\n";
            structCode += "        destroyBecauseOfExceptionHandling(activeTryBlocks.back());\n";
            structCode += "      nextState = dispatchExceptionHandling(std::move(eptr));\n";
            structCode += "      resume();\n";
            structCode += "    }\n";

            // Add dispatchExceptionHandling function
            structCode += "\n    size_t dispatchExceptionHandling(std::exception_ptr eptr) {\n";
            structCode += "      switch (activeTryBlocks.back()) {\n";
            for (const auto &tryCatch : coro.tryCatchBlocks) {
                structCode += "        case " + std::to_string(tryCatch.resumeIndex) + ": return catchClauseImpl_" + std::to_string(tryCatch.resumeIndex) + "(std::move(eptr));\n";
            }
            structCode += "        default: std::terminate();\n";
            structCode += "      }\n";
            structCode += "    }\n";

            // Add catch clause implementation member functions
            structCode += "\n    // Exception handler member functions\n";
            for (const auto &tryCatch : coro.tryCatchBlocks) {
                structCode += "    size_t catchClauseImpl_" + std::to_string(tryCatch.resumeIndex) + "(std::exception_ptr eptr) {\n";
                structCode += "      auto nextState = activeTryBlocks.back();\n";
                structCode += "      activeTryBlocks.pop_back();\n";
                structCode += "      auto lambda = [&]() {\n";
                structCode += "        try {\n";
                structCode += "          std::rethrow_exception(eptr);\n";
                structCode += "        } ";

                // Add all catch clauses
                for (const auto &catchClause : tryCatch.catchClauses) {
                    structCode += catchClause + " ";
                }

                structCode += "\n        return nextState;\n";
                structCode += "      };\n";
                structCode += "      if (activeTryBlocks.empty()) {\n";
                structCode += "        return lambda();\n";
                structCode += "      } else {\n";
                structCode += "        try {\n";
                structCode += "          return lambda();\n";
                structCode += "        } catch (...) {\n";
                structCode += "          return dispatchExceptionHandling(std::current_exception());\n";
                structCode += "        }\n";
                structCode += "      }\n";
                structCode += "    }\n";

                REWRITE_LOG() << "    Added catchClauseImpl_" << tryCatch.resumeIndex << " to struct\n";
            }

            // Add destroyBecauseOfException function
            structCode += "\n    // Destroy variables in case of exception in try block\n";
            structCode += "    void destroyBecauseOfException(size_t tryCatchBlockIndex) {\n";
            structCode += "      switch (tryCatchBlockIndex) {\n";
            for (const auto &tryCatch : coro.tryCatchBlocks) {
                structCode += "        case " + std::to_string(tryCatch.resumeIndex) + ":\n";
                // Destroy all variables in this try block in reverse order
                for (const auto &varName : tryCatch.variablesInTryBlock) {
                    structCode += "          if (" + varName + ".constructed) { " + varName + ".destroy(); }\n";
                }
                structCode += "          break;\n";
            }
            structCode += "        default: break;\n";
            structCode += "      }\n";
            structCode += "    }\n";
            REWRITE_LOG() << "    Added destroyBecauseOfException function\n";
        }

        // Add destroySuspendedCoro function (always needed if there are any coroutine statements)
        if (!coro.coroutineStatements.empty()) {
            structCode += "\n    // Destroy variables when coroutine is suspended at a specific state\n";
            structCode += "    void destroySuspendedCoro(size_t curState) {\n";
            structCode += "      switch (curState) {\n";

            // Generate cases in descending order (from highest index to lowest)
            // This allows us to use fallthrough and goto for proper destruction order
            std::vector<const CoroutineStatement*> sortedStmts;
            for (const auto &stmt : coro.coroutineStatements) {
                sortedStmts.push_back(&stmt);
            }
            std::sort(sortedStmts.begin(), sortedStmts.end(),
                     [](const CoroutineStatement* a, const CoroutineStatement* b) {
                         return a->index > b->index;  // Descending order
                     });

            for (size_t i = 0; i < sortedStmts.size(); ++i) {
                const CoroutineStatement &currentStmt = *sortedStmts[i];
                structCode += "        case " + std::to_string(currentStmt.index) + ":\n";

                // Find the next state we'll process (next in sorted list = next lower index)
                const CoroutineStatement *nextStmt = (i + 1 < sortedStmts.size()) ? sortedStmts[i + 1] : nullptr;

                // Determine which variables to destroy: those in current but NOT in next
                // These are the variables that were added since the previous state
                std::vector<std::string> varsToDestroy;
                for (const auto &varName : currentStmt.aliveVariables) {
                    bool inNext = false;
                    if (nextStmt) {
                        for (const auto &nextVar : nextStmt->aliveVariables) {
                            if (varName == nextVar) {
                                inNext = true;
                                break;
                            }
                        }
                    }
                    if (!inNext) {
                        varsToDestroy.push_back(varName);
                    }
                }

                // Destroy only the new variables
                for (const auto &varName : varsToDestroy) {
                    structCode += "          " + varName + ".destroy();\n";
                }

                // Find the target state to continue with:
                // The first earlier state whose alive variables are ALL contained in current state
                const CoroutineStatement *targetStmt = nullptr;
                for (size_t j = i + 1; j < sortedStmts.size(); ++j) {
                    const CoroutineStatement *candidateStmt = sortedStmts[j];

                    // Check if ALL variables in candidate are also in current
                    bool allContained = true;
                    for (const auto &candidateVar : candidateStmt->aliveVariables) {
                        bool found = false;
                        for (const auto &currentVar : currentStmt.aliveVariables) {
                            if (candidateVar == currentVar) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            allContained = false;
                            break;
                        }
                    }

                    if (allContained) {
                        targetStmt = candidateStmt;
                        break;  // Found the first matching state
                    }
                }

                // Determine if we need goto or fallthrough
                if (targetStmt) {
                    // Check if target is the immediately next state
                    if (nextStmt && targetStmt->index == nextStmt->index) {
                        // Fall through naturally to the next state
                    } else {
                        // Jump to the target state's cleanup label
                        structCode += "          goto cleanup_" + std::to_string(targetStmt->index) + ";\n";
                    }
                } else {
                    // No more states to process
                    structCode += "          break;\n";
                }

                // Add cleanup label if this is a potential goto target
                if (i + 1 < sortedStmts.size()) {
                    const CoroutineStatement &nextStmt = *sortedStmts[i + 1];
                    structCode += "        cleanup_" + std::to_string(nextStmt.index) + ":\n";
                }
            }

            structCode += "        case 0:  // initial state\n";
            structCode += "          break;\n";
            structCode += "      }\n";
            structCode += "    }\n";
            REWRITE_LOG() << "    Added destroySuspendedCoro function\n";
        }

        structCode += "  };\n\n";

        // Add typedef to avoid comma issues in macro call
        structCode += "  using _ActualCoroType = " + returnType + ";\n";

        // Generate the COROUTINE_HEADER macro call
        // The coroutine body will follow immediately after this
        if (!coro.tryCatchBlocks.empty()) {
            structCode += "  COROUTINE_HEADER_WITH_TRY(_ActualCoroType, _detail_coro_impl) ";
        } else {
            structCode += "  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl) ";
        }

        // Generate goto-based dispatch switch
        // Collect all resume point indices (suspension points + TRY_END resume points)
        // CO_RETURN indices are NOT included — they don't create resume points
        structCode += "\nswitch(this->curState) {\n  case 0: break;\n";
        for (const auto &coroStmt : coro.coroutineStatements) {
            if (coroStmt.type != CoroutineStatement::RETURN) {
                structCode += "  case " + std::to_string(coroStmt.index) +
                              ": goto label_" + std::to_string(coroStmt.index) + ";\n";
            }
        }
        for (const auto &tryCatch : coro.tryCatchBlocks) {
            structCode += "  case " + std::to_string(tryCatch.resumeIndex) +
                          ": goto label_" + std::to_string(tryCatch.resumeIndex) + ";\n";
        }
        structCode += "  default: return;\n}\n";

        return structCode;
    }

bool CoroutineRewriter::VisitFunctionDecl(FunctionDecl *funcDecl) {
        if (!funcDecl->hasBody()) {
            return true;
        }

        auto fileId = sourceManager.getFileID(funcDecl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            return true;
        }

        if (containsCoroutineKeywords(funcDecl->getBody())) {
            CoroutineInfo coro;
            coro.function = funcDecl;

            REWRITE_LOG() << "Processing coroutine: " << funcDecl->getQualifiedNameAsString() << "\n";

            // Check for variable name collisions - skip rewriting if found
            if (!collectLocalVariables(funcDecl->getBody(), coro.localVariables)) {
                REWRITE_LOG() << "Skipping coroutine " << funcDecl->getQualifiedNameAsString()
                        << " due to variable name collisions\n";
                return true; // Skip this coroutine
            }

            collectFunctionParameters(funcDecl, coro.parameters);
            collectMemberFunctionInfo(funcDecl, coro);

            coro.insertionPoint = findStructInsertionPoint(funcDecl);
            if (coro.insertionPoint.isInvalid()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Warning,
                    "Could not find insertion point for _detail_coro_impl struct");
                diagnosticsEngine.Report(funcDecl->getLocation(), diagID);
                coro.hasError = true;

                // Output the struct code to console as fallback
                std::string structCode = generateCoroImplStruct(coro);
                REWRITE_LOG() << "WARNING: Could not insert struct, here's what would have been inserted:\n";
                REWRITE_LOG() << "========== STRUCT CODE ==========\n";
                REWRITE_LOG() << structCode;
                REWRITE_LOG() << "==================================\n";
            }

            coroutines.push_back(coro);

            REWRITE_LOG() << "  Found " << coro.localVariables.size() << " local variables\n";

            if (const auto *methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
                REWRITE_LOG() << "  Type: Member function of " << methodDecl->getParent()->getQualifiedNameAsString() <<
                        "\n";
            } else {
                REWRITE_LOG() << "  Type: Free function\n";
            }
        }

        return true;
    }

bool CoroutineRewriter::VisitLambdaExpr(LambdaExpr *lambdaExpr) {
        // Get the call operator (the lambda body function)
        const CXXMethodDecl *callOperator = lambdaExpr->getCallOperator();
        if (!callOperator->hasBody()) {
            return true;
        }

        auto fileId = sourceManager.getFileID(lambdaExpr->getBeginLoc());
        if (fileId != sourceManager.getMainFileID()) {
            return true;
        }

        if (containsCoroutineKeywords(callOperator->getBody())) {
            // Check for captures - we don't support lambda coroutines with captures yet
            unsigned numCaptures = lambdaExpr->capture_size();

            if (numCaptures > 0) {
                // Report error for lambda coroutines with captures
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Error,
                    "coroutine lambdas with captures are not supported");
                diagnosticsEngine.Report(lambdaExpr->getBeginLoc(), diagID);

                // Also report each capture location for clarity
                unsigned noteID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Note,
                    "capture found here");
                for (const auto& capture : lambdaExpr->captures()) {
                    diagnosticsEngine.Report(capture.getLocation(), noteID);
                }

                REWRITE_LOG() << "Skipping lambda coroutine due to captures\n";
                return true; // Skip this lambda coroutine
            }

            CoroutineInfo coro;
            coro.function = callOperator;
            coro.isLambda = true;
            coro.lambdaExpr = lambdaExpr;

            REWRITE_LOG() << "Processing lambda coroutine\n";

            // Check for variable name collisions - skip rewriting if found
            if (!collectLocalVariables(callOperator->getBody(), coro.localVariables)) {
                REWRITE_LOG() << "Skipping lambda coroutine due to variable name collisions\n";
                return true; // Skip this lambda coroutine
            }

            // Collect lambda parameters - they need to be added to the impl struct
            collectFunctionParameters(callOperator, coro.parameters);

            // Lambda coroutines are not member functions of classes in the traditional sense
            // collectMemberFunctionInfo is not needed for lambdas

            coro.insertionPoint = findStructInsertionPoint(callOperator);
            if (coro.insertionPoint.isInvalid()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Warning,
                    "Could not find insertion point for lambda _detail_coro_impl struct");
                diagnosticsEngine.Report(lambdaExpr->getBeginLoc(), diagID);
                coro.hasError = true;

                // Output the struct code to console as fallback
                std::string structCode = generateCoroImplStruct(coro);
                REWRITE_LOG() << "WARNING: Could not insert lambda struct, here's what would have been inserted:\n";
                REWRITE_LOG() << "========== STRUCT CODE ==========\n";
                REWRITE_LOG() << structCode;
                REWRITE_LOG() << "==================================\n";
            }

            coroutines.push_back(coro);

            REWRITE_LOG() << "  Found " << coro.localVariables.size() << " local variables in lambda\n";
            REWRITE_LOG() << "  Type: Lambda coroutine\n";
        }

        return true;
    }

void CoroutineRewriter::updateFunctionReturnType(const CoroutineInfo &coro) {
        if (coro.isLambda) {
            REWRITE_LOG() << "Updating lambda return type\n";
            updateLambdaReturnType(coro);
            return;
        }

        REWRITE_LOG() << "Updating function return type for: " << coro.function->getQualifiedNameAsString() << "\n";

        if (!coro.function) {
            REWRITE_LOG() << "  ERROR: No function to update\n";
            return;
        }

        // Get the return type location
        SourceRange returnTypeRange = coro.function->getReturnTypeSourceRange();
        if (returnTypeRange.isInvalid()) {
            REWRITE_LOG() << "  ERROR: Invalid return type source range\n";
            return;
        }

        // Use the same logic as generateCoroImplStruct to get the canonical type
        QualType retType = coro.function->getReturnType();
        QualType canonicalType = retType.getCanonicalType();

        PrintingPolicy policy(astContext->getLangOpts());
        policy.SuppressScope = false;
        policy.PrintCanonicalTypes = true;

        std::string originalReturnType = canonicalType.getAsString(policy);

        // Replace last template argument with Handle (same as typedef)
        std::string modifiedReturnType = replaceLastTemplateArgWithHandle(originalReturnType);

        REWRITE_LOG() << "  DEBUG: Original canonical return type: " << originalReturnType << "\n";
        REWRITE_LOG() << "  DEBUG: Modified return type: " << modifiedReturnType << "\n";

        if (originalReturnType != modifiedReturnType) {
            rewriter.ReplaceText(returnTypeRange, modifiedReturnType);
            REWRITE_LOG() << "  Updated function return type\n";
        } else {
            REWRITE_LOG() << "  Return type unchanged (no template arguments found)\n";
        }
    }

void CoroutineRewriter::updateLambdaReturnType(const CoroutineInfo &coro) {
        if (!coro.lambdaExpr) {
            REWRITE_LOG() << "  ERROR: No lambda expression to update\n";
            return;
        }

        // Check if lambda has an explicit trailing return type
        if (!coro.lambdaExpr->hasExplicitResultType()) {
            unsigned diagID = diagnosticsEngine.getCustomDiagID(
                clang::DiagnosticsEngine::Error,
                "coroutine lambda must have an explicit trailing return type");
            diagnosticsEngine.Report(coro.lambdaExpr->getBeginLoc(), diagID);
            return;
        }

        // Find and delete the trailing return type by searching for "-> ReturnType"
        SourceRange lambdaRange = coro.lambdaExpr->getSourceRange();
        SourceLocation lambdaStart = lambdaRange.getBegin();
        SourceLocation lambdaEnd = lambdaRange.getEnd();
        
        // Get the source text of the entire lambda
        auto txt = rewriter.getRewrittenText(lambdaRange);
        StringRef lambdaText = txt;
        if (lambdaText.empty()) {
            // Fallback: get original source text
            bool invalid = false;
            lambdaText = sourceManager.getCharacterData(lambdaStart, &invalid);
            if (invalid) {
                REWRITE_LOG() << "  ERROR: Cannot get lambda source text\n";
                return;
            }
            
            // Calculate length manually
            unsigned startOffset = sourceManager.getFileOffset(lambdaStart);
            unsigned endOffset = sourceManager.getFileOffset(lambdaEnd);
            if (endOffset <= startOffset) {
                REWRITE_LOG() << "  ERROR: Invalid lambda range\n";
                return;
            }
            lambdaText = lambdaText.substr(0, endOffset - startOffset + 1);
        }
        
        // Look for "-> " pattern in the lambda text
        size_t arrowPos = lambdaText.find("-> ");
        if (arrowPos == StringRef::npos) {
            REWRITE_LOG() << "  ERROR: Cannot find trailing return type arrow\n";
            return;
        }
        
        // Find the opening brace after the return type
        size_t bracePos = lambdaText.find('{', arrowPos);
        if (bracePos == StringRef::npos) {
            REWRITE_LOG() << "  ERROR: Cannot find lambda body opening brace\n";
            return;
        }
        
        // Calculate the range to delete: from "-> " to just before the '{'
        SourceLocation deleteStart = lambdaStart.getLocWithOffset(arrowPos);
        SourceLocation deleteEnd = lambdaStart.getLocWithOffset(bracePos - 1);
        
        // Delete the trailing return type (including the "-> " part)
        SourceRange deleteRange(deleteStart, deleteEnd);
        rewriter.RemoveText(deleteRange);
        
        REWRITE_LOG() << "  Deleted lambda trailing return type from position " << arrowPos 
                      << " to " << (bracePos - 1) << "\n";
    }

void CoroutineRewriter::performRewrites() {
        for (auto &coro: coroutines) {
            if (coro.hasError) {
                continue;
            }

            if (coro.insertionPoint.isValid()) {
                // Update the function's return type before rewriting the body
                updateFunctionReturnType(coro);

                // First rewrite the coroutine body (this adds ranged-for variables to localVariables)
                rewriteCoroutineBody(coro);

                // THEN generate the struct with all variables (including ranged-for vars)
                std::string structCode = generateCoroImplStruct(coro);
                rewriter.InsertTextBefore(coro.insertionPoint, structCode);
                REWRITE_LOG() << "Inserted _detail_coro_impl struct into "
                        << coro.function->getQualifiedNameAsString() << "\n";

                // Insert lambda functor structs BEFORE the coroutine function
                if (!coro.lambdasInBody.empty()) {
                    SourceLocation preFuncLoc = findPreFunctionInsertionPoint(coro.function);
                    if (preFuncLoc.isValid()) {
                        std::string allLambdaStructs;
                        for (const auto &lambda : coro.lambdasInBody) {
                            allLambdaStructs += lambda.classDefinition + ";\n\n";
                        }
                        rewriter.InsertTextBefore(preFuncLoc, allLambdaStructs);
                        REWRITE_LOG() << "Inserted " << coro.lambdasInBody.size()
                                      << " lambda struct(s) before " << coro.function->getQualifiedNameAsString() << "\n";
                    }
                }
            }
        }
    }

void CoroutineRewriter::rewriteCoroutineBody(CoroutineInfo &coro) {
        REWRITE_LOG() << "Rewriting coroutine body for: " << coro.function->getQualifiedNameAsString() << "\n";

        const Stmt *bodyStmt = coro.function->getBody();
        if (!bodyStmt) {
            return;
        }

        // Handle CoroutineBodyStmt wrapper
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            bodyStmt = coroBody->getBody();
        }

        if (bodyStmt) {
            // First pass: collect ranged-for loops only (don't apply any replacements yet)
            const CXXRecordDecl *classRecord = nullptr;
            if (coro.isMemberFunction) {
                if (const auto *methodDecl = dyn_cast<CXXMethodDecl>(coro.function)) {
                    classRecord = methodDecl->getParent();
                }
            }
            CoroutineBodyRewriter initialRewriter(coro.localVariables, rewriter, sourceManager,
                                                  coro, *astContext, coro.isMemberFunction, classRecord);
            initialRewriter.TraverseStmt(const_cast<Stmt *>(bodyStmt));

            // Get the ranged-for loops and add them to the coroutine info for struct generation
            const auto &rangedForLoops = initialRewriter.getRangedForLoops();
            const_cast<CoroutineInfo &>(coro).rangedForLoops = rangedForLoops;
            REWRITE_LOG() << "  DEBUG: Found " << rangedForLoops.size() << " ranged-for loops\n";

            // Get the try-catch blocks and add them to the coroutine info for struct generation
            const auto &tryCatchBlocks = initialRewriter.getTryCatchBlocks();
            const_cast<CoroutineInfo &>(coro).tryCatchBlocks = tryCatchBlocks;
            REWRITE_LOG() << "  DEBUG: Found " << tryCatchBlocks.size() << " try-catch blocks\n";

            // Get the coroutine statements (suspension points) and add them to the coroutine info
            const auto &coroutineStatements = initialRewriter.getCoroutineStatements();
            const_cast<CoroutineInfo &>(coro).coroutineStatements = coroutineStatements;
            REWRITE_LOG() << "  DEBUG: Found " << coroutineStatements.size() << " suspension points\n";

            // Add ranged-for variables to the local variables set so they appear in the struct
            for (const auto &rangedFor: rangedForLoops) {
                // Add the actual loop variable to the struct
                LocalVariable loopVar;
                loopVar.name = rangedFor.loopVarName;
                loopVar.type = rangedFor.loopVarType;

                // For ranged-for loop variables, we need to determine storage based on the loop variable type
                // Since there's no explicit initializer, we analyze the type directly
                // The conceptual initializer would be `*iterator`, which is typically an lvalue

                loopVar.isReference = (loopVar.type.find("&") != std::string::npos);

                loopVar.referenceType = loopVar.type;
                loopVar.isOwning = !loopVar.isReference;
                loopVar.location = rangedFor.fullRange.getBegin();
                loopVar.priority = sourceManager.getFileOffset(loopVar.location); // Use file offset as priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(loopVar);

                LocalVariable rangeVar;
                rangeVar.name = rangedFor.rangeVarName;
                std::tie(rangeVar.type, rangeVar.isOwning) =
                        getTypeForAutoRefRefVariable(rangedFor.rangeExpr, *astContext);
                rangeVar.referenceType = rangeVar.type + " &";
                rangeVar.isReference = false;
                rangeVar.location = rangedFor.fullRange.getBegin();
                rangeVar.priority = sourceManager.getFileOffset(rangeVar.location) + 1; // Slightly later priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(rangeVar);

                auto addBeginAndEndVar = [&](const std::string &beginOrEnd, const std::string &name, int priority) {
                    LocalVariable beginVar;
                    beginVar.name = name;
                    beginVar.type = "std::decay_t<decltype(std::declval<" + rangeVar.referenceType + ">()." + beginOrEnd
                                    + "())>";
                    beginVar.isOwning = true; // Iterator types are typically not references
                    beginVar.referenceType = beginVar.type + " &";
                    beginVar.isReference = false;
                    beginVar.location = rangedFor.fullRange.getBegin();
                    beginVar.priority = sourceManager.getFileOffset(beginVar.location) + priority;
                    const_cast<CoroutineInfo &>(coro).localVariables.insert(beginVar);
                };

                addBeginAndEndVar("begin", rangedFor.beginVarName, 2);
                addBeginAndEndVar("end", rangedFor.endVarName, 3);
                /*
                LocalVariable beginVar;
                beginVar.name = rangedFor.beginVarName;
                beginVar.type = "decltype(begin(std::declval<decltype(" + rangedFor.rangeExpr + ")>()))";
                beginVar.isOwning = true; // Iterator types are typically not references
                beginVar.referenceType = beginVar.type + " &";
                beginVar.isReference = false;
                beginVar.location = rangedFor.fullRange.getBegin();
                beginVar.priority = sourceManager.getFileOffset(beginVar.location) + 2; // Slightly later priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(beginVar);

                LocalVariable endVar;
                endVar.name = rangedFor.endVarName;
                endVar.type = "decltype(end(std::declval<decltype(" + rangedFor.rangeExpr + ")>()))";
                endVar.isOwning = true; // Iterator types are typically not references
                endVar.referenceType = endVar.type + " &";
                endVar.isReference = false;
                endVar.location = rangedFor.fullRange.getBegin();
                endVar.priority = sourceManager.getFileOffset(endVar.location) + 3; // Even later priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(endVar);

                REWRITE_LOG() << "    Added ranged-for variables to struct: " << rangedFor.loopVarName
                        << ", " << rangedFor.rangeVarName << ", " << rangedFor.beginVarName
                        << ", " << rangedFor.endVarName << "\n";
                        */
            }

            // Apply only ranged-for loop replacements first (this transforms ranged-for to explicit loops)
            //REWRITE_LOG() << "  DEBUG: Applying ranged-for loop transformations\n";
            //initialRewriter.applyRangedForLoopReplacements();

            // Second pass: create new rewriter with all variables (including ranged-for vars)
            // This will process the explicit loops with proper variable rewriting
            REWRITE_LOG() << "  DEBUG: Creating final rewriter with " << coro.localVariables.size() << " variables:\n";
            for (const auto &var: coro.localVariables) {
                REWRITE_LOG() << "    Variable: " << var.type << " " << var.name << "\n";
            }

            CoroutineBodyRewriter finalRewriter(coro.localVariables, rewriter, sourceManager,
                                                coro, *astContext, coro.isMemberFunction, classRecord);
            finalRewriter.TraverseStmt(const_cast<Stmt *>(bodyStmt));

            // Collect lambda rewrites from the final rewriter
            const auto &lambdas = finalRewriter.getCollectedLambdas();
            coro.lambdasInBody = lambdas;
            REWRITE_LOG() << "  DEBUG: Found " << lambdas.size() << " regular lambdas in coroutine body\n";

            // Apply all the variable rewrites in place (construct/destroy calls, get() access)
            REWRITE_LOG() << "  DEBUG: Applying all variable transformations in place\n";

            // Now wrap the transformed code with run() method braces
            REWRITE_LOG() << "  DEBUG: Wrapping transformed code with run() method\n";
            wrapBodyWithRunMethod(coro, finalRewriter);
            finalRewriter.applyReplacements();

            REWRITE_LOG() << "Completed body rewriting for: " << coro.function->getQualifiedNameAsString() << "\n";
        }
    }

    /*
std::string CoroutineRewriter::getTransformedBodyText(const Stmt *bodyStmt, CoroutineBodyRewriter &bodyRewriter) {
        // Get the compound statement body
        if (auto *compoundStmt = dyn_cast<CompoundStmt>(bodyStmt)) {
            // Get the inner part (without the outer braces)
            SourceLocation startLoc = Lexer::getLocForEndOfToken(compoundStmt->getLBracLoc(), 0, sourceManager,
                                                                 langOptions);
            SourceLocation endLoc = compoundStmt->getRBracLoc();

            SourceRange innerRange(startLoc, endLoc);
            CharSourceRange charRange = CharSourceRange::getCharRange(innerRange);
            std::string bodyText = Lexer::getSourceText(charRange, sourceManager, langOptions).str();

            // Apply transformations in memory
            std::string transformedText = applyTransformationsInMemory(bodyText, bodyRewriter);

            return transformedText;
        }
        return "";
    }

    std::string applyTransformationsInMemory(const std::string &originalText, CoroutineBodyRewriter &bodyRewriter) {
        std::string result = originalText;

        // Get all the replacements that would have been made
        const auto &declReplacements = bodyRewriter.getDeclReplacements();
        const auto &refReplacements = bodyRewriter.getRefReplacements();
        const auto &destructorInsertions = bodyRewriter.getDestructorInsertions();

        // For now, we'll do simple text-based transformations
        // In a full implementation, we would need more sophisticated source location mapping

        REWRITE_LOG() << "    DEBUG: Original text length: " << originalText.length() << "\n";
        REWRITE_LOG() << "    DEBUG: Found " << declReplacements.size() << " declaration replacements\n";
        REWRITE_LOG() << "    DEBUG: Found " << refReplacements.size() << " reference replacements\n";
        REWRITE_LOG() << "    DEBUG: Found " << destructorInsertions.size() << " destructor insertions\n";

        // Simple regex-based replacement for now
        // TODO: Implement proper source location mapping for precise replacements

        return result;
    }
    */

void CoroutineRewriter::replaceEntireBodyWithStateMachine(const CoroutineInfo &coro) {
        REWRITE_LOG() << "  DEBUG: Replacing entire body with state machine instantiation\n";

        const Stmt *originalBody = coro.function->getBody();
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(originalBody)) {
            originalBody = coroBody->getBody();
        }

        if (auto *compoundStmt = dyn_cast<CompoundStmt>(originalBody)) {
            SourceLocation lbraceLoc = compoundStmt->getLBracLoc();
            SourceLocation rbraceLoc = compoundStmt->getRBracLoc();

            REWRITE_LOG() << "  DEBUG: LBrace location: " << lbraceLoc.printToString(sourceManager) << "\n";
            REWRITE_LOG() << "  DEBUG: RBrace location: " << rbraceLoc.printToString(sourceManager) << "\n";

            // Validate basic locations
            if (lbraceLoc.isInvalid() || rbraceLoc.isInvalid()) {
                REWRITE_LOG() << "  ERROR: Invalid brace locations\n";
                return;
            }

            // Get the full range including braces
            SourceRange fullRange = compoundStmt->getSourceRange();
            REWRITE_LOG() << "  DEBUG: Full compound statement range: "
                    << fullRange.getBegin().printToString(sourceManager) << " to "
                    << fullRange.getEnd().printToString(sourceManager) << "\n";

            // Get the original text to see what we're working with
            CharSourceRange charRange = CharSourceRange::getCharRange(fullRange);
            std::string originalText = Lexer::getSourceText(charRange, sourceManager, langOptions).str();
            REWRITE_LOG() << "  DEBUG: Original compound statement text: '" << originalText << "'\n";
            REWRITE_LOG() << "  DEBUG: Original text length: " << originalText.length() << "\n";

            // Calculate file offsets for debugging
            unsigned startOffset = sourceManager.getFileOffset(fullRange.getBegin());
            unsigned endOffset = sourceManager.getFileOffset(fullRange.getEnd());
            REWRITE_LOG() << "  DEBUG: File offset range: " << startOffset << " to " << endOffset
                    << " (length: " << (endOffset - startOffset + 1) << ")\n";

            // Try a safer approach: replace the entire compound statement
            std::string stateMachineCall =
                    "{\n    _detail_coro_statemachine_impl stateMachine;\n    stateMachine.run();\n  }";

            REWRITE_LOG() << "  DEBUG: About to replace with: '" << stateMachineCall << "'\n";
            REWRITE_LOG() << "  DEBUG: Replacement length: " << stateMachineCall.length() << "\n";

            // Use the full range instead of trying to calculate inner range
            rewriter.ReplaceText(fullRange, stateMachineCall);
            /*
                try {
                rewriter.ReplaceText(fullRange, stateMachineCall);
                REWRITE_LOG() << "  DEBUG: Successfully replaced compound statement\n";
            } catch (...) {
                REWRITE_LOG() << "  ERROR: Exception during replacement\n";
            }
            */
        }
    }


void CoroutineRewriter::wrapBodyWithRunMethod(const CoroutineInfo &coro, CoroutineBodyRewriter &body_rewriter) {
        REWRITE_LOG() << "  DEBUG: Adding closing braces after coroutine body for: " << coro.function->
                getQualifiedNameAsString() << "\n";

        const Stmt *bodyStmt = coro.function->getBody();
        if (!bodyStmt) {
            REWRITE_LOG() << "  ERROR: Function has no body to wrap\n";
            return;
        }

        // Handle CoroutineBodyStmt wrapper
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            bodyStmt = coroBody->getBody();
        }

        if (auto *compoundStmt = dyn_cast<CompoundStmt>(bodyStmt)) {
            SourceLocation rbraceLoc = compoundStmt->getRBracLoc();

            REWRITE_LOG() << "  DEBUG: Found compound statement closing brace\n";
            REWRITE_LOG() << "  DEBUG: RBrace location: " << rbraceLoc.printToString(sourceManager) << "\n";

            if (rbraceLoc.isValid()) {
                // Add COROUTINE_FOOTER to the scope end replacements system
                // This ensures it gets properly ordered with destructor calls
                unsigned fileOffset = sourceManager.getFileOffset(rbraceLoc);

                ScopeEndReplacement replacement;

                // Build parameter list for COROUTINE_FOOTER
                std::string paramList;

                // For member functions, 'this' must be the first argument
                if (coro.isMemberFunction) {
                    paramList = "this";
                }

                // Add function parameters
                if (!coro.parameters.empty()) {
                    for (size_t i = 0; i < coro.parameters.size(); ++i) {
                        if (!paramList.empty()) paramList += ", ";
                        paramList += coro.parameters[i].name;
                    }
                }

                // Add CO_RETURN_FALLOFF before COROUTINE_FOOTER
                // Calculate the falloff index - it's the next available coroutine statement index
                unsigned falloffIndex = coro.coroutineStatements.empty() ? 1 :
                    std::max_element(coro.coroutineStatements.begin(), coro.coroutineStatements.end(),
                        [](const CoroutineStatement& a, const CoroutineStatement& b) {
                            return a.index < b.index;
                        })->index + 1;

                ScopeEndReplacement falloffReplacement;
                falloffReplacement.replacement = "CO_RETURN_FALLOFF(" + std::to_string(falloffIndex) + ");\n";
                falloffReplacement.priority = std::numeric_limits<int>::max() - 1; // Just before COROUTINE_FOOTER
                body_rewriter.scopeEndReplacements[fileOffset].push_back(falloffReplacement);

                REWRITE_LOG() << "  DEBUG: Added CO_RETURN_FALLOFF(" << falloffIndex << ") with priority "
                        << falloffReplacement.priority << "\n";

                // Choose the appropriate footer macro based on whether we have try-catch blocks
                std::string footerMacro = coro.tryCatchBlocks.empty() ? "COROUTINE_FOOTER" : "COROUTINE_FOOTER_WITH_TRY";

                if (!paramList.empty()) {
                    replacement.replacement = footerMacro + "(" + paramList + ")\n";
                } else {
                    replacement.replacement = footerMacro + "()\n";
                }

                // Use maximum priority to ensure COROUTINE_FOOTER comes after all destructors and CO_RETURN_FALLOFF
                replacement.priority = std::numeric_limits<int>::max();

                body_rewriter.scopeEndReplacements[fileOffset].push_back(replacement);

                REWRITE_LOG() << "  DEBUG: Added " << footerMacro << " to scope end replacements with maximum priority ("
                        << replacement.priority << ") at file offset " << fileOffset << "\n";
                REWRITE_LOG() << "  DEBUG: " << footerMacro << " replacement text: '" << replacement.replacement << "'\n";

                REWRITE_LOG() << "  DEBUG: Successfully added " << footerMacro << " to scope end system\n";
            } else {
                REWRITE_LOG() << "  ERROR: Invalid closing brace location for compound statement\n";
            }
        } else {
            REWRITE_LOG() << "  ERROR: Body is not a compound statement, cannot add closing braces\n";
        }
    }

