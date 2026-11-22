#pragma once

// Private: the process's one GPU device, shared by sampling and meshing.

#include <memory>
#include <string>

#include "device.hpp"

namespace vs::gpu {

// The device, opened on first use; null (with `why`) when there is none or
// after shutdown(). Hold the pointer for the length of a call: shutdown()
// drops only the process's reference, so the device closes when the last
// call using it returns.
std::shared_ptr<Device> shared_device(std::string* why);

// The calibration's rule for one stage: the GPU takes it only when its
// runs all succeeded (a finite time) and it was faster.
bool gpu_wins(double gpu_ms, double cpu_ms);

}  // namespace vs::gpu
