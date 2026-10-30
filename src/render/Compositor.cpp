#include <render/Compositor.h>
#include <render/ShaderCompiler.h>
#include <render/TextLayout.h>

#include <shader/effect.vert.h>
#include <shader/effect_blur.frag.h>
#include <shader/effect_chromakey.frag.h>
#include <shader/effect_color.frag.h>
#include <shader/effect_pixelate.frag.h>
#include <shader/effect_sharpen.frag.h>
#include <shader/effect_vignette.frag.h>
#include <shader/layer.frag.h>
#include <shader/layer.vert.h>
#include <shader/text.frag.h>
#include <shader/text.vert.h>

#include <vkutils/util.h>

#include <backends/imgui_impl_vulkan.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>


using namespace core;


// Matches the Glyph structure of shader/text_glyph.glsl.
struct Compositor::GlyphInstance
{
    float corners01[4];
    float corners23[4];
    float uv[4];
    float color[4];
    float outlineColor[4];
    float params[4];
};


namespace {

constexpr VkFormat OutputFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Most layers drawn in a frame (descriptor sets of the pool).
constexpr uint32_t MaxLayers = 64;

// Descriptor sets of a frame: the layers and their effect passes.
constexpr uint32_t MaxSets = 8 * MaxLayers;

constexpr float Pi = 3.14159265358979f;


// Push constants of layer.vert / layer.frag (128 bytes, the minimum the
// devices offer).
struct LayerParameters
{
    float corners01[4];
    float corners23[4];
    float color[4];
    float local[4];
    float picture[4];
    float box[4];
    float extra[4];
    float shape[4];
};

static_assert(sizeof(LayerParameters) <= 128);


// Push constants of the effect passes (shader/effect_common.glsl).
struct EffectParameters
{
    float a[4];
    float b[4];
    float texel[2];
    float direction[2];
    float pixelScale;
    float padding[3];
};


// Placement of a layer: layer pixels (centered, y down) to sequence pixels.
struct Placement
{
    float scale;
    float cosine;
    float sine;
    float x;
    float y;

    ImVec2 apply(float px, float py) const
    {
        const float sx = px * scale;
        const float sy = py * scale;
        return ImVec2(x + sx * cosine - sy * sine, y + sx * sine + sy * cosine);
    }
};


Placement placement(const Compositor::Layer& layer, uint32_t width, uint32_t height)
{
    const Clip& clip = *layer.clip;
    const Time t = layer.localTime;
    const float rotation = clip.transform.rotation.at(t) * Pi / 180.f;

    return {
        clip.transform.scale.at(t) * layer.scale,
        std::cos(rotation),
        std::sin(rotation),
        0.5f * width + clip.transform.x.at(t) + layer.offsetX,
        0.5f * height + clip.transform.y.at(t) + layer.offsetY
    };
}


float layerOpacity(const Compositor::Layer& layer)
{
    return std::clamp(layer.clip->transform.opacity.at(layer.localTime) * layer.opacity, 0.f, 1.f);
}


void toNdc(const ImVec2& p, uint32_t width, uint32_t height, float* out)
{
    out[0] = 2.f * p.x / (float)width - 1.f;
    out[1] = 2.f * p.y / (float)height - 1.f;
}


void setColor(float* out, const Color& color, float opacity)
{
    out[0] = color.r;
    out[1] = color.g;
    out[2] = color.b;
    out[3] = color.a * opacity;
}


// Opacity, extra scale and rise of a letter for an animation, at progress
// p in [0, 1] (0: hidden, 1: in place).
void letterAnimation(LetterAnimation animation, float p, float size, float& opacity, float& scale, float& rise)
{
    const float eased = applyEasing(Easing::EaseOut, p);

    switch (animation) {
        case LetterAnimation::Typewriter:
            opacity *= (p > 0.f) ? 1.f : 0.f;
            break;
        case LetterAnimation::Fade:
            opacity *= eased;
            break;
        case LetterAnimation::Rise:
            opacity *= eased;
            rise += (1.f - eased) * 0.5f * size;
            break;
        case LetterAnimation::Pop: {
            // Overshoots a little before settling.
            const float c = 1.70158f;
            const float x = std::clamp(p, 0.f, 1.f) - 1.f;
            const float back = 1.f + (c + 1.f) * x * x * x + c * x * x;
            opacity *= eased;
            scale *= 0.3f + 0.7f * back;
            break;
        }
        default:
            break;
    }
}

} // namespace


