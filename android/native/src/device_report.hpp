// device_report.hpp — see device_report.cpp.
#pragma once

#include <cstdint>
#include <string>

namespace bo2device {

struct Gpu {
    uint32_t vendor_id = 0;
    uint32_t api_version = 0;
    std::string name, driver;
};

// One line per Vulkan physical device; the first is described in *first when given.
std::string gpu_report(Gpu* first = nullptr);

// What a GPU vendor id means for a Galaxy S26.
const char* s26_variant(uint32_t vendor_id);

}  // namespace bo2device
