#pragma once
#include <objc/runtime.h>

// Install on this class only: an inherited Method belongs to its superclass and
// must never be changed in place. The caller installs once per class on main.
static inline IMP SGRPlayerHookMethod(Class cls, SEL selector, IMP replacement) {
    if (!cls || !selector || !replacement) return (IMP)0;
    Method method = class_getInstanceMethod(cls, selector);
    if (!method) return (IMP)0;
    IMP original = method_getImplementation(method);
    if (!class_addMethod(cls, selector, replacement, method_getTypeEncoding(method)))
        original = method_setImplementation(method, replacement);
    return original;
}
