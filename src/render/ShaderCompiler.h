#pragma once

#include <cstdint>
#include <string>
#include <vector>


// GLSL to SPIR-V at run time (shaderc), for the shaders of the user. False
// with the messages of the compiler in error when it fails.
bool compileFragmentShader(const std::string& source, const std::string& name, std::vector<uint32_t>& spirv, std::string& error);
