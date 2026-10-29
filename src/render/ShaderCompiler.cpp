#include <render/ShaderCompiler.h>

#include <shaderc/shaderc.h>

#include <cstring>
#include <mutex>


bool compileFragmentShader(const std::string& source, const std::string& name, std::vector<uint32_t>& spirv, std::string& error)
{
    // One compiler for the application, used by one thread at a time.
    static std::mutex mutex;
    static shaderc_compiler_t compiler = shaderc_compiler_initialize();

    std::scoped_lock lock(mutex);

    if (!compiler) {
        error = "The shader compiler could not start.";
        return false;
    }

    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);

    shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, source.data(), source.size(),
        shaderc_fragment_shader, name.c_str(), "main", options);

    const bool compiled = shaderc_result_get_compilation_status(result) == shaderc_compilation_status_success;

    if (compiled) {
        const size_t size = shaderc_result_get_length(result);
        spirv.resize(size / sizeof(uint32_t));
        std::memcpy(spirv.data(), shaderc_result_get_bytes(result), size);
    }
    else {
        error = shaderc_result_get_error_message(result);
    }

    shaderc_result_release(result);
    shaderc_compile_options_release(options);

    return compiled;
}
