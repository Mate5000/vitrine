#include "Redesigned/Player/PlayerMethodHook.h"
#include <assert.h>
#include <stdio.h>

static id original(id receiver, SEL selector, ...) { (void)selector; return receiver; }
static id replacement(id receiver, SEL selector, ...) { (void)selector; return receiver; }
static id childReplacement(id receiver, SEL selector, ...) { (void)selector; return receiver; }

static Class newClass(Class parent, const char *name) {
    Class cls = objc_allocateClassPair(parent, name, 0);
    assert(cls);
    objc_registerClassPair(cls);
    return cls;
}

int main(void) {
    SEL selector = sel_registerName("testMethod");
    Class parent = newClass(Nil, "SGHookTestParent");
    assert(class_addMethod(parent, selector, (IMP)original, "@@:"));
    Class child = newClass(parent, "SGHookTestChild");
    Class sibling = newClass(parent, "SGHookTestSibling");

    // Hooking an inherited method must leave the parent and sibling untouched.
    assert(SGRPlayerHookMethod(child, selector, (IMP)replacement) == (IMP)original);
    assert(class_getMethodImplementation(child, selector) == (IMP)replacement);
    assert(class_getMethodImplementation(parent, selector) == (IMP)original);
    assert(class_getMethodImplementation(sibling, selector) == (IMP)original);

    // A class-local method returns its real previous implementation.
    assert(SGRPlayerHookMethod(child, selector, (IMP)childReplacement) == (IMP)replacement);
    assert(class_getMethodImplementation(child, selector) == (IMP)childReplacement);

    // Children created after a parent hook preserve the inherited hook as original.
    assert(SGRPlayerHookMethod(parent, selector, (IMP)replacement) == (IMP)original);
    Class laterChild = newClass(parent, "SGHookTestLaterChild");
    assert(SGRPlayerHookMethod(laterChild, selector, (IMP)childReplacement) == (IMP)replacement);
    assert(class_getMethodImplementation(parent, selector) == (IMP)replacement);
    assert(class_getMethodImplementation(sibling, selector) == (IMP)replacement);
    assert(class_getMethodImplementation(child, selector) == (IMP)childReplacement);

    // Missing methods and invalid inputs must not install anything.
    SEL missing = sel_registerName("missingMethod");
    assert(!SGRPlayerHookMethod(child, missing, (IMP)replacement));
    assert(!class_getInstanceMethod(child, missing));
    assert(!SGRPlayerHookMethod(Nil, selector, (IMP)replacement));
    assert(!SGRPlayerHookMethod(child, (SEL)0, (IMP)replacement));
    assert(!SGRPlayerHookMethod(child, selector, (IMP)0));
    puts("PASS: player hook preserves inherited/class-local methods without Substrate");
    return 0;
}
