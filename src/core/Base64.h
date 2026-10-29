#pragma once

#include <cstdint>
#include <string>
#include <vector>


namespace core
{
    // Binary data in the JSON project files (plugin states).
    std::string base64Encode(const std::vector<uint8_t>& data);

    // Invalid characters are skipped.
    std::vector<uint8_t> base64Decode(const std::string& text);
}
