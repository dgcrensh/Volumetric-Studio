#pragma once

// Private: the process's one GPU device, shared by sampling and meshing.

#include <string>

#include "device.hpp"

namespace vs::gpu {

// The device, opened on first use; null (with `why`) when there is none.
Device* shared_device(std::string* why);

}  // namespace vs::gpu
