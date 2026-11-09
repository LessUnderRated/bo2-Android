// device_report.cpp — which GPU and driver this phone has, from Vulkan.
//
// The Galaxy S26 ships as a Snapdragon 8 Elite Gen 5 (Adreno) model and an Exynos 2600 (Xclipse,
// AMD RDNA-based) model. They need different handling (Turnip through libadrenotools applies only to
// Adreno; Xclipse runs Samsung's AMD-derived driver), so the shell reports the GPU on the setup
// screen and in every log, instead of anyone having to guess from the model number.
#include "device_report.hpp"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <string>
#include <vector>

namespace bo2device {

namespace {

const char* vendor_name(uint32_t id) {
    switch (id) {
        case 0x5143: return "Qualcomm (Adreno)";
        case 0x144D: return "Samsung (Xclipse)";
        case 0x1002: return "AMD";
        case 0x13B5: return "ARM (Mali)";
        case 0x10DE: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x10005: return "Mesa (software)";
        default: return "unknown vendor";
    }
}

std::string version(uint32_t v) {
    char text[32];
    snprintf(text, sizeof text, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return text;
}

}  // namespace

std::string gpu_report(Gpu* first) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bo2-apk device report";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult made = vkCreateInstance(&info, nullptr, &instance);
    if (made != VK_SUCCESS) return "Vulkan: vkCreateInstance failed (" + std::to_string(made) + ")";

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    std::string out;
    for (uint32_t i = 0; i < count; i++) {
        // The driver's name needs Vulkan 1.2 (or VK_KHR_driver_properties); ask only a device that has it.
        VkPhysicalDeviceProperties basic;
        vkGetPhysicalDeviceProperties(devices[i], &basic);
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        if (basic.apiVersion >= VK_API_VERSION_1_2) props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        const VkPhysicalDeviceProperties& p = props.properties;
        char line[512];
        snprintf(line, sizeof line, "GPU %u: %s, %s, Vulkan %s, driver %s %s (raw %#x)\n", i, p.deviceName,
                 vendor_name(p.vendorID), version(p.apiVersion).c_str(), driver.driverName, driver.driverInfo,
                 p.driverVersion);
        out += line;
        if (i == 0 && first) {
            first->vendor_id = p.vendorID;
            first->api_version = p.apiVersion;
            first->name = p.deviceName;
            first->driver = std::string(driver.driverName) + " " + driver.driverInfo;
        }
    }
    vkDestroyInstance(instance, nullptr);
    if (out.empty()) out = "Vulkan: no physical device\n";
    return out;
}

const char* s26_variant(uint32_t vendor_id) {
    if (vendor_id == 0x5143) return "Snapdragon 8 Elite Gen 5 / Adreno model (Turnip via libadrenotools is an option)";
    if (vendor_id == 0x144D) return "Exynos 2600 / Xclipse model (stock Samsung driver only; no Turnip)";
    return "neither Adreno nor Xclipse";
}

}  // namespace bo2device
