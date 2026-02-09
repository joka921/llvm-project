//
// LambdaRewriter - Implementation
//

#include "LambdaRewriter.h"
#include "infrastructure/ASTHelpers.h"
#include "Common.h"
#include "clang/Lex/Lexer.h"

#include <sstream>

LambdaRewriter::LambdaRewriter(const SourceManager &SM, ASTContext &ctx)
    : sourceManager(SM), astContext(ctx) {}

LambdaRewriteResult LambdaRewriter::rewriteLambda(const LambdaExpr *lambda) {
    std::string className = generateClassName(lambda);
    std::vector<CaptureInfo> captures = analyzeCaptures(lambda);

    LambdaRewriteResult result;
    result.className = className;
    result.classDefinition = generateClassDefinition(lambda, className, captures);
    result.constructorCall = generateConstructorCall(className, captures);
    return result;
}

std::string LambdaRewriter::generateClassName(const LambdaExpr *lambda) {
    SourceLocation loc = lambda->getBeginLoc();
    unsigned line = sourceManager.getSpellingLineNumber(loc);
    unsigned col = sourceManager.getSpellingColumnNumber(loc);
    return "__lambda_L" + std::to_string(line) + "_C" + std::to_string(col);
}

std::vector<CaptureInfo> LambdaRewriter::analyzeCaptures(const LambdaExpr *lambda) {
    std::vector<CaptureInfo> captures;

    // Iterate captures and their init expressions in parallel
    auto initIt = lambda->capture_init_begin();
    for (const auto &capture : lambda->captures()) {
        const Expr *initExpr = *initIt;
        ++initIt;

        CaptureInfo info;
        info.isThisCapture = false;
        info.isStarThisCapture = false;
        info.isReference = false;

        switch (capture.getCaptureKind()) {
        case LCK_This: {
            info.isThisCapture = true;
            info.memberName = "__this";
            // Get the enclosing class type from the lambda's parent context
            const CXXRecordDecl *parent = dyn_cast<CXXRecordDecl>(
                lambda->getCallOperator()->getParent()->getParent());
            if (parent) {
                QualType thisType = astContext.getPointerType(astContext.getRecordType(parent));
                info.memberType = typeAsString(thisType, astContext);
            } else {
                info.memberType = "auto *";
            }
            info.initExpression = "this";
            break;
        }
        case LCK_StarThis: {
            info.isStarThisCapture = true;
            info.memberName = "__this";
            const CXXRecordDecl *parent = dyn_cast<CXXRecordDecl>(
                lambda->getCallOperator()->getParent()->getParent());
            if (parent) {
                QualType classType = astContext.getRecordType(parent);
                info.memberType = typeAsString(classType, astContext);
            } else {
                info.memberType = "auto";
            }
            info.initExpression = "*this";
            break;
        }
        case LCK_ByCopy: {
            const ValueDecl *valDecl = capture.getCapturedVar();
            info.memberName = valDecl->getNameAsString();

            if (lambda->isInitCapture(&capture)) {
                // Init capture: [z = expr]
                info.memberType = typeAsString(valDecl->getType(), astContext);
                if (initExpr) {
                    info.initExpression = getSourceText(initExpr, sourceManager);
                } else {
                    info.initExpression = info.memberName;
                }
            } else {
                // Simple by-value: [x]
                QualType type = valDecl->getType().getNonReferenceType();
                info.memberType = typeAsString(type, astContext);
                info.initExpression = valDecl->getNameAsString();
            }
            info.isReference = false;
            break;
        }
        case LCK_ByRef: {
            const ValueDecl *valDecl = capture.getCapturedVar();
            info.memberName = valDecl->getNameAsString();
            info.isReference = true;

            if (lambda->isInitCapture(&capture)) {
                // Init capture by ref: [&z = expr]
                info.memberType = typeAsString(valDecl->getType(), astContext);
                if (!valDecl->getType()->isReferenceType()) {
                    info.memberType += " &";
                }
                if (initExpr) {
                    info.initExpression = getSourceText(initExpr, sourceManager);
                } else {
                    info.initExpression = info.memberName;
                }
            } else {
                // Simple by-ref: [&x]
                info.memberType = typeAsString(valDecl->getType(), astContext) + " &";
                info.initExpression = valDecl->getNameAsString();
            }
            break;
        }
        default:
            REWRITE_LOG() << "Warning: unknown capture kind in lambda\n";
            continue;
        }

        captures.push_back(info);
    }

    return captures;
}

std::string LambdaRewriter::getOperatorSignature(const LambdaExpr *lambda) {
    // Strategy: extract the text between ']' and '{' from source.
    // This preserves auto params, default args, mutable, trailing return types, etc.
    SourceLocation introEnd = lambda->getIntroducerRange().getEnd();
    SourceLocation bodyBegin = lambda->getBody()->getBeginLoc();

    CharSourceRange charRange = CharSourceRange::getCharRange(
        Lexer::getLocForEndOfToken(introEnd, 0, sourceManager, astContext.getLangOpts()),
        bodyBegin);
    std::string sigText = Lexer::getSourceText(charRange, sourceManager, astContext.getLangOpts()).str();

    // Trim trailing whitespace
    while (!sigText.empty() && (sigText.back() == ' ' || sigText.back() == '\t' ||
           sigText.back() == '\n' || sigText.back() == '\r')) {
        sigText.pop_back();
    }

    return sigText;
}

std::string LambdaRewriter::getBodyText(const LambdaExpr *lambda) {
    const Stmt *body = lambda->getBody();
    SourceRange bodyRange = body->getSourceRange();
    CharSourceRange charRange = CharSourceRange::getTokenRange(bodyRange);
    return Lexer::getSourceText(charRange, sourceManager, astContext.getLangOpts()).str();
}

std::string LambdaRewriter::generateClassDefinition(
    const LambdaExpr *lambda,
    const std::string &className,
    const std::vector<CaptureInfo> &captures) {

    std::ostringstream out;
    out << "struct " << className << " {\n";

    // Member declarations
    for (const auto &cap : captures) {
        out << "    " << cap.memberType << " " << cap.memberName << ";\n";
    }

    if (!captures.empty()) {
        out << "\n";
    }

    // operator() signature
    std::string sig = getOperatorSignature(lambda);
    std::string body = getBodyText(lambda);

    // Build the operator() declaration
    // The signature text already contains params, mutable, trailing return type etc.
    // We need to determine the return type and const qualification
    out << "    auto operator()";
    out << sig << " ";
    out << body << "\n";

    out << "}";

    return out.str();
}

std::string LambdaRewriter::generateConstructorCall(
    const std::string &className,
    const std::vector<CaptureInfo> &captures) {

    std::ostringstream out;
    out << className << "{";

    for (size_t i = 0; i < captures.size(); ++i) {
        if (i > 0) out << ", ";
        out << captures[i].initExpression;
    }

    out << "}";
    return out.str();
}
