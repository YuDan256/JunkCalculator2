#ifndef JC2_EXTENSION_BRIDGE_H
#define JC2_EXTENSION_BRIDGE_H

#include "jc2_extension_api.h"
#include "../memory/Value.h"
#include <unordered_map>
#include <string>
#include <set>
#include <functional>

namespace jc {

extern thread_local std::vector<Value> nativeTempRefs;

struct ModuleLoadContext {
    std::unordered_map<std::string, Value>* env;
    std::unordered_map<std::string, std::function<Value(const std::vector<Value>&)>>* builtins;
    std::unordered_map<std::string, std::set<int>>* arity;
};

const JC2_HostAPI* get_host_api();

}

#endif
