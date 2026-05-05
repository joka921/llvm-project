//
// Code generation helpers for the coroutine macro infrastructure
//

#ifndef LLVM_REWRITE_GENERATORS_MACROCODEGENERATOR_H
#define LLVM_REWRITE_GENERATORS_MACROCODEGENERATOR_H

#include <string>
#include <vector>

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
    return "this->" + memberName + ".construct(" + initializer + ")";
}

// Helper function to generate state destroy() call
inline std::string makeStateDestroyCall(const std::string &memberName) {
    return "this->" + memberName + ".destroy()";
}

// Destroy and clear the constructed flag (for normal flow, known to be constructed)
inline std::string makeStateDestroyAndClearFlag(const std::string &memberName) {
    return "DESTROY_UNCONDITIONALLY("+ memberName + ")";
}

// Destroy only if constructed, then clear flag (for exception paths)
inline std::string makeStateDestroyIfConstructed(const std::string &memberName) {
    return "DESTROY_IF_CONSTRUCTED("+ memberName + ");";
}

// Emit one or more DESTROY_UNCONDITIONALLY(...) calls for a list of members.
// Splits into groups of <=10 to stay within the macro's variadic limit.
inline std::string makeDestroyUnconditionally(const std::vector<std::string> &names) {
    if (names.empty()) return "";
    std::string result;
    for (size_t i = 0; i < names.size(); i += 10) {
        size_t end = std::min(i + 10, names.size());
        result += "DESTROY_UNCONDITIONALLY(";
        for (size_t j = i; j < end; ++j) {
            if (j > i) result += ", ";
            result += names[j];
        }
        result += ");";
        if (end < names.size()) result += "\n";
    }
    return result;
}

// Emit one or more DESTROY_IF_CONSTRUCTED(...) calls for a list of members.
// Splits into groups of <=10 to stay within the macro's variadic limit.
inline std::string makeDestroyIfConstructed(const std::vector<std::string> &names) {
    if (names.empty()) return "";
    std::string result;
    for (size_t i = 0; i < names.size(); i += 10) {
        size_t end = std::min(i + 10, names.size());
        result += "DESTROY_IF_CONSTRUCTED(";
        for (size_t j = i; j < end; ++j) {
            if (j > i) result += ", ";
            result += names[j];
        }
        result += ");";
        if (end < names.size()) result += "\n";
    }
    return result;
}

// Helper function to generate CO_BRACED_INIT prefix
inline std::string makeBracedInitPrefix(const std::string &varName, bool isOwning) {
    if (isOwning) {
        return "CO_INIT(" + varName + ", { ";
    } else {
        return "CO_BRACED_INIT(" + varName + ", ";
    }
}

// Helper function to generate CO_PAREN_INIT prefix
inline std::string makeParenInitPrefix(const std::string &varName, bool isOwning) {
    if (isOwning) {
        return "CO_INIT(" + varName + ",(";
    } else {
        return "CO_PAREN_INIT(" + varName + ", ";
    }
}

#endif // LLVM_REWRITE_GENERATORS_MACROCODEGENERATOR_H