Compositor::Compositor(const GPU::Device& device, const GPU::PipelineCache& pipelineCache, FontLibrary& fonts, bool display)
    : _device(device)
    , _fonts(fonts)
    , _display(display)
    , _pipelineCache(pipelineCache)
{
    createPipelines(pipelineCache);

    _atlas = std::make_unique<GPU::Image>(
        "Glyph atlas",
        _device,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_FORMAT_R8_UNORM,
        FontLibrary::AtlasSize, FontLibrary::AtlasSize);

    _atlasStaging = std::make_unique<GPU::Buffer>(
        "Glyph atlas upload",
        _device,
        (VkDeviceSize)FontLibrary::AtlasSize * FontLibrary::AtlasSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    _atlasStaging->bind();
}


Compositor::~Compositor()
{
    VkDevice device = _device.device();

    if (_textureDescriptor) {
        ImGui_ImplVulkan_RemoveTexture(_textureDescriptor);
    }

    _output.reset();
    _atlas.reset();
    _atlasStaging.reset();
    _glyphBuffer.reset();

    vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
    for (const auto& [type, pipeline] : _effectPipelines) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(device, _effectPipelineLayout, nullptr);

    for (const auto& [code, pipeline] : _userPipelines) {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(device, _generatorPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, _transitionPipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _transitionSetLayout, nullptr);
    vkDestroyPipeline(device, _layerPipeline, nullptr);

    for (int mode = 1; mode < (int)BlendMode::Count; mode++) {
        vkDestroyPipeline(device, _blendPipelines[mode], nullptr);
    }
    vkDestroyPipeline(device, _textPipeline, nullptr);
    vkDestroyPipelineLayout(device, _layerPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, _textPipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _layerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _textSetLayout, nullptr);
    vkDestroySampler(device, _sampler, nullptr);
    vkDestroySampler(device, _nearestSampler, nullptr);
}


void Compositor::createPipelines(const GPU::PipelineCache& pipelineCache)
{
    VkDevice device = _device.device();

    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VK_THROW_IF_FAILED(vkCreateSampler(device, &samplerInfo, nullptr, &_sampler));

    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    VK_THROW_IF_FAILED(vkCreateSampler(device, &samplerInfo, nullptr, &_nearestSampler));

    // Layers: the picture.
    {
        const VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        VK_THROW_IF_FAILED(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &_layerSetLayout));

        const VkPushConstantRange pushConstants = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LayerParameters) };

        VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &_layerSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
        VK_THROW_IF_FAILED(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_layerPipelineLayout));

        _layerPipeline = createPipeline(pipelineCache, _layerPipelineLayout, layer_vert, layer_vert_size, layer_frag, layer_frag_size);
        _blendPipelines[0] = _layerPipeline;

        for (int mode = 1; mode < (int)BlendMode::Count; mode++) {
            _blendPipelines[mode] = createPipeline(pipelineCache, _layerPipelineLayout, layer_vert, layer_vert_size,
                layer_frag, layer_frag_size, true, mode);
        }
    }

    // Texts: the atlas and the glyph instances.
    {
        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = C_ARRAY_SIZE(bindings);
        layoutInfo.pBindings = bindings;
        VK_THROW_IF_FAILED(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &_textSetLayout));

        VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &_textSetLayout;
        VK_THROW_IF_FAILED(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_textPipelineLayout));

        _textPipeline = createPipeline(pipelineCache, _textPipelineLayout, text_vert, text_vert_size, text_frag, text_frag_size);
    }

    // Video effects: a picture in, a picture out (no blending).
    {
        const VkPushConstantRange pushConstants = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(EffectParameters) };

        VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &_layerSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
        VK_THROW_IF_FAILED(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_effectPipelineLayout));

        auto effect = [&](VideoEffectType type, const uint32_t* code, size_t size) {
            _effectPipelines[type] = createPipeline(pipelineCache, _effectPipelineLayout, effect_vert, effect_vert_size, code, size, false);
        };

        effect(VideoEffectType::ColorCorrection, effect_color_frag, effect_color_frag_size);
        effect(VideoEffectType::Blur, effect_blur_frag, effect_blur_frag_size);
        effect(VideoEffectType::Sharpen, effect_sharpen_frag, effect_sharpen_frag_size);
        effect(VideoEffectType::Vignette, effect_vignette_frag, effect_vignette_frag_size);
        effect(VideoEffectType::ChromaKey, effect_chromakey_frag, effect_chromakey_frag_size);
        effect(VideoEffectType::Pixelate, effect_pixelate_frag, effect_pixelate_frag_size);
    }

    // Shaders of the user: generators (parameters only), transitions (the
    // two pictures). Their pipelines are made when first used.
    {
        const VkPushConstantRange generatorConstants = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48 };

        VkPipelineLayoutCreateInfo generatorLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        generatorLayoutInfo.pushConstantRangeCount = 1;
        generatorLayoutInfo.pPushConstantRanges = &generatorConstants;
        VK_THROW_IF_FAILED(vkCreatePipelineLayout(device, &generatorLayoutInfo, nullptr, &_generatorPipelineLayout));

        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = C_ARRAY_SIZE(bindings);
        layoutInfo.pBindings = bindings;
        VK_THROW_IF_FAILED(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &_transitionSetLayout));

        const VkPushConstantRange transitionConstants = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 };

        VkPipelineLayoutCreateInfo transitionLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        transitionLayoutInfo.setLayoutCount = 1;
        transitionLayoutInfo.pSetLayouts = &_transitionSetLayout;
        transitionLayoutInfo.pushConstantRangeCount = 1;
        transitionLayoutInfo.pPushConstantRanges = &transitionConstants;
        VK_THROW_IF_FAILED(vkCreatePipelineLayout(device, &transitionLayoutInfo, nullptr, &_transitionPipelineLayout));
    }

    // Sets of the frame, recycled by beginFrame().
    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxSets },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MaxLayers },
    };

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = MaxSets;
    poolInfo.poolSizeCount = C_ARRAY_SIZE(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    VK_THROW_IF_FAILED(vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool));
}


