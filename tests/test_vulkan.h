#pragma once

// Vulkan validation for tests: every fixture that creates a VkInstance enables the Khronos
// validation layer when it is installed and fails the test on any validation error.

#include "platform.h"

#include <volk.h>
#include <stdio.h>
#include <string.h>

struct TestValidation
{
    const char*                        layers[1];
    u32                                layers_count;
    const char*                        extensions[8];
    u32                                extensions_count;
    VkDebugUtilsMessengerEXT           messenger;
    VkDebugUtilsMessengerCreateInfoEXT lifetime; // chained into the instance: also reports from vkDestroyInstance, such as leaks
};

inline u32 test_validation_errors = 0;

inline VKAPI_ATTR VkBool32 VKAPI_CALL test_validation_callback (VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) // NOLINT(misc-const-correctness): PFN_vkDebugUtilsMessengerCallbackEXT
{
    (void)types;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        test_validation_errors++;
        (void)fprintf(stderr, "Vulkan validation error: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

// Adds the layer and the debug utils extension to `info` when available; `extra` are the caller's extensions
inline void test_validation_prepare (TestValidation& validation, VkInstanceCreateInfo& info, const char* const* extra, u32 extra_count)
{
    validation = {};
    test_validation_errors = 0;
    for (u32 i = 0; i < extra_count && validation.extensions_count < 5; i++)
    {
        validation.extensions[validation.extensions_count++] = extra[i];
    }

    // The loader hides portability drivers such as MoltenVK from instances that do not ask for them
    VkExtensionProperties extensions[256];
    u32                   extensions_count = 256;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensions_count, extensions);
    for (u32 i = 0; i < extensions_count; i++)
    {
        if (strcmp(extensions[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
        {
            validation.extensions[validation.extensions_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            validation.extensions[validation.extensions_count++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
            info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    }

    VkLayerProperties layers[64];
    u32               layers_count = 64;
    vkEnumerateInstanceLayerProperties(&layers_count, layers);
    for (u32 i = 0; i < layers_count; i++)
    {
        if (strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0)
        {
            validation.layers[0]                                  = "VK_LAYER_KHRONOS_validation";
            validation.layers_count                               = 1;
            validation.extensions[validation.extensions_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }
    }
    if (validation.layers_count)
    {
        validation.lifetime = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext           = info.pNext,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = test_validation_callback,
        };
        info.pNext = &validation.lifetime;
    }
    info.enabledLayerCount       = validation.layers_count;
    info.ppEnabledLayerNames     = validation.layers;
    info.enabledExtensionCount   = validation.extensions_count;
    info.ppEnabledExtensionNames = validation.extensions;
}

// Appends VK_KHR_portability_subset to a device's extensions when offered: the spec requires it then
inline u32 test_portability_device_extensions (VkPhysicalDevice physical, const char** names, u32 count)
{
    VkExtensionProperties extensions[512];
    u32                   extensions_count = 512;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensions_count, extensions);
    for (u32 i = 0; i < extensions_count; i++)
    {
        if (strcmp(extensions[i].extensionName, "VK_KHR_portability_subset") == 0)
        {
            names[count++] = "VK_KHR_portability_subset";
        }
    }
    return count;
}

// Call after volkLoadInstanceOnly on the new instance. `validation` must stay alive until the
// instance is destroyed, since the instance keeps a pointer to `lifetime` for vkDestroyInstance.
inline void test_validation_attach (TestValidation& validation, VkInstance instance)
{
    if (!validation.layers_count || !vkCreateDebugUtilsMessengerEXT)
    {
        return;
    }
    const VkDebugUtilsMessengerCreateInfoEXT info = {
        .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = test_validation_callback,
    };
    vkCreateDebugUtilsMessengerEXT(instance, &info, nullptr, &validation.messenger);
}

inline void test_validation_detach (TestValidation& validation, VkInstance instance)
{
    if (validation.messenger && vkDestroyDebugUtilsMessengerEXT)
    {
        vkDestroyDebugUtilsMessengerEXT(instance, validation.messenger, nullptr);
    }
    validation.messenger = VK_NULL_HANDLE;
}
