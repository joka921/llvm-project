//
// Code generation helpers for the coroutine macro infrastructure
//

#ifndef LLVM_REWRITE_GENERATORS_MACROCODEGENERATOR_H
#define LLVM_REWRITE_GENERATORS_MACROCODEGENERATOR_H

#include <string>

// Helper function to generate state get() call
inline std::string makeStateGetCall(const std::string &varName) {
    return "CO_GET(" + varName + ")";
}

// Helper function to generate state get() call
inline std::string makeGetCallInsideState(const std::string &varName) {
    return "CO_GET_STATE(" + varName + ")";
}

// Helper function to generate state construct() call
inline std::string makeStateConstructCall(const std::string &memberName, const std::string &initializer) {
    return "this->state." + memberName + ".construct(" + initializer + ")";
}

// Helper function to generate state destroy() call
inline std::string makeStateDestroyCall(const std::string &memberName) {
    return "this->state." + memberName + ".destroy()";
}

// Helper function to generate CO_BRACED_INIT prefix
inline std::string makeBracedInitPrefix(const std::string &varName, bool isOwning) {
    if (isOwning) {
        return "CO_BRACED_INIT_OWNING(" + varName + ", ";
    } else {
        return "CO_BRACED_INIT(" + varName + ", ";
    }
}

// Helper function to generate CO_PAREN_INIT prefix
inline std::string makeParenInitPrefix(const std::string &varName, bool isOwning) {
    if (isOwning) {
        return "CO_PAREN_INIT_OWNING(" + varName + ", ";
    } else {
        return "CO_PAREN_INIT(" + varName + ", ";
    }
}

#endif // LLVM_REWRITE_GENERATORS_MACROCODEGENERATOR_H
