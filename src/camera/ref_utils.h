#pragma once

#include <reframework/API.hpp>
#include <span>
#include <vector>

namespace RE3HT {

// Shared empty args vector for REFramework method invocations
inline const std::vector<void*>& EmptyArgs() {
    static const std::vector<void*> args;
    return args;
}

// Invoke a REFramework method with no arguments, returning the pointer result
inline void* InvokePtr(reframework::API::Method* method, void* obj) {
    auto ret = method->invoke(reinterpret_cast<reframework::API::ManagedObject*>(obj), EmptyArgs());
    return ret.ptr;
}

// Invoke a no-argument method returning a managed bool (byte-sized result)
inline bool InvokeBool(reframework::API::Method* method, void* obj) {
    auto ret = method->invoke(reinterpret_cast<reframework::API::ManagedObject*>(obj), EmptyArgs());
    return ret.byte != 0;
}

// Invoke a method taking a single managed-object argument, returning the pointer result
inline void* InvokePtrArg(reframework::API::Method* method, void* obj, void* arg) {
    void* args[1] = { arg };
    auto ret = method->invoke(reinterpret_cast<reframework::API::ManagedObject*>(obj), std::span<void*>(args));
    return ret.ptr;
}

} // namespace RE3HT
