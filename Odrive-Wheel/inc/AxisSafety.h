#pragma once

#include <cmath>

// Runtime and EEPROM loads share the same bounds; no persistence format change.
inline constexpr float sanitize_axis_range(float value) {
    if (!std::isfinite(value) || value <= 0.0f) return 900.0f;
    return value < 1.0f ? 1.0f : value > 65534.0f ? 65534.0f : value;
}

inline constexpr float sanitize_axis_torque(float value) {
    if (!std::isfinite(value)) return 0.1f;
    return value < 0.1f ? 0.1f : value > 40.0f ? 40.0f : value;
}

inline constexpr float sanitize_axis_fxratio(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

inline constexpr float sanitize_input_torque(float value) {
    return std::isfinite(value) ? value : 0.0f;
}