VkPipeline Compositor::createPipeline(const GPU::PipelineCache& pipelineCache, VkPipelineLayout layout,
    const uint32_t* vertexCode, size_t vertexSize, const uint32_t* fragmentCode, size_t fragmentSize, bool blending, int blendMode)
{
    VkDevice device = _device.device();

    VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vertexModule = VK_NULL_HANDLE;
    VkShaderModule fragmentModule = VK_NULL_HANDLE;

    moduleInfo.codeSize = vertexSize;
    moduleInfo.pCode = vertexCode;
    VK_THROW_IF_FAILED(vkCreateShaderModule(device, &moduleInfo, nullptr, &vertexModule));

    moduleInfo.codeSize = fragmentSize;
    moduleInfo.pCode = fragmentCode;
    VK_THROW_IF_FAILED(vkCreateShaderModule(device, &moduleInfo, nullptr, &fragmentModule));

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentModule;
    stages[1].pName = "main";

    // The blend mode, as the specialization constant 0 of the layer shader.
    const VkSpecializationMapEntry blendEntry = { 0, 0, sizeof(int) };
    VkSpecializationInfo specialization = { 1, &blendEntry, sizeof(int), &blendMode };

    if (blendMode != 0) {
        stages[1].pSpecializationInfo = &specialization;
    }

    // Quads from the vertex index: no vertex input.
    VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.f;

    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Straight alpha over the layers below.
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = blending ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    // The other modes, from the premultiplied color P = c a of the shader:
    // Add d + P, Screen d + P (1 - d), Multiply d (1 - a) + P d,
    // Lighten max(d, P), Darken min(d, c a + 1 - a).
    switch ((BlendMode)blendMode) {
        case BlendMode::Add:
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::Screen:
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::Multiply:
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Lighten:
            blendAttachment.colorBlendOp = VK_BLEND_OP_MAX;
            break;
        case BlendMode::Darken:
            blendAttachment.colorBlendOp = VK_BLEND_OP_MIN;
            break;
        default:
            break;
    }

    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = C_ARRAY_SIZE(dynamicStates);
    dynamic.pDynamicStates = dynamicStates;

    // Dynamic rendering into the output.
    VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &OutputFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device, pipelineCache.pipelineCache(), 1, &pipelineInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, vertexModule, nullptr);
    vkDestroyShaderModule(device, fragmentModule, nullptr);
    VK_THROW_IF_FAILED(result);

    return pipeline;
}


void Compositor::beginFrame()
{
    vkResetDescriptorPool(_device.device(), _descriptorPool, 0);

    for (PassTarget& target : _passTargets) {
        target.used = false;
    }
}


GPU::Image* Compositor::passTarget(uint32_t width, uint32_t height)
{
    for (PassTarget& target : _passTargets) {
        if (!target.used && target.image->width() == width && target.image->height() == height) {
            target.used = true;
            return target.image.get();
        }
    }

    // Images of sizes no longer used go, beyond a few.
    if (_passTargets.size() > 16) {
        std::erase_if(_passTargets, [](const PassTarget& target) { return !target.used; });
    }

    PassTarget& target = _passTargets.emplace_back();
    target.image = std::make_unique<GPU::Image>("Effect pass", _device,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, OutputFormat, width, height);
    target.used = true;

    return target.image.get();
}


const GPU::Image* Compositor::cmdApplyEffects(VkCommandBuffer commandBuffer, const Layer& layer, uint32_t layoutWidth, uint32_t layoutHeight)
{
    // The effects of the clip, then those of its track; the ones on.
    // Their keyframes: in the time of the clip, or of the sequence for a
    // track.
    std::vector<std::pair<const VideoEffect*, Time>> effects;

    for (const VideoEffect& effect : layer.clip->videoEffects) {
        if (effect.enabled && effect.info()) effects.emplace_back(&effect, layer.localTime);
    }

    if (layer.trackEffects) {
        for (const VideoEffect& effect : *layer.trackEffects) {
            if (effect.enabled && effect.info()) effects.emplace_back(&effect, layer.clip->start + layer.localTime);
        }
    }

    const GPU::Image* source = layer.image;

    if (effects.empty()) {
        return source;
    }

    // Distances in pixels of the sequence, before the transform: in pixels
    // of the picture through its fit in the frame.
    const float fit = fitScale(layer.clip->fit, (float)layoutWidth, (float)layoutHeight, (float)layer.width, (float)layer.height);
    const uint32_t width = source->width();
    const uint32_t height = source->height();

    for (const auto& [effect, time] : effects) {
        const VideoEffectInfo& info = *effect->info();
        EffectParameters parameters = {};

        for (size_t i = 0; i < effect->parameters.size() && i < 8; i++) {
            (i < 4 ? parameters.a[i] : parameters.b[i - 4]) = effect->parameters[i].at(time);
        }

        parameters.texel[0] = 1.f / (float)width;
        parameters.texel[1] = 1.f / (float)height;
        parameters.pixelScale = (float)layer.width / std::max(1.f, (float)layer.width * fit) * ((float)width / std::max(1u, layer.width));

        // Nothing to do.
        if (info.type == VideoEffectType::Blur && parameters.a[0] * parameters.pixelScale < 0.5f) {
            continue;
        }

        // A blur in two passes: across, then down.
        const int passes = (info.type == VideoEffectType::Blur) ? 2 : 1;

        for (int pass = 0; pass < passes; pass++) {
            const VkDescriptorSet set = allocateSet(_layerSetLayout);

            if (!set) {
                return source;
            }

            GPU::Image* target = passTarget(width, height);

            const VkDescriptorImageInfo imageInfo = { _sampler, source->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(_device.device(), 1, &write, 0, nullptr);

            target->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            attachment.imageView = target->imageView();
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
            renderingInfo.renderArea = { { 0, 0 }, { width, height } };
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &attachment;

            vkCmdBeginRendering(commandBuffer, &renderingInfo);

            const VkViewport viewport = { 0.f, 0.f, (float)width, (float)height, 0.f, 1.f };
            const VkRect2D scissor = { { 0, 0 }, { width, height } };
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            parameters.direction[0] = (pass == 0) ? 1.f : 0.f;
            parameters.direction[1] = (pass == 0) ? 0.f : 1.f;

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _effectPipelines.at(info.type));
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _effectPipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(commandBuffer, _effectPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), &parameters);
            vkCmdDraw(commandBuffer, 4, 1, 0, 0);

            vkCmdEndRendering(commandBuffer);

            target->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            source = target;
        }
    }

    return source;
}


