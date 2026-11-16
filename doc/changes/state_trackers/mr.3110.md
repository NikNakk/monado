st/oxr: Enable portability enumeration and subset and VK_EXT_metal_objects for XR_KHR_vulkan_enable2 applications on macOS.

st/oxr: Use the application's Vulkan entry point during session setup for
XR_KHR_vulkan_enable2, fixing session creation for applications that use a
different Vulkan loader or link directly to a driver, such as Godot with MoltenVK.
