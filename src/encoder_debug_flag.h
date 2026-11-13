// Consistent interpretation of the encoder diagnostic switch.
// An unset, empty or explicitly zero value disables diagnostic output.
#pragma once

#include <cstdlib>
#include <cstring>

inline bool encoder_debug_enabled() noexcept
{
    const char* value = std::getenv("LIBVA_V4L2_ENC_DEBUG");
    return value && *value && std::strcmp(value, "0") != 0;
}
