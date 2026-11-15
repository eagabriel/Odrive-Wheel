// Compile with the firmware toolchain (C++17), including the firmware math flags.
#include "AxisSafety.h"
#include <limits>

constexpr float invalid_nan = std::numeric_limits<float>::quiet_NaN();
constexpr float inf = std::numeric_limits<float>::infinity();
static_assert(sanitize_axis_range(900.0f) == 900.0f);
static_assert(sanitize_axis_range(1080.0f) == 1080.0f);
static_assert(sanitize_axis_range(0.0f) == 900.0f);
static_assert(sanitize_axis_range(-1.0f) == 900.0f);
static_assert(sanitize_axis_range(invalid_nan) == 900.0f);
static_assert(sanitize_axis_range(inf) == 900.0f);
static_assert(sanitize_axis_range(0.5f) == 1.0f);
static_assert(sanitize_axis_range(70000.0f) == 65534.0f);
static_assert(sanitize_axis_torque(18.0f) == 18.0f);
static_assert(sanitize_axis_torque(0.0f) == 0.1f);
static_assert(sanitize_axis_torque(655.34f) == 40.0f);
static_assert(sanitize_axis_torque(invalid_nan) == 0.1f);
static_assert(sanitize_axis_torque(inf) == 0.1f);
static_assert(sanitize_axis_fxratio(0.8f) == 0.8f);
static_assert(sanitize_axis_fxratio(0.0f) == 0.0f);
static_assert(sanitize_axis_fxratio(1.0f) == 1.0f);
static_assert(sanitize_axis_fxratio(65.534f) == 1.0f);
static_assert(sanitize_axis_fxratio(-1.0f) == 0.0f);
static_assert(sanitize_axis_fxratio(invalid_nan) == 0.0f);
static_assert(sanitize_axis_fxratio(inf) == 0.0f);
static_assert(sanitize_input_torque(18.0f) == 18.0f);
static_assert(sanitize_input_torque(-18.0f) == -18.0f);
static_assert(sanitize_input_torque(invalid_nan) == 0.0f);
static_assert(sanitize_input_torque(inf) == 0.0f);
static_assert(sanitize_input_torque(-inf) == 0.0f);
