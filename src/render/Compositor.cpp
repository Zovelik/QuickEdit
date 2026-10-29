#include <render/Compositor.h>
#include <render/TextLayout.h>

#include <shader/layer.frag.h>
#include <shader/layer.vert.h>
#include <shader/text.frag.h>
#include <shader/text.vert.h>

#include <vkutils/util.h>

#include <backends/imgui_impl_vulkan.h>

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
    vkDestroyPipeline(device, _layerPipeline, nullptr);
    vkDestroyPipeline(device, _textPipeline, nullptr);
    vkDestroyPipelineLayout(device, _layerPipelineLayout, nullptr);
    vkDestroyPipelineLayout(device, _textPipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _layerSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, _textSetLayout, nullptr);
    vkDestroySampler(device, _sampler, nullptr);
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

    // Sets of the frame, recycled by beginFrame().
    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MaxLayers },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MaxLayers },
    };

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = MaxLayers;
    poolInfo.poolSizeCount = C_ARRAY_SIZE(poolSizes);
    poolInfo.pPoolSizes = poolSizes;
    VK_THROW_IF_FAILED(vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool));
}


VkPipeline Compositor::createPipeline(const GPU::PipelineCache& pipelineCache, VkPipelineLayout layout,
    const uint32_t* vertexCode, size_t vertexSize, const uint32_t* fragmentCode, size_t fragmentSize)
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
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
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
                return place.apply(lx, ly);
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

    // Texts first: their layout renders the missing glyphs into the atlas,
    // uploaded before drawing.
    std::vector<GlyphInstance> instances;
    std::vector<std::pair<uint32_t, uint32_t>> textRanges(layers.size(), { 0, 0 });
    std::vector<LayerBounds> bounds(layers.size());

    for (size_t i = 0; i < layers.size(); i++) {
        const Layer& layer = layers[i];
        bounds[i].clipId = layer.clip->id;

        if (!layer.image) {
            const uint32_t first = (uint32_t)instances.size();
            const uint32_t count = buildText(layer, width, height, instances, bounds[i]);
            textRanges[i] = { first, count };
        }
    }

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

    // Render
    _output->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    attachment.imageView = _output->imageView();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = { { 0.f, 0.f, 0.f, 1.f } };

    VkRenderingInfo renderingInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderingInfo.renderArea = { { 0, 0 }, { outputWidth, outputHeight } };
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &attachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);

    const VkViewport viewport = { 0.f, 0.f, (float)outputWidth, (float)outputHeight, 0.f, 1.f };
    const VkRect2D scissor = { { 0, 0 }, { outputWidth, outputHeight } };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    for (size_t i = 0; i < layers.size(); i++) {
        const Layer& layer = layers[i];

        // Wipe transition: the layer only covers the left part of the frame.
        const VkRect2D layerScissor = { { 0, 0 }, { (uint32_t)std::clamp(std::lround(layer.wipe * (float)outputWidth), 0L, (long)outputWidth), outputHeight } };

        if (layerScissor.extent.width == 0) {
            continue;
        }

        vkCmdSetScissor(commandBuffer, 0, 1, &layerScissor);

        if (layer.image) {
            const float opacity = layerOpacity(layer);

            // The picture fits the frame, then the transform applies. The
            // crop keeps the rest of the picture in place.
            const float fit = std::min((float)width / (float)layer.width, (float)height / (float)layer.height);
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

            bounds[i].corners = { place.apply(box[0], box[1]), place.apply(box[2], box[1]), place.apply(box[2], box[3]), place.apply(box[0], box[3]) };

            if (opacity <= 0.f) {
                continue;
            }

            const VkDescriptorSet set = allocateSet(_layerSetLayout);

            if (!set) {
                continue;
            }

            const VkDescriptorImageInfo imageInfo = { _sampler, layer.image->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            vkUpdateDescriptorSets(_device.device(), 1, &write, 0, nullptr);

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _layerPipeline);
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

                parameters.color[0] = parameters.color[1] = parameters.color[2] = 1.f;
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
        else if (textRanges[i].second > 0) {
            const VkDescriptorSet set = allocateSet(_textSetLayout);

            if (!set) {
                continue;
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
            vkCmdDraw(commandBuffer, 4, textRanges[i].second, 0, textRanges[i].first);
        }
    }

    vkCmdEndRendering(commandBuffer);

    _output->cmdBarrierTransition(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Top first, for picking.
    std::reverse(bounds.begin(), bounds.end());
    _bounds = std::move(bounds);
}
