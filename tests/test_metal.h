#pragma once

// Plays the role of a Metal app for tests: a command queue on the device the runtime names
// (r_metal_default_device), made through the Objective-C runtime so that the tests stay C++.
// Without Metal in the build, create fails and the tests skip.

#include "render.h"

// The two functions of the Objective-C runtime the tests call, declared as objc/message.h does:
// that header also declares a global `id`, which every test local of that name would shadow
#if PUPPETXR_HAVE_METAL
struct objc_selector;
extern "C" void           objc_msgSend     (void);
extern "C" objc_selector* sel_registerName (const char* name);

template <typename R>
inline R test_objc_send (void* object, const char* selector)
{
    return ((R (*)(void*, objc_selector*))objc_msgSend)(object, sel_registerName(selector));
}
#endif

struct TestMetalApp
{
    void* device = {}; // id<MTLDevice>, not owned
    void* queue  = {}; // id<MTLCommandQueue>

    b8 create ()
    {
#if PUPPETXR_HAVE_METAL
        device = r_metal_default_device();
        if (!device)
        {
            return false;
        }
        queue = test_objc_send<void*>(device, "newCommandQueue");
        return queue != nullptr;
#else
        return false;
#endif
    }

    void destroy ()
    {
#if PUPPETXR_HAVE_METAL
        if (queue)
        {
            test_objc_send<void>(queue, "release");
        }
#endif
        queue  = nullptr;
        device = nullptr;
    }

    R_DeviceCreateInfo info () const
    {
        return { .backend = R_Backend::Metal, .metal = { .queue = queue } };
    }
};