VkDescriptorSet Compositor::allocateSet(VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocateInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocateInfo.descriptorPool = _descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;

    // Out of sets: the layer is skipped, see MaxLayers.
    if (vkAllocateDescriptorSets(_device.device(), &allocateInfo, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return set;
}


void Compositor::ensureOutput(uint32_t width, uint32_t height)
{
    if (_output && _output->width() == width && _output->height() == height) {
        return;
    }

    // The previous frame is done with the image, see beginFrame().
    if (_textureDescriptor) {
        ImGui_ImplVulkan_RemoveTexture(_textureDescriptor);
        _textureDescriptor = VK_NULL_HANDLE;
    }

    _output = std::make_unique<GPU::Image>(
        "Composited frame",
        _device,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        OutputFormat,
        width, height);

    if (_display) {
        _textureDescriptor = ImGui_ImplVulkan_AddTexture(_sampler, _output->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}


void Compositor::cmdUploadAtlas(VkCommandBuffer commandBuffer)
{
    // The previous upload is done, see beginFrame().
    std::memcpy(_atlasStaging->mappedMemory(), _fonts.atlas().data(), _fonts.atlas().size());
    _fonts.markUploaded();

    _atlas->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { FontLibrary::AtlasSize, FontLibrary::AtlasSize, 1 };

    vkCmdCopyBufferToImage(commandBuffer, _atlasStaging->buffer(), _atlas->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    _atlas->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}


uint32_t Compositor::buildText(const Layer& layer, uint32_t width, uint32_t height, std::vector<GlyphInstance>& instances, LayerBounds& bounds)
{
    const Clip& clip = *layer.clip;
    const TextContent& text = clip.text;
    const Time t = layer.localTime;

    const TextLayout layout = layoutText(_fonts, text);
    const Placement place = placement(layer, width, height);
    const float opacityOfLayer = layerOpacity(layer);

    const float hw = 0.5f * layout.width;
    const float hh = 0.5f * layout.height;
    bounds.corners = { place.apply(-hw, -hh), place.apply(hw, -hh), place.apply(hw, hh), place.apply(-hw, hh) };

    if (opacityOfLayer <= 0.f) {
        return 0;
    }

    const size_t first = instances.size();
    const int letters = (int)layout.letterCount;
    const TextAnimation& animation = text.animation;
    const double letterDuration = (double)std::max<Time>(1, animation.letterDuration);

    // The movement of the whole text: from out of the frame on one side to
    // out of it on the other, over the clip, in pixels of the sequence.
    const float pad = text.background.enabled ? text.background.padding : 0.f;
    const ImVec2 center = place.apply(0.f, 0.f);
    const float progress = (float)std::clamp((double)t / (double)std::max<Time>(1, clip.duration), 0.0, 1.0);
    ImVec2 motion(0.f, 0.f);

    if (text.motion == TextMotion::Roll) {
        const float half = (hh + pad) * place.scale;
        motion.y = ((float)height + half) + (-half - ((float)height + half)) * progress - center.y;
    }
    else if (text.motion == TextMotion::Crawl) {
        const float half = (hw + pad) * place.scale;
        motion.x = ((float)width + half) + (-half - ((float)width + half)) * progress - center.x;
    }

    // The box behind the text, under its shadows. A full width bar stays in
    // place while the text crawls along it.
    if (text.background.enabled) {
        const float boxScale = place.scale;
        ImVec2 corners[4];

        if (text.background.fullWidth) {
            const float top = center.y + motion.y - (hh + pad) * boxScale;
            const float bottom = center.y + motion.y + (hh + pad) * boxScale;
            corners[0] = ImVec2(0.f, top);
            corners[1] = ImVec2((float)width, top);
            corners[2] = ImVec2(0.f, bottom);
            corners[3] = ImVec2((float)width, bottom);
        }
        else {
            const ImVec2 points[4] = {
                place.apply(-hw - pad, -hh - pad), place.apply(hw + pad, -hh - pad),
                place.apply(-hw - pad, hh + pad), place.apply(hw + pad, hh + pad) };

            for (int i = 0; i < 4; i++) {
                corners[i] = ImVec2(points[i].x + motion.x, points[i].y + motion.y);
            }
        }

        GlyphInstance box = {};
        toNdc(corners[0], width, height, box.corners01);
        toNdc(corners[1], width, height, box.corners01 + 2);
        toNdc(corners[2], width, height, box.corners23);
        toNdc(corners[3], width, height, box.corners23 + 2);

        box.uv[0] = 0.f;
        box.uv[1] = 0.f;
        box.uv[2] = 1.f;
        box.uv[3] = 1.f;

        setColor(box.color, text.background.color, opacityOfLayer);
        box.params[0] = text.background.fullWidth ? 0.f : text.background.cornerRadius * boxScale;
        box.params[1] = std::hypot(corners[1].x - corners[0].x, corners[1].y - corners[0].y);
        box.params[2] = std::hypot(corners[2].x - corners[0].x, corners[2].y - corners[0].y);
        box.params[3] = 2.f;

        instances.push_back(box);
    }

    // Shadows first, under every letter, then the letters.
    for (int pass = 0; pass < 2; pass++) {
        const bool shadowPass = pass == 0;

        for (const LaidOutGlyph& glyph : layout.glyphs) {
            const TextStyle& style = text.spans[glyph.span].style;

            if (shadowPass && style.shadowOffsetX == 0.f && style.shadowOffsetY == 0.f && style.shadowSoftness == 0.f) {
                continue;
            }

            // Letter animations: the letters come in one after the other
            // from the start of the clip, and leave the same way before its
            // end.
            float opacity = opacityOfLayer;
            float letterScale = 1.f;
            float rise = 0.f;
            const float size = glyph.pixelsPerBase * FontLibrary::BaseSize;

            if (animation.in != LetterAnimation::None && layer.animateLetters) {
                const double start = (double)glyph.letter * (double)animation.letterDelay;
                const float p = (float)std::clamp(((double)t - start) / letterDuration, 0.0, 1.0);
                letterAnimation(animation.in, p, size, opacity, letterScale, rise);
            }

            if (animation.out != LetterAnimation::None && layer.animateLetters) {
                const double start = (double)(letters - 1 - glyph.letter) * (double)animation.letterDelay;
                const float p = (float)std::clamp(((double)(clip.duration - t) - start) / letterDuration, 0.0, 1.0);
                letterAnimation(animation.out, p, size, opacity, letterScale, rise);
            }

            if (opacity <= 0.f || letterScale <= 0.f) {
                continue;
            }

            const float offsetX = shadowPass ? style.shadowOffsetX : 0.f;
            const float offsetY = shadowPass ? style.shadowOffsetY : 0.f;

            auto corner = [&](float x, float y) {
                const float lx = glyph.pivotX + (x - glyph.pivotX) * letterScale + offsetX;
                const float ly = glyph.pivotY + (y - glyph.pivotY) * letterScale + rise + offsetY;
                const ImVec2 p = place.apply(lx, ly);
                return ImVec2(p.x + motion.x, p.y + motion.y);
            };

            GlyphInstance instance = {};
            toNdc(corner(glyph.x0, glyph.y0), width, height, instance.corners01);
            toNdc(corner(glyph.x1, glyph.y0), width, height, instance.corners01 + 2);
            toNdc(corner(glyph.x0, glyph.y1), width, height, instance.corners23);
            toNdc(corner(glyph.x1, glyph.y1), width, height, instance.corners23 + 2);

            instance.uv[0] = glyph.u0;
            instance.uv[1] = glyph.v0;
            instance.uv[2] = glyph.u1;
            instance.uv[3] = glyph.v1;

            const float outputScale = place.scale * letterScale;

            setColor(instance.color, shadowPass ? style.shadowColor : style.color, opacity);
            setColor(instance.outlineColor, style.outlineColor, opacity);

            instance.params[0] = style.outlineWidth * outputScale;
            instance.params[1] = style.shadowSoftness * outputScale;
            instance.params[2] = glyph.pixelsPerBase * outputScale;
            instance.params[3] = shadowPass ? 1.f : 0.f;

            instances.push_back(instance);
        }
    }

    return (uint32_t)(instances.size() - first);
}


void Compositor::cmdComposite(VkCommandBuffer commandBuffer, uint32_t outputWidth, uint32_t outputHeight, const std::vector<Layer>& layers,
    uint32_t layoutWidth, uint32_t layoutHeight)
{
    ensureOutput(outputWidth, outputHeight);

    // Placements in pixels of the layout; the output covers it whole.
    const uint32_t width = layoutWidth ? layoutWidth : outputWidth;
    const uint32_t height = layoutHeight ? layoutHeight : outputHeight;

    // Texts first, nested ones included: their layout renders the missing
    // glyphs into the atlas, uploaded before drawing.
    std::vector<Layer> tree = layers;
    std::vector<GlyphInstance> instances;
    std::vector<LayerBounds> bounds(tree.size());

    collectTexts(tree, width, height, instances, &bounds);

    if (_fonts.dirty()) {
        cmdUploadAtlas(commandBuffer);
    }

    if (!instances.empty()) {
        const VkDeviceSize size = instances.size() * sizeof(GlyphInstance);

        if (!_glyphBuffer || _glyphBuffer->size() < size) {
            _glyphBuffer = std::make_unique<GPU::Buffer>(
                "Glyph instances",
                _device,
                std::max<VkDeviceSize>(size * 2, 4096 * sizeof(GlyphInstance)),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            _glyphBuffer->bind();
        }

        // The previous frame is done with the buffer, see beginFrame().
        std::memcpy(_glyphBuffer->mappedMemory(), instances.data(), (size_t)size);
    }

    cmdRenderLevel(commandBuffer, _output.get(), tree, outputWidth, outputHeight, width, height, bounds, 0);

    // Top first, for picking.
    std::reverse(bounds.begin(), bounds.end());
    _bounds = std::move(bounds);
}


uint32_t Compositor::collectTexts(std::vector<Layer>& layers, uint32_t width, uint32_t height, std::vector<GlyphInstance>& instances, std::vector<LayerBounds>* bounds)
{
    uint32_t total = 0;

    for (size_t i = 0; i < layers.size(); i++) {
        Layer& layer = layers[i];
        LayerBounds ignored;
        LayerBounds& layerBounds = bounds ? (*bounds)[i] : ignored;
        layerBounds.clipId = layer.clip->id;

        if (!layer.children.empty()) {
            total += collectTexts(layer.children, layer.childWidth, layer.childHeight, instances, nullptr);
        }
        else if (!layer.image && layer.clip->kind == ClipKind::Text) {
            layer.textFirst = (uint32_t)instances.size();
            layer.textCount = buildText(layer, width, height, instances, layerBounds);
            total += layer.textCount;
        }
    }

    return total;
}


void Compositor::cmdRenderLevel(VkCommandBuffer commandBuffer, GPU::Image* target, std::vector<Layer>& work,
    uint32_t outputWidth, uint32_t outputHeight, uint32_t width, uint32_t height, std::vector<LayerBounds>& bounds, int depth)
{
    // Nested sequences: their frames first, at their size times the scale
    // of this level.
    const float scale = (float)outputWidth / (float)std::max(1u, width);

    for (Layer& layer : work) {
        if (layer.children.empty() || !layer.childWidth || !layer.childHeight || depth > 8) {
            continue;
        }

        const uint32_t w = std::max(2u, (uint32_t)std::lround((float)layer.childWidth * scale) & ~1u);
        const uint32_t h = std::max(2u, (uint32_t)std::lround((float)layer.childHeight * scale) & ~1u);

        GPU::Image* frame = passTarget(w, h);
        std::vector<LayerBounds> ignored(layer.children.size());
        cmdRenderLevel(commandBuffer, frame, layer.children, w, h, layer.childWidth, layer.childHeight, ignored, depth + 1);

        layer.image = frame;
        layer.width = w;
        layer.height = h;
    }

    // Generators: their pictures, at the size of the output.
    for (Layer& layer : work) {
        if (layer.clip->kind == ClipKind::Generator && layer.generator) {
            if (GPU::Image* picture = cmdGenerate(commandBuffer, layer, outputWidth, outputHeight)) {
                layer.image = picture;
                layer.width = outputWidth;
                layer.height = outputHeight;
            }
        }
    }

    // The pictures through their effects.
    std::vector<const GPU::Image*> pictures(work.size(), nullptr);

    for (size_t i = 0; i < work.size(); i++) {
        if (work[i].image) {
            pictures[i] = cmdApplyEffects(commandBuffer, work[i], width, height);
        }
    }

    // Shader transitions: the outgoing and incoming layers each drawn in a
    // frame of their own, mixed by the shader into a frame drawn instead.
    std::vector<const GPU::Image*> frames(work.size(), nullptr);
    std::vector<bool> skip(work.size(), false);

    auto layerFrame = [&](int index) {
        GPU::Image* frame = passTarget(outputWidth, outputHeight);
        cmdBeginPass(commandBuffer, frame, true);

        if (index >= 0) {
            const Layer& layer = work[index];
            cmdDrawLayer(commandBuffer, layer, pictures[index], { layer.textFirst, layer.textCount }, width, height, outputWidth, outputHeight, bounds[index]);
        }

        cmdEndPass(commandBuffer, frame);
        return frame;
    };

    for (size_t i = 0; i < work.size(); i++) {
        if (work[i].transitionRole != 2 || !work[i].transitionShader) {
            continue;
        }

        const bool withOutgoing = i > 0 && work[i - 1].transitionRole == 1;
        const GPU::Image* from = layerFrame(withOutgoing ? (int)i - 1 : -1);
        const GPU::Image* to = layerFrame((int)i);

        if (const GPU::Image* mixed = cmdTransition(commandBuffer, *work[i].transitionShader, work[i].transitionProgress, from, to, outputWidth, outputHeight)) {
            frames[i] = mixed;

            if (withOutgoing) {
                skip[i - 1] = true;
            }
        }
    }

    // The level: black under the sequence shown, transparent under a nested
    // one (what is below it shows through).
    cmdBeginPass(commandBuffer, target, true, depth == 0);

    for (size_t i = 0; i < work.size(); i++) {
        if (skip[i]) {
            continue;
        }

        // A shader transition: its frame, over the whole output.
        if (frames[i]) {
            static const Clip wholeFrame;
            Layer frameLayer;
            frameLayer.clip = &wholeFrame;
            frameLayer.image = frames[i];
            frameLayer.width = outputWidth;
            frameLayer.height = outputHeight;

            LayerBounds ignored;
            cmdDrawLayer(commandBuffer, frameLayer, frames[i], { 0, 0 }, width, height, outputWidth, outputHeight, ignored);
            continue;
        }

        cmdDrawLayer(commandBuffer, work[i], pictures[i], { work[i].textFirst, work[i].textCount }, width, height, outputWidth, outputHeight, bounds[i]);
    }

    cmdEndPass(commandBuffer, target);
}


void Compositor::cmdDrawLayer(VkCommandBuffer commandBuffer, const Layer& layer, const GPU::Image* source, std::pair<uint32_t, uint32_t> textRange,
    uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight, LayerBounds& bounds)
{

    // Wipe transition: the layer only covers the left part of the frame.
    const VkRect2D layerScissor = { { 0, 0 }, { (uint32_t)std::clamp(std::lround(layer.wipe * (float)outputWidth), 0L, (long)outputWidth), outputHeight } };

    if (layerScissor.extent.width == 0) {
        return;
    }

    vkCmdSetScissor(commandBuffer, 0, 1, &layerScissor);

    if (layer.image) {
        const float opacity = layerOpacity(layer);

        // The picture fits the frame (or keeps its size), then the
        // transform applies. The crop keeps the rest of the picture in place.
        const float fit = fitScale(layer.clip->fit, (float)width, (float)height, (float)layer.width, (float)layer.height);
        const float hw = 0.5f * layer.width * fit;
        const float hh = 0.5f * layer.height * fit;
        const Placement place = placement(layer, width, height);

        const Frame& frame = layer.clip->frame;
        const float cropLeft = std::clamp(frame.cropLeft, 0.f, 0.99f);
        const float cropTop = std::clamp(frame.cropTop, 0.f, 0.99f);
        const float cropRight = std::clamp(frame.cropRight, 0.f, 0.99f - cropLeft);
        const float cropBottom = std::clamp(frame.cropBottom, 0.f, 0.99f - cropTop);

        const float picture[4] = { -hw, -hh, hw, hh };
        const float box[4] = {
            -hw + 2.f * hw * cropLeft, -hh + 2.f * hh * cropTop,
            hw - 2.f * hw * cropRight, hh - 2.f * hh * cropBottom };

        bounds.corners = { place.apply(box[0], box[1]), place.apply(box[2], box[1]), place.apply(box[2], box[3]), place.apply(box[0], box[3]) };

        if (opacity <= 0.f) {
            return;
        }

        const VkDescriptorSet set = allocateSet(_layerSetLayout);

        if (!set) {
            return;
        }

        // Sampling and blending of the clip (its track's when Normal).
        const VkSampler sampler = (layer.clip->filter == ScaleFilter::Nearest) ? _nearestSampler : _sampler;
        const BlendMode blend = (layer.clip->blend != BlendMode::Normal) ? layer.clip->blend : layer.trackBlend;
        const VkPipeline pipeline = _blendPipelines[std::min((size_t)blend, _blendPipelines.size() - 1)];

        const VkDescriptorImageInfo imageInfo = { sampler, source->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(_device.device(), 1, &write, 0, nullptr);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _layerPipelineLayout, 0, 1, &set, 0, nullptr);

        // A quad over the given rectangle of the layer, grown by margin
        // (layer pixels), the box shaped in it.
        const float pixel = 1.f / std::max(place.scale, 1e-3f);
        const float border = std::max(0.f, frame.borderWidth);

        auto draw = [&](const float* shownBox, float margin, const Color& extra, bool shadow) {
            LayerParameters parameters = {};
            const float local[4] = { shownBox[0] - margin, shownBox[1] - margin, shownBox[2] + margin, shownBox[3] + margin };

            toNdc(place.apply(local[0], local[1]), width, height, parameters.corners01);
            toNdc(place.apply(local[2], local[1]), width, height, parameters.corners01 + 2);
            toNdc(place.apply(local[0], local[3]), width, height, parameters.corners23);
            toNdc(place.apply(local[2], local[3]), width, height, parameters.corners23 + 2);

            parameters.color[0] = (layer.clip->filter == ScaleFilter::Bicubic) ? 2.f : (layer.clip->filter == ScaleFilter::Lanczos) ? 3.f : 0.f;
            parameters.color[1] = parameters.color[2] = 1.f;
            parameters.color[3] = opacity;
            std::copy(local, local + 4, parameters.local);
            std::copy(picture, picture + 4, parameters.picture);
            std::copy(shownBox, shownBox + 4, parameters.box);
            parameters.extra[0] = extra.r;
            parameters.extra[1] = extra.g;
            parameters.extra[2] = extra.b;
            parameters.extra[3] = extra.a;
            parameters.shape[0] = std::max(0.f, frame.cornerRadius);
            parameters.shape[1] = border;
            parameters.shape[2] = std::max(0.f, frame.shadowSoftness);
            parameters.shape[3] = shadow ? 1.f : 0.f;

            // The shadow stays a normal shadow, whatever the blending.
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow ? _layerPipeline : pipeline);
            vkCmdPushConstants(commandBuffer, _layerPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), &parameters);
            vkCmdDraw(commandBuffer, 4, 1, 0, 0);
        };

        // Shadow, below.
        if (frame.hasShadow()) {
            const float shadowBox[4] = {
                box[0] + frame.shadowOffsetX, box[1] + frame.shadowOffsetY,
                box[2] + frame.shadowOffsetX, box[3] + frame.shadowOffsetY };

            draw(shadowBox, border + std::max(0.f, frame.shadowSoftness) + 2.f * pixel, frame.shadowColor, true);
        }

        draw(box, border + 2.f * pixel, frame.borderColor, false);
    }
    else if (textRange.second > 0) {
        const VkDescriptorSet set = allocateSet(_textSetLayout);

        if (!set) {
            return;
        }

        const VkDescriptorImageInfo atlasInfo = { _sampler, _atlas->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorBufferInfo bufferInfo = { _glyphBuffer->buffer(), 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &atlasInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(_device.device(), 2, writes, 0, nullptr);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _textPipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _textPipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(commandBuffer, 4, textRange.second, 0, textRange.first);
    }
}


void Compositor::cmdBeginPass(VkCommandBuffer commandBuffer, GPU::Image* target, bool clear, bool opaque)
{
    target->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    attachment.imageView = target->imageView();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = { { 0.f, 0.f, 0.f, opaque ? 1.f : 0.f } };

    VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderingInfo.renderArea = { { 0, 0 }, { target->width(), target->height() } };
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &attachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);

    const VkViewport viewport = { 0.f, 0.f, (float)target->width(), (float)target->height(), 0.f, 1.f };
    const VkRect2D scissor = { { 0, 0 }, { target->width(), target->height() } };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}


void Compositor::cmdEndPass(VkCommandBuffer commandBuffer, GPU::Image* target)
{
    vkCmdEndRendering(commandBuffer);
    target->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}


namespace {

// The code of the user inside what Vulkan needs (see core/Shaders.h).
std::string wrapShader(const ShaderDef& shader)
{
    std::string code = "#version 460\n";

    if (shader.kind == ShaderKind::Generator) {
        code +=
            "layout(push_constant) uniform QuickEditParameters { vec2 resolution; float time; float duration; vec4 p0; vec4 p1; } qe;\n"
            "layout(location = 0) in vec2 qeUV;\n"
            "layout(location = 0) out vec4 qeColor;\n"
            "#define iResolution qe.resolution\n"
            "#define iTime qe.time\n"
            "#define iDuration qe.duration\n"
            "float param(int i) { return i < 4 ? qe.p0[i] : qe.p1[i - 4]; }\n"
            "#line 1\n" + shader.source + "\n"
            "void main() { qeColor = generate(qeUV, qe.time); }\n";
    }
    else {
        code +=
            "layout(set = 0, binding = 0) uniform sampler2D qeFrom;\n"
            "layout(set = 0, binding = 1) uniform sampler2D qeTo;\n"
            "layout(push_constant) uniform QuickEditParameters { float progress; float ratio; } qe;\n"
            "layout(location = 0) in vec2 qeUV;\n"
            "layout(location = 0) out vec4 qeColor;\n"
            "#define progress qe.progress\n"
            "#define ratio qe.ratio\n"
            "vec4 getFromColor(vec2 uv) { return texture(qeFrom, uv); }\n"
            "vec4 getToColor(vec2 uv) { return texture(qeTo, uv); }\n"
            "#line 1\n" + shader.source + "\n"
            "void main() { qeColor = transition(qeUV); }\n";
    }

    return code;
}

} // namespace


VkPipeline Compositor::userPipeline(const ShaderDef& shader)
{
    const std::string code = wrapShader(shader);

    if (const auto it = _userPipelines.find(code); it != _userPipelines.end()) {
        return it->second;
    }

    std::vector<uint32_t> spirv;
    std::string error;
    VkPipeline pipeline = VK_NULL_HANDLE;

    if (compileFragmentShader(code, shader.name, spirv, error)) {
        const VkPipelineLayout layout = (shader.kind == ShaderKind::Generator) ? _generatorPipelineLayout : _transitionPipelineLayout;
        pipeline = createPipeline(_pipelineCache, layout, effect_vert, effect_vert_size, spirv.data(), spirv.size() * sizeof(uint32_t), false);
        _shaderErrors.erase(shader.id);
    }
    else {
        _shaderErrors[shader.id] = error;
        SDL_Log("Shader %s: %s", shader.name.c_str(), error.c_str());
    }

    _userPipelines[code] = pipeline;

    return pipeline;
}


std::string Compositor::checkShader(const ShaderDef& shader)
{
    std::vector<uint32_t> spirv;
    std::string error;

    return compileFragmentShader(wrapShader(shader), shader.name, spirv, error) ? std::string() : error;
}


std::string Compositor::shaderError(const std::string& shaderId) const
{
    const auto it = _shaderErrors.find(shaderId);
    return (it != _shaderErrors.end()) ? it->second : std::string();
}


GPU::Image* Compositor::cmdGenerate(VkCommandBuffer commandBuffer, const Layer& layer, uint32_t width, uint32_t height)
{
    const VkPipeline pipeline = userPipeline(*layer.generator);

    if (!pipeline) {
        return nullptr;
    }

    struct
    {
        float resolution[2];
        float time;
        float duration;
        float parameters[8];
    } parameters = {};

    parameters.resolution[0] = (float)width;
    parameters.resolution[1] = (float)height;
    parameters.time = (float)timeToSeconds(layer.localTime);
    parameters.duration = (float)timeToSeconds(layer.clip->duration);

    for (size_t i = 0; i < layer.clip->shaderParameters.size() && i < 8; i++) {
        parameters.parameters[i] = layer.clip->shaderParameters[i].at(layer.localTime);
    }

    GPU::Image* target = passTarget(width, height);
    cmdBeginPass(commandBuffer, target, false);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(commandBuffer, _generatorPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), &parameters);
    vkCmdDraw(commandBuffer, 4, 1, 0, 0);

    cmdEndPass(commandBuffer, target);

    return target;
}


GPU::Image* Compositor::cmdTransition(VkCommandBuffer commandBuffer, const ShaderDef& shader, float progress,
    const GPU::Image* from, const GPU::Image* to, uint32_t width, uint32_t height)
{
    const VkPipeline pipeline = userPipeline(shader);
    const VkDescriptorSet set = pipeline ? allocateSet(_transitionSetLayout) : VK_NULL_HANDLE;

    if (!set) {
        return nullptr;
    }

    const VkDescriptorImageInfo images[2] = {
        { _sampler, from->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { _sampler, to->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };

    VkWriteDescriptorSet writes[2] = {};

    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }

    vkUpdateDescriptorSets(_device.device(), 2, writes, 0, nullptr);

    const float parameters[2] = { std::clamp(progress, 0.f, 1.f), (float)width / (float)std::max(1u, height) };

    GPU::Image* target = passTarget(width, height);
    cmdBeginPass(commandBuffer, target, false);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _transitionPipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commandBuffer, _transitionPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(parameters), parameters);
    vkCmdDraw(commandBuffer, 4, 1, 0, 0);

    cmdEndPass(commandBuffer, target);

    return target;
}
