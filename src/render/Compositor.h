#pragma once

#include <render/FontLibrary.h>

#include <core/Project.h>

#include <vkutils/Buffer.h>
#include <vkutils/Device.h>
#include <vkutils/Image.h>
#include <vkutils/PipelineCache.h>

#include <imgui.h>

#include <array>
#include <memory>
#include <vector>


// Composites the frame of the sequence on the GPU: the layers, bottom to
// top, each placed by the transform of its clip at the current time, into
// an image of the size of the sequence that the viewer shows (and that the
// export will encode).
//
// Video and image layers are textured quads; text layers are drawn glyph by
// glyph from the distance fields of the FontLibrary, one instance per glyph
// (shadow, then outline and fill), with the letter animations applied to
// each glyph.
class Compositor
{
public:
    struct Layer
    {
        const core::Clip* clip = nullptr;
        core::Time localTime = 0;           // Since the start of the clip

        // Picture of a video / image layer, sampled in the shader read only
        // layout. Null for a text layer.
        const GPU::Image* image = nullptr;
        uint32_t width = 0;                 // Size of the picture in the image
        uint32_t height = 0;

        // Added by a transition to the transform of the clip.
        float opacity = 1.f;
        float offsetX = 0.f;
        float offsetY = 0.f;
        float scale = 1.f;
        float wipe = 1.f;                   // Part of the width shown, from the left

        // Texts: false to show all the letters, as while editing.
        bool animateLetters = true;
    };

    // Where a layer was drawn, in pixels of the sequence, for picking it in
    // the viewer: top left, top right, bottom right, bottom left.
    struct LayerBounds
    {
        core::Id clipId;
        std::array<ImVec2, 4> corners;
    };

    // display: the output is shown by ImGui (texture()), otherwise only
    // read by the caller (output()), from any thread.
    Compositor(const GPU::Device& device, const GPU::PipelineCache& pipelineCache, FontLibrary& fonts, bool display = true);
    ~Compositor();

    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Releases the resources of the previous frame, once it completed.
    void beginFrame();

    // Records the composition of the layers (bottom first) into an output of
    // outputWidth x outputHeight. The layers are placed in pixels of the
    // sequence, layoutWidth x layoutHeight (the output size when 0): an
    // output of another size, of the same shape, gets the same frame,
    // drawn at its own resolution.
    void cmdComposite(VkCommandBuffer commandBuffer, uint32_t outputWidth, uint32_t outputHeight, const std::vector<Layer>& layers,
        uint32_t layoutWidth = 0, uint32_t layoutHeight = 0);

    ImTextureID texture() const { return (ImTextureID)_textureDescriptor; }

    // The composited frame, in the shader read only layout after the
    // commands of cmdComposite().
    GPU::Image* output() const { return _output.get(); }
    uint32_t width() const { return _output ? _output->width() : 0; }
    uint32_t height() const { return _output ? _output->height() : 0; }

    // Layers of the last composition, top first.
    const std::vector<LayerBounds>& bounds() const { return _bounds; }

private:
    struct GlyphInstance;

    void createPipelines(const GPU::PipelineCache& pipelineCache);
    VkPipeline createPipeline(const GPU::PipelineCache& pipelineCache, VkPipelineLayout layout,
        const uint32_t* vertexCode, size_t vertexSize, const uint32_t* fragmentCode, size_t fragmentSize);

    void ensureOutput(uint32_t width, uint32_t height);
    void cmdUploadAtlas(VkCommandBuffer commandBuffer);

    // Appends the glyph instances of a text layer. Returns how many.
    uint32_t buildText(const Layer& layer, uint32_t width, uint32_t height, std::vector<GlyphInstance>& instances, LayerBounds& bounds);

    VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);

    const GPU::Device& _device;
    FontLibrary& _fonts;
    bool _display;

    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _layerSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout _textSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layerPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout _textPipelineLayout = VK_NULL_HANDLE;
    VkPipeline _layerPipeline = VK_NULL_HANDLE;
    VkPipeline _textPipeline = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;

    std::unique_ptr<GPU::Image> _output;
    VkDescriptorSet _textureDescriptor = VK_NULL_HANDLE;

    std::unique_ptr<GPU::Image> _atlas;
    std::unique_ptr<GPU::Buffer> _atlasStaging;
    std::unique_ptr<GPU::Buffer> _glyphBuffer;

    std::vector<LayerBounds> _bounds;
};
