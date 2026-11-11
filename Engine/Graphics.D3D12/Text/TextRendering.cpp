#include "Text/TextRendering.h"
#include "Text/D3D12TextRendering.h"
#include "Renderer/FrameResources.h"

#include "Shader/ShaderCompiler.h"

#include <d3dcompiler.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "dwrite.lib")

namespace PhantomCore
{
namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr std::uint32_t AtlasWidth = 1024;
    constexpr std::uint32_t AtlasHeight = 1024;
    constexpr std::uint32_t DefaultAtlasPages = 16;
    constexpr std::uint32_t GlyphPadding = 1;

    void ThrowIfFailed(const HRESULT result, const char* operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation) +
                " failed with HRESULT " +
                std::to_string(static_cast<unsigned long>(result)) + ".");
        }
    }

    [[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(
        const D3D12_HEAP_TYPE type) noexcept
    {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = type;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    [[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(
        const std::size_t sizeBytes) noexcept
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Alignment = 0;
        description.Width = static_cast<UINT64>(sizeBytes);
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        return description;
    }

    [[nodiscard]] D3D12_RESOURCE_BARRIER TransitionBarrier(
        ID3D12Resource& resource,
        const D3D12_RESOURCE_STATES before,
        const D3D12_RESOURCE_STATES after) noexcept
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = &resource;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    [[nodiscard]] std::uint32_t Align(
        const std::uint32_t value,
        const std::uint32_t alignment) noexcept
    {
        return (value + alignment - 1U) & ~(alignment - 1U);
    }

    [[nodiscard]] std::wstring LocalizedString(
        IDWriteLocalizedStrings& strings)
    {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(strings.FindLocaleName(L"en-us", &index, &exists)) ||
            !exists)
        {
            index = 0;
        }

        UINT32 length = 0;
        ThrowIfFailed(
            strings.GetStringLength(index, &length),
            "IDWriteLocalizedStrings::GetStringLength");
        std::wstring value(length + 1U, L'\0');
        ThrowIfFailed(
            strings.GetString(index, value.data(), length + 1U),
            "IDWriteLocalizedStrings::GetString");
        value.resize(length);
        return value;
    }

    [[nodiscard]] DWRITE_TEXT_ALIGNMENT ToDirectWrite(
        const TextHorizontalAlignment alignment) noexcept
    {
        switch (alignment)
        {
        case TextHorizontalAlignment::Center:
            return DWRITE_TEXT_ALIGNMENT_CENTER;
        case TextHorizontalAlignment::Trailing:
            return DWRITE_TEXT_ALIGNMENT_TRAILING;
        case TextHorizontalAlignment::Leading:
        default:
            return DWRITE_TEXT_ALIGNMENT_LEADING;
        }
    }

    [[nodiscard]] DWRITE_PARAGRAPH_ALIGNMENT ToDirectWrite(
        const TextVerticalAlignment alignment) noexcept
    {
        switch (alignment)
        {
        case TextVerticalAlignment::Center:
            return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
        case TextVerticalAlignment::Far:
            return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
        case TextVerticalAlignment::Near:
        default:
            return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
        }
    }

    template <typename Value>
    void HashCombine(std::size_t& seed, const Value& value) noexcept
    {
        const std::size_t hash = std::hash<Value>{}(value);
        seed ^= hash + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    }
}


struct Font::Impl final
{
    std::wstring familyName;
    ComPtr<IDWriteFontCollection> collection;
};

Font::Font(std::shared_ptr<Impl> implementation)
    : implementation_(std::move(implementation))
{
}

Font::~Font() = default;

std::wstring_view Font::FamilyName() const noexcept
{
    return implementation_->familyName;
}

struct TextLayout::Impl final
{
    struct Glyph
    {
        std::uint16_t index{};
        DirectX::XMFLOAT2 origin{};
        DirectX::XMFLOAT4 bounds{};
    };
    struct Block
    {
        std::size_t begin{}, end{};
        DirectX::XMFLOAT4 bounds{};
    };
    struct Run
    {
        ComPtr<IDWriteFontFace> face;
        ComPtr<IUnknown> identity;
        float size{};
        DWRITE_MEASURING_MODE measuringMode{};
        std::vector<Glyph> glyphs;
        std::vector<Block> blocks;
    };

    void AppendRun(float originX, float originY, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN &source)
    {
        if (source.isSideways)
            throw std::invalid_argument("Vertical text is not supported by this text renderer.");
        Run run;
        run.face = source.fontFace;
        ThrowIfFailed(source.fontFace->QueryInterface(IID_PPV_ARGS(run.identity.GetAddressOf())),
                      "IDWriteFontFace::QueryInterface");
        run.size = source.fontEmSize;
        run.measuringMode = mode;
        DWRITE_FONT_METRICS fontMetrics{};
        source.fontFace->GetMetrics(&fontMetrics);
        if (fontMetrics.designUnitsPerEm == 0)
            throw std::invalid_argument("A font must have nonzero design units per em.");
        const float scale = source.fontEmSize / static_cast<float>(fontMetrics.designUnitsPerEm);
        std::vector<DWRITE_GLYPH_METRICS> metrics(source.glyphCount);
        ThrowIfFailed(
            source.fontFace->GetDesignGlyphMetrics(source.glyphIndices, source.glyphCount, metrics.data(), FALSE),
            "IDWriteFontFace::GetDesignGlyphMetrics");
        run.glyphs.reserve(source.glyphCount);
        const bool rtl = (source.bidiLevel & 1U) != 0;
        float pen = originX;
        for (UINT32 index = 0; index < source.glyphCount; ++index)
        {
            const float advance = source.glyphAdvances ? source.glyphAdvances[index] : 0.0F;
            if (rtl)
                pen -= advance;
            const auto offset = source.glyphOffsets ? source.glyphOffsets[index] : DWRITE_GLYPH_OFFSET{};
            Glyph glyph;
            glyph.index = source.glyphIndices[index];
            glyph.origin = {pen + (rtl ? -offset.advanceOffset : offset.advanceOffset),
                            originY - offset.ascenderOffset};
            const auto &metric = metrics[index];
            // Conservative outline bounds include grid fitting / antialias overhang.
            glyph.bounds = {
                glyph.origin.x + static_cast<float>(metric.leftSideBearing) * scale - 2.0F,
                glyph.origin.y +
                    (static_cast<float>(metric.topSideBearing) - static_cast<float>(metric.verticalOriginY)) *
                        scale -
                    2.0F,
                glyph.origin.x +
                    (static_cast<float>(metric.advanceWidth) - static_cast<float>(metric.rightSideBearing)) *
                        scale +
                    2.0F,
                glyph.origin.y +
                    (static_cast<float>(metric.advanceHeight) - static_cast<float>(metric.bottomSideBearing) -
                     static_cast<float>(metric.verticalOriginY)) *
                        scale +
                    2.0F};
            run.glyphs.push_back(glyph);
            if (!rtl)
                pen += advance;
        }
        // A long clipped title visits blocks first, then only nearby glyphs.
        for (std::size_t begin = 0; begin < run.glyphs.size(); begin += 64)
        {
            Block block{begin, std::min(begin + 64, run.glyphs.size()), run.glyphs[begin].bounds};
            for (std::size_t index = begin + 1; index < block.end; ++index)
            {
                const auto &bounds = run.glyphs[index].bounds;
                block.bounds.x = std::min(block.bounds.x, bounds.x);
                block.bounds.y = std::min(block.bounds.y, bounds.y);
                block.bounds.z = std::max(block.bounds.z, bounds.z);
                block.bounds.w = std::max(block.bounds.w, bounds.w);
            }
            run.blocks.push_back(block);
        }
        runs.push_back(std::move(run));
    }

    FontHandle font;
    DirectX::XMFLOAT2 size{};
    std::vector<Run> runs;
};

namespace
{
    // Owns the immutable text root signature and graphics pipeline.
    class TextPipeline final
    {
    public:
        void Initialize(ID3D12Device& device, const DXGI_FORMAT color,
                        const DXGI_FORMAT depth)
        {
            CreateRootSignature(device);
            CreatePipelineState(device, color, depth);
        }

        void Reset() noexcept
        {
            pipelineState_.Reset();
            rootSignature_.Reset();
        }

        void Bind(ID3D12GraphicsCommandList& commands) const
        {
            commands.SetGraphicsRootSignature(rootSignature_.Get());
            commands.SetPipelineState(pipelineState_.Get());
        }

    private:
        void CreateRootSignature(ID3D12Device& device)
        {
            // Root parameters expose the current atlas page, the structured
            // glyph-instance range, and the viewport size to the shaders.
            D3D12_DESCRIPTOR_RANGE atlasRange{};
            atlasRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            atlasRange.NumDescriptors = 1;
            atlasRange.BaseShaderRegister = 0;
            atlasRange.RegisterSpace = 0;
            atlasRange.OffsetInDescriptorsFromTableStart = 0;

            std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
            parameters[0].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[0].DescriptorTable.NumDescriptorRanges = 1;
            parameters[0].DescriptorTable.pDescriptorRanges =
                &atlasRange;
            parameters[0].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;

            parameters[1].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[1].Descriptor.ShaderRegister = 1;
            parameters[1].Descriptor.RegisterSpace = 0;
            parameters[1].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_VERTEX;

            parameters[2].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[2].Constants.ShaderRegister = 0;
            parameters[2].Constants.RegisterSpace = 0;
            parameters[2].Constants.Num32BitValues = 2;
            parameters[2].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_VERTEX;

            D3D12_STATIC_SAMPLER_DESC sampler{};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MipLODBias = 0.0F;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler.BorderColor =
                D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            sampler.MinLOD = 0.0F;
            sampler.MaxLOD = FLT_MAX;
            sampler.ShaderRegister = 0;
            sampler.RegisterSpace = 0;
            sampler.ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rootDescription{};
            rootDescription.NumParameters =
                static_cast<UINT>(parameters.size());
            rootDescription.pParameters = parameters.data();
            rootDescription.NumStaticSamplers = 1;
            rootDescription.pStaticSamplers = &sampler;
            rootDescription.Flags =
                D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> errors;
            const HRESULT serializationResult =
                D3D12SerializeRootSignature(
                    &rootDescription,
                    D3D_ROOT_SIGNATURE_VERSION_1,
                    serialized.ReleaseAndGetAddressOf(),
                    errors.ReleaseAndGetAddressOf());
            if (FAILED(serializationResult))
            {
                const std::string details = errors != nullptr
                    ? std::string(
                        static_cast<const char*>(
                            errors->GetBufferPointer()),
                        errors->GetBufferSize())
                    : "No root-signature diagnostics were returned.";
                throw std::runtime_error(
                    "Text root-signature serialization failed: " +
                    details);
            }
            ThrowIfFailed(
                device.CreateRootSignature(
                    0,
                    serialized->GetBufferPointer(),
                    serialized->GetBufferSize(),
                    IID_PPV_ARGS(
                        rootSignature_.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateRootSignature(text)");
        }

        void CreatePipelineState(ID3D12Device& device,
            const DXGI_FORMAT renderTargetFormat, const DXGI_FORMAT depthStencilFormat)
        {
            const ComPtr<ID3DBlob> vertexShader =
                CompileShader(
                    BuiltInShader::Text,
                    "VSMain",
                    "vs_5_1");
            const ComPtr<ID3DBlob> pixelShader =
                CompileShader(
                    BuiltInShader::Text,
                    "PSMain",
                    "ps_5_1");

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = rootSignature_.Get();
            pipeline.VS = {
                vertexShader->GetBufferPointer(),
                vertexShader->GetBufferSize()};
            pipeline.PS = {
                pixelShader->GetBufferPointer(),
                pixelShader->GetBufferSize()};
            pipeline.BlendState.AlphaToCoverageEnable = FALSE;
            pipeline.BlendState.IndependentBlendEnable = FALSE;
            D3D12_RENDER_TARGET_BLEND_DESC& blend =
                pipeline.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.LogicOpEnable = FALSE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;

            pipeline.SampleMask = UINT_MAX;
            pipeline.RasterizerState.FillMode =
                D3D12_FILL_MODE_SOLID;
            pipeline.RasterizerState.CullMode =
                D3D12_CULL_MODE_NONE;
            pipeline.RasterizerState.FrontCounterClockwise = FALSE;
            pipeline.RasterizerState.DepthBias =
                D3D12_DEFAULT_DEPTH_BIAS;
            pipeline.RasterizerState.DepthBiasClamp =
                D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
            pipeline.RasterizerState.SlopeScaledDepthBias =
                D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
            pipeline.RasterizerState.DepthClipEnable = TRUE;
            pipeline.RasterizerState.MultisampleEnable = FALSE;
            pipeline.RasterizerState.AntialiasedLineEnable = FALSE;
            pipeline.RasterizerState.ForcedSampleCount = 0;
            pipeline.RasterizerState.ConservativeRaster =
                D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            pipeline.DepthStencilState.DepthEnable =
                depthStencilFormat != DXGI_FORMAT_UNKNOWN;
            pipeline.DepthStencilState.DepthWriteMask =
                D3D12_DEPTH_WRITE_MASK_ZERO;
            pipeline.DepthStencilState.DepthFunc =
                D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pipeline.DepthStencilState.StencilEnable = FALSE;
            pipeline.InputLayout = {nullptr, 0};
            pipeline.PrimitiveTopologyType =
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline.NumRenderTargets = 1;
            pipeline.RTVFormats[0] = renderTargetFormat;
            pipeline.DSVFormat = depthStencilFormat;
            pipeline.SampleDesc.Count = 1;

            ThrowIfFailed(
                device.CreateGraphicsPipelineState(
                    &pipeline,
                    IID_PPV_ARGS(
                        pipelineState_.ReleaseAndGetAddressOf())),
                "ID3D12Device::CreateGraphicsPipelineState(text)");
        }

        ComPtr<ID3D12RootSignature> rootSignature_;
        ComPtr<ID3D12PipelineState> pipelineState_;
    };
}

struct TextRenderSystem::Impl final
{
    using CanvasDraw = TextRenderingBackend::CanvasDraw;
    class LayoutCache final
    {
    public:
        void Initialize();
        void ClearCaches() noexcept;
        void ReleaseFactory() noexcept;
        [[nodiscard]] IDWriteFactory5* Factory() const noexcept;
        [[nodiscard]] FontHandle LoadSystemFont(std::wstring_view familyName);
        [[nodiscard]] FontHandle LoadFontFile(const std::filesystem::path& fontFile);
        [[nodiscard]] TextLayoutHandle FindOrCreateLayout(std::wstring_view text,
            const TextDrawCommand& command, TextRenderStatistics& statistics);

    private:
        struct LayoutKey final
        {
            FontHandle font;
            std::wstring text;
            std::uint32_t fontSizeBits{};
            std::uint32_t widthBits{};
            std::uint32_t heightBits{};
            TextHorizontalAlignment horizontalAlignment{};
            TextVerticalAlignment verticalAlignment{};
            std::uint16_t fontWeight{400};

            [[nodiscard]] bool operator==(
                const LayoutKey& other) const noexcept
            {
                return font.get() == other.font.get() && text == other.text && fontSizeBits == other.fontSizeBits &&
                       widthBits == other.widthBits && heightBits == other.heightBits &&
                       horizontalAlignment == other.horizontalAlignment &&
                       verticalAlignment == other.verticalAlignment && fontWeight == other.fontWeight;
            }
        };

        struct LayoutKeyView
        {
            const Font *font{};
            std::wstring_view text;
            std::uint32_t fontSizeBits{}, widthBits{}, heightBits{};
            TextHorizontalAlignment horizontalAlignment{};
            TextVerticalAlignment verticalAlignment{};
            std::uint16_t fontWeight{400};
            bool operator==(const LayoutKeyView &) const noexcept = default;
        };
        static LayoutKeyView KeyView(const LayoutKey &key) noexcept
        {
            return {key.font.get(),        key.text,       key.fontSizeBits,
                    key.widthBits,         key.heightBits, key.horizontalAlignment,
                    key.verticalAlignment, key.fontWeight};
        }
        struct LayoutKeyHash final
        {
            using is_transparent = void;
            std::size_t operator()(const LayoutKey &key) const noexcept
            {
                return (*this)(KeyView(key));
            }
            std::size_t operator()(const LayoutKeyView &key) const noexcept
            {
                std::size_t result = 0;
                HashCombine(result, reinterpret_cast<std::uintptr_t>(key.font));
                HashCombine(result, key.text);
                HashCombine(result, key.fontSizeBits);
                HashCombine(result, key.widthBits);
                HashCombine(result, key.heightBits);
                HashCombine(result, static_cast<std::uint32_t>(key.horizontalAlignment));
                HashCombine(result, static_cast<std::uint32_t>(key.verticalAlignment));
                HashCombine(result, key.fontWeight);
                return result;
            }
        };
        struct LayoutKeyEqual final
        {
            using is_transparent = void;
            bool operator()(const LayoutKey &a, const LayoutKey &b) const noexcept
            {
                return a == b;
            }
            bool operator()(const LayoutKey &a, const LayoutKeyView &b) const noexcept
            {
                return KeyView(a) == b;
            }
            bool operator()(const LayoutKeyView &a, const LayoutKey &b) const noexcept
            {
                return a == KeyView(b);
            }
        };

        class GlyphRunCollector;
        static constexpr std::size_t CachedLayoutLimit = 2048;
        ComPtr<IDWriteTextFormat> FindOrCreateFormat(const TextDrawCommand& command);
        TextLayoutHandle CompileLayout(std::wstring_view text, const TextDrawCommand& command,
            const ComPtr<IDWriteTextFormat>& format);
        ComPtr<IDWriteFactory5> directWriteFactory;
        struct LayoutEntry
        {
            TextLayoutHandle layout;
            std::list<const LayoutKey *>::iterator recency;
        };
        // Map nodes own each key once; rehash preserves their addresses.
        std::list<const LayoutKey *> layoutRecency;
        std::unordered_map<LayoutKey, LayoutEntry, LayoutKeyHash, LayoutKeyEqual> layoutCache;
        struct FormatEntry
        {
            ComPtr<IDWriteTextFormat> format;
            std::list<LayoutKey>::iterator recency;
        };
        std::list<LayoutKey> formatRecency;
        std::unordered_map<LayoutKey, FormatEntry, LayoutKeyHash> formatCache;
    };

    struct GlyphInstance final
    {
        DirectX::XMFLOAT4X4 transform{};
        DirectX::XMFLOAT2 positionPixels{};
        DirectX::XMFLOAT2 sizePixels{};
        DirectX::XMFLOAT4 uvRectangle{};
        DirectX::XMFLOAT4 color{1.0F, 1.0F, 1.0F, 1.0F};
        DirectX::XMFLOAT4 clipRectPixels{
            -1.0e9F, -1.0e9F, 1.0e9F, 1.0e9F};
        float depth{};
        std::uint32_t pageIndex{};
    };

    struct GpuGlyphInstance final
    {
        DirectX::XMFLOAT4X4 transform{};
        DirectX::XMFLOAT2 positionPixels{};
        DirectX::XMFLOAT2 sizePixels{};
        DirectX::XMFLOAT4 uvRectangle{};
        DirectX::XMFLOAT4 color{1.0F, 1.0F, 1.0F, 1.0F};
        DirectX::XMFLOAT4 clipRectPixels{
            -1.0e9F, -1.0e9F, 1.0e9F, 1.0e9F};
        float depth{};
    };

    static_assert(sizeof(GpuGlyphInstance) == 132);

    struct FrameInstancePage final
    {
        ComPtr<ID3D12Resource> resource;
        std::byte* mappedData{};
        std::size_t capacity{};
        std::size_t used{};
    };

    struct FrameInstanceBuffer final
    {
        std::vector<FrameInstancePage> pages;
        std::vector<FrameInstancePage> glyphUploadPages;
        std::vector<ComPtr<ID3D12DescriptorHeap>> descriptorHeaps;
        std::uint64_t renderIndex{};
    };

    struct AtlasPage final
    {
        ComPtr<ID3D12Resource> texture;
        std::uint32_t nextX{GlyphPadding};
        std::uint32_t nextY{GlyphPadding};
        std::uint32_t rowHeight{};
        std::uint8_t inFlightFrames{};
        std::uint64_t lastUsedRenderIndex{};
    };

    struct PendingGlyphUpload final
    {
        std::uint32_t pageIndex{};
        std::uint32_t destinationX{};
        std::uint32_t destinationY{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> pixels;
    };

    struct GlyphKey final
    {
        std::uintptr_t fontFaceIdentity{};
        std::uint32_t fontSizeBits{};
        std::uint16_t glyphIndex{};
        DWRITE_MEASURING_MODE measuringMode{};

        [[nodiscard]] bool operator==(const GlyphKey&) const noexcept =
            default;
    };

    struct GlyphKeyHash final
    {
        [[nodiscard]] std::size_t operator()(
            const GlyphKey& key) const noexcept
        {
            std::size_t result = 0;
            HashCombine(result, key.fontFaceIdentity);
            HashCombine(result, key.fontSizeBits);
            HashCombine(result, key.glyphIndex);
            HashCombine(
                result,
                static_cast<std::uint32_t>(key.measuringMode));
            return result;
        }
    };

    struct GlyphEntry final
    {
        ComPtr<IUnknown> fontFaceIdentity;
        std::uint32_t pageIndex{};
        DirectX::XMFLOAT2 sizePixels{};
        DirectX::XMFLOAT2 bearingPixels{};
        DirectX::XMFLOAT4 uvRectangle{};
        bool drawable{};
    };



    void Initialize(
        ID3D12Device& newDevice,
        const DXGI_FORMAT renderTargetFormat,
        const DXGI_FORMAT newDepthStencilFormat)
    {
        if (initialized)
        {
            throw std::logic_error(
                "TextRenderSystem is already initialized.");
        }

        device = &newDevice;
        layouts.Initialize();

        CreateDescriptorHeap();
        pipeline.Initialize(newDevice, renderTargetFormat, newDepthStencilFormat);
        initialized = true;
    }

    void Shutdown() noexcept
    {
        for (FrameInstanceBuffer& frame : frameBuffers)
        {
            for (FrameInstancePage& page : frame.pages)
            {
                if (page.resource != nullptr &&
                    page.mappedData != nullptr)
                {
                    page.resource->Unmap(0, nullptr);
                }
            }
            for (FrameInstancePage &page : frame.glyphUploadPages)
                if (page.resource && page.mappedData)
                    page.resource->Unmap(0, nullptr);
            frame = {};
        }

        pendingInstances.clear();
        pendingUploads.clear();
        glyphCache.clear();
        layouts.ClearCaches();
        atlasPages.clear();
        pipeline.Reset();
        atlasDescriptorHeap.Reset();
        layouts.ReleaseFactory();
        device = nullptr;
        currentFrameIndex = 0;
        currentRenderIndex = 0;
        atlasDescriptorCapacity = 0;
        statistics = {};
        initialized = false;
        frameOpen = false;
    }

    [[nodiscard]] FontHandle LoadSystemFont(
        const std::wstring_view familyName)
    {
        RequireInitialized();
        return layouts.LoadSystemFont(familyName);
    }

    [[nodiscard]] FontHandle LoadFontFile(
        const std::filesystem::path& fontFile)
    {
        RequireInitialized();
        return layouts.LoadFontFile(fontFile);
    }

    void BeginFrame(
        const std::uint32_t frameIndex,
        const std::uint64_t renderIndex)
    {
        RequireInitialized();
        if (frameIndex >= D3D12Detail::FrameCount)
        {
            throw std::out_of_range(
                "The text frame index is outside the frame ring.");
        }
        if (frameOpen)
        {
            throw std::logic_error(
                "A text rendering frame is already open.");
        }

        currentFrameIndex = frameIndex;
        if (currentRenderIndex != renderIndex)
            statistics = {};
        currentRenderIndex = renderIndex;
        pendingInstances.clear();
        pendingUploads.clear();
        FrameInstanceBuffer& frame = frameBuffers[frameIndex];
        if (frame.renderIndex != renderIndex)
        {
            frame.renderIndex = renderIndex;

            frame.descriptorHeaps.clear();
            for (auto &page : atlasPages)
            {
                page.inFlightFrames &= static_cast<std::uint8_t>(~(1U << frameIndex));
                // An inactive off-screen pass may skip one ring index.
                // Engine frames older than the two-frame fence ring are complete.
                if (renderIndex >= page.lastUsedRenderIndex && renderIndex - page.lastUsedRenderIndex >= D3D12Detail::FrameCount)
                    page.inFlightFrames = 0;
            }
            TrimAtlasPages();
            for (FrameInstancePage& page : frame.pages)
            {
                page.used = 0;
            }
            for (auto &page : frame.glyphUploadPages)
                page.used = 0;
        }
        frameOpen = true;
    }

    static void ValidateLayoutCommand(const TextDrawCommand &command)
    {
        if (!command.style.font)
            throw std::invalid_argument("TextStyle requires a valid FontHandle.");
        if (command.style.fontWeight < 1 || command.style.fontWeight > 999)
            throw std::invalid_argument("Text font weight must be in 1..999.");
        if (!std::isfinite(command.style.fontSizePixels) || command.style.fontSizePixels <= 0)
            throw std::invalid_argument("Text font size must be positive and finite.");
        if (!std::isfinite(command.layoutSizePixels.x) || !std::isfinite(command.layoutSizePixels.y) ||
            command.layoutSizePixels.x <= 0 || command.layoutSizePixels.y <= 0)
            throw std::invalid_argument("Text layout size must be positive and finite.");
    }

    TextLayoutHandle CreateLayout(std::wstring_view text, const TextDrawCommand &command)
    {
        RequireInitialized();
        ValidateLayoutCommand(command);
        if (text.size() > std::numeric_limits<UINT32>::max())
            throw std::length_error("Text exceeds DirectWrite's string length limit.");
        return layouts.FindOrCreateLayout(text, command, statistics);
    }

    void Submit(std::wstring_view text, const TextDrawCommand &command)
    {
        RequireFrameOpen();
        if (!text.empty())
            Submit(CreateLayout(text, command), command);
    }

    static bool IntersectsClip(const DirectX::XMFLOAT4 &localBounds, const TextDrawCommand &command)
    {
        const auto matrix = DirectX::XMLoadFloat4x4(&command.transform);
        float left = std::numeric_limits<float>::infinity(), top = left;
        float right = -left, bottom = -left;
        for (const auto x : {localBounds.x, localBounds.z})
            for (const auto y : {localBounds.y, localBounds.w})
            {
                const auto point = DirectX::XMVector4Transform(
                    DirectX::XMVectorSet(x + command.positionPixels.x, y + command.positionPixels.y, 0, 1), matrix);
                const float px = DirectX::XMVectorGetX(point), py = DirectX::XMVectorGetY(point);
                left = std::min(left, px);
                right = std::max(right, px);
                top = std::min(top, py);
                bottom = std::max(bottom, py);
            }
        const auto &clip = command.clipRectPixels;
        return left < clip.z && top < clip.w && right > clip.x && bottom > clip.y;
    }

    void Submit(const TextLayoutHandle &layout, const TextDrawCommand &command)
    {
        RequireFrameOpen();
        if (!layout)
            throw std::invalid_argument("A prepared text layout is required.");
        for (const auto &run : layout->implementation_->runs)
        {
            for (const auto &block : run.blocks)
            {
                if (!IntersectsClip(block.bounds, command))
                    continue;
                for (std::size_t index = block.begin; index < block.end; ++index)
                {
                    const auto &prepared = run.glyphs[index];
                    if (!IntersectsClip(prepared.bounds, command))
                        continue;
                    const GlyphEntry &glyph = FindOrCreateGlyph(*run.face.Get(), run.identity.Get(), prepared.index,
                                                                run.size, run.measuringMode);
                    if (!glyph.drawable)
                        continue;
                    auto &atlas = atlasPages[glyph.pageIndex];
                    atlas.lastUsedRenderIndex = currentRenderIndex;
                    atlas.inFlightFrames |= static_cast<std::uint8_t>(1U << currentFrameIndex);
                    pendingInstances.push_back(
                        GlyphInstance{command.transform,
                                      {command.positionPixels.x + prepared.origin.x + glyph.bearingPixels.x,
                                       command.positionPixels.y + prepared.origin.y + glyph.bearingPixels.y},
                                      glyph.sizePixels,
                                      glyph.uvRectangle,
                                      command.style.color,
                                      command.clipRectPixels,
                                      command.depth,
                                      glyph.pageIndex});
                    ++statistics.submittedGlyphs;
                }
            }
        }
    }
    void Flush(
        ID3D12GraphicsCommandList& commandList,
        const std::uint32_t viewportWidth,
        const std::uint32_t viewportHeight)
    {
        RequireFrameOpen();
        if (viewportWidth != 0 && viewportHeight != 0)
        {
            RecordInstances(commandList, viewportWidth, viewportHeight, pendingInstances);
        }
        pendingInstances.clear();
        pendingUploads.clear();
        frameOpen = false;
    }

    void DrawCanvasBatch(
        ID3D12GraphicsCommandList& commandList,
        const std::uint32_t viewportWidth,
        const std::uint32_t viewportHeight,
        const std::span<const CanvasDraw> draws)
    {
        RequireFrameOpen();
        // Direct text stays queued for the final pass. Atlas uploads are
        // shared so either pass can use a glyph prepared by the other.
        const auto first = pendingInstances.size();
        for (const auto& draw : draws)
        {
            Submit(draw.layout, draw.command);
        }
        RecordInstances(commandList, viewportWidth, viewportHeight,
            std::span<const GlyphInstance>(pendingInstances).subspan(first));
        pendingInstances.resize(first);
    }

    void RecordInstances(
        ID3D12GraphicsCommandList& commandList,
        const std::uint32_t viewportWidth,
        const std::uint32_t viewportHeight,
        const std::span<const GlyphInstance> instances)
    {
        // New glyph bitmaps must reach their atlas pages before any
        // instance in this frame samples them.
        UploadNewGlyphs(commandList);
        pendingUploads.clear();
        if (!instances.empty())
        {
            // Preserve paint order, including overlapping translucent text.
            // Consecutive pages are batched below; no per-frame glyph sorting.
            FrameInstanceBuffer& frame =
                frameBuffers[currentFrameIndex];
            std::size_t allocationStart = 0;
            FrameInstancePage& page = AllocateInstanceRange(
                frame,
                instances.size(),
                allocationStart);
            auto* destination =
                reinterpret_cast<GpuGlyphInstance*>(page.mappedData) +
                allocationStart;
            for (std::size_t index = 0;
                 index < instances.size();
                 ++index)
            {
                const GlyphInstance& source = instances[index];
                destination[index] = GpuGlyphInstance{
                    source.transform,
                    source.positionPixels,
                    source.sizePixels,
                    source.uvRectangle,
                    source.color,
                    source.clipRectPixels,
                    source.depth};
            }

            ID3D12DescriptorHeap* heaps[]{
                atlasDescriptorHeap.Get()};
            // Heap growth keeps every heap used by recorded passes alive.
            if (frame.descriptorHeaps.empty() || frame.descriptorHeaps.back() != atlasDescriptorHeap)
                frame.descriptorHeaps.push_back(atlasDescriptorHeap);
            commandList.SetDescriptorHeaps(1, heaps);
            pipeline.Bind(commandList);
            commandList.IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            const std::array viewportConstants{
                std::bit_cast<std::uint32_t>(
                    static_cast<float>(viewportWidth)),
                std::bit_cast<std::uint32_t>(
                    static_cast<float>(viewportHeight))};
            commandList.SetGraphicsRoot32BitConstants(
                2,
                static_cast<UINT>(viewportConstants.size()),
                viewportConstants.data(),
                0);

            // Emit one instanced draw for each consecutive atlas page.
            const D3D12_GPU_DESCRIPTOR_HANDLE heapStart =
                atlasDescriptorHeap->
                    GetGPUDescriptorHandleForHeapStart();
            std::size_t firstInstance = 0;
            while (firstInstance < instances.size())
            {
                const std::uint32_t pageIndex =
                    instances[firstInstance].pageIndex;
                std::size_t endInstance = firstInstance + 1;
                while (endInstance < instances.size() &&
                    instances[endInstance].pageIndex == pageIndex)
                {
                    ++endInstance;
                }

                D3D12_GPU_DESCRIPTOR_HANDLE pageDescriptor = heapStart;
                pageDescriptor.ptr +=
                    static_cast<UINT64>(pageIndex) *
                    atlasDescriptorSize;
                commandList.SetGraphicsRootDescriptorTable(
                    0,
                    pageDescriptor);
                commandList.SetGraphicsRootShaderResourceView(
                    1,
                    page.resource->GetGPUVirtualAddress() +
                        static_cast<UINT64>(
                            allocationStart + firstInstance) *
                        sizeof(GpuGlyphInstance));
                commandList.DrawInstanced(
                    6,
                    static_cast<UINT>(endInstance - firstInstance),
                    0,
                    0);
                firstInstance = endInstance;
            }
        }
    }








    [[nodiscard]] const GlyphEntry &FindOrCreateGlyph(IDWriteFontFace &fontFace, IUnknown *identity,
                                                      const std::uint16_t glyphIndex, const float fontEmSize,
                                                      const DWRITE_MEASURING_MODE measuringMode)
    {
        const GlyphKey key{reinterpret_cast<std::uintptr_t>(identity), std::bit_cast<std::uint32_t>(fontEmSize),
                           glyphIndex, measuringMode};
        if (const auto found = glyphCache.find(key);
            found != glyphCache.end())
        {
            return found->second;
        }

        ++statistics.glyphRasterizations;
        // Rasterize exactly one glyph. Its font-face COM identity is part
        // of the cache key, so equal glyph indices from different fonts
        // never share an atlas entry accidentally.
        const float zeroAdvance = 0.0F;
        const DWRITE_GLYPH_OFFSET zeroOffset{};
        DWRITE_GLYPH_RUN singleGlyphRun{};
        singleGlyphRun.fontFace = &fontFace;
        singleGlyphRun.fontEmSize = fontEmSize;
        singleGlyphRun.glyphCount = 1;
        singleGlyphRun.glyphIndices = &glyphIndex;
        singleGlyphRun.glyphAdvances = &zeroAdvance;
        singleGlyphRun.glyphOffsets = &zeroOffset;

        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        // Grayscale directly produces the single alpha channel used by R8.
        ThrowIfFailed(layouts.Factory()->CreateGlyphRunAnalysis(
                          &singleGlyphRun, nullptr, DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC, measuringMode,
                          DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0F, 0.0F,
                          analysis.ReleaseAndGetAddressOf()),
                      "IDWriteFactory3::CreateGlyphRunAnalysis");

        RECT bounds{};
        ThrowIfFailed(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds),
                      "IDWriteGlyphRunAnalysis::GetAlphaTextureBounds");
        const std::uint32_t width = static_cast<std::uint32_t>(
            std::max<LONG>(bounds.right - bounds.left, 0));
        const std::uint32_t height = static_cast<std::uint32_t>(
            std::max<LONG>(bounds.bottom - bounds.top, 0));

        GlyphEntry entry;
        entry.fontFaceIdentity = identity;
        entry.sizePixels = {
            static_cast<float>(width),
            static_cast<float>(height)};
        entry.bearingPixels = {
            static_cast<float>(bounds.left),
            static_cast<float>(bounds.top)};
        entry.drawable = width > 0 && height > 0;
        if (entry.drawable)
        {
            std::vector<std::uint8_t> grayscalePixels(static_cast<std::size_t>(width) * height);
            ThrowIfFailed(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, grayscalePixels.data(),
                                                       static_cast<UINT32>(grayscalePixels.size())),
                          "IDWriteGlyphRunAnalysis::CreateAlphaTexture");

            const std::uint32_t uploadWidth =
                width + GlyphPadding * 2U;
            const std::uint32_t uploadHeight =
                height + GlyphPadding * 2U;
            std::vector<std::uint8_t> coverage(
                static_cast<std::size_t>(uploadWidth) * uploadHeight,
                0U);
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const std::uint8_t alpha = grayscalePixels[static_cast<std::size_t>(y) * width + x];
                    coverage[
                        static_cast<std::size_t>(
                            y + GlyphPadding) * uploadWidth +
                        x + GlyphPadding] = alpha;
                }
            }

            std::uint32_t pageIndex = 0;
            std::uint32_t destinationX = 0;
            std::uint32_t destinationY = 0;
            AllocateAtlasRegion(
                uploadWidth,
                uploadHeight,
                pageIndex,
                destinationX,
                destinationY);
            entry.pageIndex = pageIndex;

            // Padding prevents filtering from sampling an adjacent glyph
            // when the UV rectangle reaches an atlas-cell edge.
            const float left = static_cast<float>(
                destinationX + GlyphPadding);
            const float top = static_cast<float>(
                destinationY + GlyphPadding);
            entry.uvRectangle = {
                left / static_cast<float>(AtlasWidth),
                top / static_cast<float>(AtlasHeight),
                (left + static_cast<float>(width)) /
                    static_cast<float>(AtlasWidth),
                (top + static_cast<float>(height)) /
                    static_cast<float>(AtlasHeight)};
            pendingUploads.push_back(PendingGlyphUpload{
                pageIndex,
                destinationX,
                destinationY,
                uploadWidth,
                uploadHeight,
                std::move(coverage)});
        }

        return glyphCache.emplace(key, std::move(entry)).first->second;
    }

    void AllocateAtlasRegion(
        const std::uint32_t width,
        const std::uint32_t height,
        std::uint32_t& pageIndex,
        std::uint32_t& destinationX,
        std::uint32_t& destinationY)
    {
        if (width > AtlasWidth || height > AtlasHeight)
        {
            throw std::runtime_error(
                "A rasterized glyph is larger than the glyph atlas.");
        }

        for (std::uint32_t index = 0;
             index < atlasPages.size();
             ++index)
        {
            if (TryAllocate(
                    atlasPages[index],
                    width,
                    height,
                    destinationX,
                    destinationY))
            {
                pageIndex = index;
                return;
            }
        }

        if (atlasPages.size() >= DefaultAtlasPages)
        {
            auto candidate = atlasPages.end();
            for (auto page = atlasPages.begin(); page != atlasPages.end(); ++page)
                if (page->inFlightFrames == 0 &&
                    (candidate == atlasPages.end() || page->lastUsedRenderIndex < candidate->lastUsedRenderIndex))
                    candidate = page;
            if (candidate != atlasPages.end())
            {
                pageIndex = static_cast<std::uint32_t>(candidate - atlasPages.begin());
                ErasePageGlyphs(pageIndex);
                candidate->nextX = GlyphPadding;
                candidate->nextY = GlyphPadding;
                candidate->rowHeight = 0;
                if (!TryAllocate(*candidate, width, height, destinationX, destinationY))
                    throw std::runtime_error("A glyph cannot fit an empty atlas page.");
                return;
            }
        }
        // Current and in-flight glyphs may temporarily exceed the soft budget.
        CreateAtlasPage();
        pageIndex =
            static_cast<std::uint32_t>(atlasPages.size() - 1U);
        if (!TryAllocate(
                atlasPages.back(),
                width,
                height,
                destinationX,
                destinationY))
        {
            throw std::runtime_error(
                "Failed to allocate a glyph in a new atlas page.");
        }
    }

    [[nodiscard]] bool TryAllocate(
        AtlasPage& page,
        const std::uint32_t width,
        const std::uint32_t height,
        std::uint32_t& destinationX,
        std::uint32_t& destinationY) noexcept
    {
        if (page.nextX + width + GlyphPadding > AtlasWidth)
        {
            page.nextX = GlyphPadding;
            page.nextY += page.rowHeight + GlyphPadding;
            page.rowHeight = 0;
        }
        if (page.nextY + height + GlyphPadding > AtlasHeight)
        {
            return false;
        }

        destinationX = page.nextX;
        destinationY = page.nextY;
        page.nextX += width + GlyphPadding;
        page.rowHeight = std::max(page.rowHeight, height);
        return true;
    }

    void CreateAtlasPage()
    {
        if (atlasPages.size() >= std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("Too many text atlas pages.");
        EnsureDescriptorCapacity(static_cast<std::uint32_t>(atlasPages.size() + 1));
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = AtlasWidth;
        description.Height = AtlasHeight;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8_UNORM;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        AtlasPage page;
        const D3D12_HEAP_PROPERTIES properties =
            HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(
            device->CreateCommittedResource(
                &properties,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                nullptr,
                IID_PPV_ARGS(page.texture.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(glyph atlas)");

        WriteAtlasDescriptor(page.texture.Get(), atlasPages.size());
        atlasPages.push_back(std::move(page));
    }

    void ErasePageGlyphs(std::uint32_t index)
    {
        std::erase_if(glyphCache, [index](const auto &entry)
                      { return entry.second.drawable && entry.second.pageIndex == index; });
        ++statistics.atlasEvictions;
    }

    void TrimAtlasPages()
    {
        while (atlasPages.size() > DefaultAtlasPages && atlasPages.back().inFlightFrames == 0)
        {
            ErasePageGlyphs(static_cast<std::uint32_t>(atlasPages.size() - 1));
            atlasPages.pop_back();
        }
    }

    void EnsureDescriptorCapacity(std::uint32_t required)
    {
        if (required <= atlasDescriptorCapacity)
            return;
        std::uint32_t capacity = std::max(atlasDescriptorCapacity, DefaultAtlasPages);
        while (capacity < required)
        {
            if (capacity > std::numeric_limits<std::uint32_t>::max() / 2)
                throw std::length_error("Text atlas descriptor capacity overflow.");
            capacity *= 2;
        }
        CreateDescriptorHeap(capacity);
        // Shader-visible heaps cannot be descriptor copy sources. Recreate
        // views from retained resources instead of reading GPU descriptors.
        for (std::size_t index = 0; index < atlasPages.size(); ++index)
            WriteAtlasDescriptor(atlasPages[index].texture.Get(), index);
    }

    void WriteAtlasDescriptor(ID3D12Resource *texture, std::size_t index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        auto descriptor = atlasDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += index * atlasDescriptorSize;
        device->CreateShaderResourceView(texture, &view, descriptor);
    }
    static std::size_t AlignUploadOffset(std::size_t value)
    {
        constexpr std::size_t alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1))
            throw std::length_error("Text upload alignment overflow.");
        return (value + alignment - 1) & ~(alignment - 1);
    }

    FrameInstancePage &AllocateGlyphUpload(std::size_t bytes, std::size_t &offset)
    {
        auto &pages = frameBuffers[currentFrameIndex].glyphUploadPages;
        for (auto &page : pages)
        {
            const auto begin = AlignUploadOffset(page.used);
            if (begin <= page.capacity && bytes <= page.capacity - begin)
            {
                offset = begin;
                page.used = begin + bytes;
                return page;
            }
        }
        std::size_t capacity = 65536;
        while (capacity < bytes)
        {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2)
                throw std::length_error("Text upload capacity overflow.");
            capacity *= 2;
        }
        FrameInstancePage page;
        const auto properties = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = BufferDescription(capacity);
        ThrowIfFailed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(page.resource.GetAddressOf())),
                      "ID3D12Device::CreateCommittedResource(text glyph arena)");
        const D3D12_RANGE noReads{0, 0};
        ThrowIfFailed(page.resource->Map(0, &noReads, reinterpret_cast<void **>(&page.mappedData)),
                      "ID3D12Resource::Map(text glyph arena)");
        page.capacity = capacity;
        page.used = bytes;
        offset = 0;
        pages.push_back(std::move(page));
        ++statistics.uploadAllocations;
        return pages.back();
    }

    void UploadNewGlyphs(ID3D12GraphicsCommandList &commandList)
    {
        if (pendingUploads.empty())
            return;
        std::vector<bool> touchedPages(atlasPages.size());
        std::size_t bytes = 0;
        for (const auto &upload : pendingUploads)
        {
            touchedPages[upload.pageIndex] = true;
            const auto rowPitch = Align(upload.width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const auto size = static_cast<std::size_t>(rowPitch) * upload.height;
            bytes = AlignUploadOffset(bytes);
            if (size > std::numeric_limits<std::size_t>::max() - bytes)
                throw std::length_error("Text glyph upload size overflow.");
            bytes += size;
        }
        std::size_t bufferOffset = 0;
        auto &arena = AllocateGlyphUpload(bytes, bufferOffset);
        statistics.uploadBytes += bytes;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (std::size_t index = 0; index < atlasPages.size(); ++index)
            if (touchedPages[index])
                barriers.push_back(TransitionBarrier(*atlasPages[index].texture.Get(),
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_COPY_DEST));
        commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        std::size_t localOffset = 0;
        for (const auto &upload : pendingUploads)
        {
            localOffset = AlignUploadOffset(localOffset);
            const auto offset = bufferOffset + localOffset;
            const auto rowPitch = Align(upload.width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            for (std::uint32_t row = 0; row < upload.height; ++row)
                std::memcpy(arena.mappedData + offset + static_cast<std::size_t>(row) * rowPitch,
                            upload.pixels.data() + static_cast<std::size_t>(row) * upload.width, upload.width);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = atlasPages[upload.pageIndex].texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = arena.resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset = offset;
            source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, upload.width, upload.height, 1, rowPitch};
            commandList.CopyTextureRegion(&destination, upload.destinationX, upload.destinationY, 0, &source,
                                          nullptr);
            localOffset += static_cast<std::size_t>(rowPitch) * upload.height;
        }
        for (auto &barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
    [[nodiscard]] FrameInstancePage& AllocateInstanceRange(
        FrameInstanceBuffer& frame,
        const std::size_t requiredCapacity,
        std::size_t& firstInstance)
    {
        for (FrameInstancePage& page : frame.pages)
        {
            if (page.capacity - page.used >= requiredCapacity)
            {
                firstInstance = page.used;
                page.used += requiredCapacity;
                return page;
            }
        }

        std::size_t capacity = 256;
        while (capacity < requiredCapacity)
        {
            capacity *= 2;
        }
        const std::size_t sizeBytes =
            capacity * sizeof(GpuGlyphInstance);
        const D3D12_HEAP_PROPERTIES properties =
            HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC description =
            BufferDescription(sizeBytes);
        FrameInstancePage page{};
        ThrowIfFailed(
            device->CreateCommittedResource(
                &properties,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(
                    page.resource.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(text instances)");
        ThrowIfFailed(
            page.resource->Map(
                0,
                nullptr,
                reinterpret_cast<void**>(&page.mappedData)),
            "ID3D12Resource::Map(text instances)");
        page.capacity = capacity;
        page.used = requiredCapacity;
        firstInstance = 0;
        frame.pages.push_back(std::move(page));
        return frame.pages.back();
    }

    void CreateDescriptorHeap(std::uint32_t capacity = DefaultAtlasPages)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = capacity;
        description.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(
            device->CreateDescriptorHeap(
                &description,
                IID_PPV_ARGS(
                    atlasDescriptorHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(text atlas)");
        atlasDescriptorSize =
            device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        atlasDescriptorCapacity = capacity;
    }

    void RequireInitialized() const
    {
        if (!initialized)
        {
            throw std::logic_error(
                "TextRenderSystem is not initialized.");
        }
    }

    void RequireFrameOpen() const
    {
        RequireInitialized();
        if (!frameOpen)
        {
            throw std::logic_error(
                "Text submission requires an open frame.");
        }
    }

    ID3D12Device* device{};
    LayoutCache layouts;
    ComPtr<ID3D12DescriptorHeap> atlasDescriptorHeap;
    TextPipeline pipeline;
    std::uint32_t atlasDescriptorSize{};
    std::vector<AtlasPage> atlasPages;
    std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> glyphCache;
    std::vector<PendingGlyphUpload> pendingUploads;
    std::vector<GlyphInstance> pendingInstances;
    std::array<FrameInstanceBuffer, D3D12Detail::FrameCount> frameBuffers{};
    std::uint32_t currentFrameIndex{};
    std::uint64_t currentRenderIndex{};
    std::uint32_t atlasDescriptorCapacity{};
    TextRenderStatistics statistics{};
    bool initialized{};
    bool frameOpen{};
};

class TextRenderSystem::Impl::LayoutCache::GlyphRunCollector final : public IDWriteTextRenderer
{
public:
  explicit GlyphRunCollector(TextLayout::Impl &owner) noexcept : owner_(owner)
  {
  }
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interfaceId,
        void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId == __uuidof(IUnknown) ||
            interfaceId == __uuidof(IDWritePixelSnapping) ||
            interfaceId == __uuidof(IDWriteTextRenderer))
        {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++referenceCount_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --referenceCount_;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(
        void*,
        BOOL* disabled) override
    {
        if (disabled == nullptr)
        {
            return E_POINTER;
        }
        *disabled = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentTransform(
        void*,
        DWRITE_MATRIX* transform) override
    {
        if (transform == nullptr)
        {
            return E_POINTER;
        }
        *transform = DWRITE_MATRIX{
            1.0F,
            0.0F,
            0.0F,
            1.0F,
            0.0F,
            0.0F};
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(
        void*,
        FLOAT* pixelsPerDip) override
    {
        if (pixelsPerDip == nullptr)
        {
            return E_POINTER;
        }
        *pixelsPerDip = 1.0F;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*,
        const FLOAT baselineOriginX,
        const FLOAT baselineOriginY,
        const DWRITE_MEASURING_MODE measuringMode,
        const DWRITE_GLYPH_RUN* glyphRun,
        const DWRITE_GLYPH_RUN_DESCRIPTION*,
        IUnknown*) override
    {
        if (glyphRun == nullptr)
        {
            return E_INVALIDARG;
        }

        try
        {
            owner_.AppendRun(baselineOriginX, baselineOriginY, measuringMode, *glyphRun);
            return S_OK;
        }
        catch (...)
        {
            error_ = std::current_exception();
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*,
        FLOAT,
        FLOAT,
        const DWRITE_UNDERLINE*,
        IUnknown*) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*,
        FLOAT,
        FLOAT,
        const DWRITE_STRIKETHROUGH*,
        IUnknown*) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void*,
        FLOAT,
        FLOAT,
        IDWriteInlineObject*,
        BOOL,
        BOOL,
        IUnknown*) override
    {
        return E_NOTIMPL;
    }

    [[nodiscard]] const std::exception_ptr& Error() const noexcept
    {
        return error_;
    }

private:
    std::atomic<ULONG> referenceCount_{1};
    TextLayout::Impl &owner_;
    std::exception_ptr error_;
};

void TextRenderSystem::Impl::LayoutCache::Initialize()
{
    ThrowIfFailed(
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory5),
            reinterpret_cast<IUnknown**>(
                directWriteFactory.ReleaseAndGetAddressOf())),
        "DWriteCreateFactory");
}

void TextRenderSystem::Impl::LayoutCache::ClearCaches() noexcept
{
    layoutCache.clear();
    layoutRecency.clear();
    formatCache.clear();
    formatRecency.clear();
}

void TextRenderSystem::Impl::LayoutCache::ReleaseFactory() noexcept
{
    directWriteFactory.Reset();
}

IDWriteFactory5* TextRenderSystem::Impl::LayoutCache::Factory() const noexcept
{
    return directWriteFactory.Get();
}

[[nodiscard]] FontHandle TextRenderSystem::Impl::LayoutCache::LoadSystemFont(
    const std::wstring_view familyName)
{
    if (familyName.empty())
    {
        throw std::invalid_argument(
            "A system font family name cannot be empty.");
    }

    const std::wstring family(familyName);
    ComPtr<IDWriteFontCollection> collection;
    ThrowIfFailed(
        directWriteFactory->GetSystemFontCollection(
            collection.ReleaseAndGetAddressOf()),
        "IDWriteFactory::GetSystemFontCollection");
    UINT32 familyIndex = 0;
    BOOL exists = FALSE;
    ThrowIfFailed(
        collection->FindFamilyName(
            family.c_str(),
            &familyIndex,
            &exists),
        "IDWriteFontCollection::FindFamilyName");
    if (!exists)
    {
        throw std::runtime_error(
            "The requested system font family is unavailable.");
    }

    auto fontImplementation = std::make_shared<Font::Impl>();
    fontImplementation->familyName = family;
    return FontHandle(
        new Font(std::move(fontImplementation)));
}

[[nodiscard]] FontHandle TextRenderSystem::Impl::LayoutCache::LoadFontFile(
    const std::filesystem::path& fontFile)
{
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(fontFile);
    if (!std::filesystem::is_regular_file(absolutePath))
    {
        throw std::runtime_error(
            "Font file was not found: " +
            absolutePath.string());
    }

    ComPtr<IDWriteFontFile> directWriteFontFile;
    ThrowIfFailed(
        directWriteFactory->CreateFontFileReference(
            absolutePath.c_str(),
            nullptr,
            directWriteFontFile.ReleaseAndGetAddressOf()),
        "IDWriteFactory::CreateFontFileReference");

    ComPtr<IDWriteFontSetBuilder1> fontSetBuilder;
    ThrowIfFailed(
        directWriteFactory->CreateFontSetBuilder(
            fontSetBuilder.ReleaseAndGetAddressOf()),
        "IDWriteFactory5::CreateFontSetBuilder");
    ThrowIfFailed(
        fontSetBuilder->AddFontFile(directWriteFontFile.Get()),
        "IDWriteFontSetBuilder1::AddFontFile");

    ComPtr<IDWriteFontSet> fontSet;
    ThrowIfFailed(
        fontSetBuilder->CreateFontSet(
            fontSet.ReleaseAndGetAddressOf()),
        "IDWriteFontSetBuilder::CreateFontSet");

    ComPtr<IDWriteFontCollection1> fontCollection;
    ThrowIfFailed(
        directWriteFactory->CreateFontCollectionFromFontSet(
            fontSet.Get(),
            fontCollection.ReleaseAndGetAddressOf()),
        "IDWriteFactory3::CreateFontCollectionFromFontSet");
    if (fontCollection->GetFontFamilyCount() == 0)
    {
        throw std::runtime_error(
            "The font file contains no font families.");
    }

    ComPtr<IDWriteFontFamily> family;
    ThrowIfFailed(
        fontCollection->GetFontFamily(
            0,
            family.ReleaseAndGetAddressOf()),
        "IDWriteFontCollection::GetFontFamily");
    ComPtr<IDWriteLocalizedStrings> familyNames;
    ThrowIfFailed(
        family->GetFamilyNames(
            familyNames.ReleaseAndGetAddressOf()),
        "IDWriteFontFamily::GetFamilyNames");

    auto fontImplementation = std::make_shared<Font::Impl>();
    fontImplementation->familyName =
        LocalizedString(*familyNames.Get());
    ThrowIfFailed(
        fontCollection.As(&fontImplementation->collection),
        "IDWriteFontCollection1::QueryInterface");
    return FontHandle(
        new Font(std::move(fontImplementation)));
}

ComPtr<IDWriteTextFormat> TextRenderSystem::Impl::LayoutCache::FindOrCreateFormat(const TextDrawCommand &command)
{
    const Font::Impl &font = *command.style.font->implementation_;
    LayoutKey formatKey{command.style.font,
                        {},
                        std::bit_cast<std::uint32_t>(command.style.fontSizePixels),
                        0,
                        0,
                        command.horizontalAlignment,
                        command.verticalAlignment,
                        command.style.fontWeight};
    ComPtr<IDWriteTextFormat> format;
    const auto cachedFormat = formatCache.find(formatKey);
    if (cachedFormat != formatCache.end())
    {
        format = cachedFormat->second.format;
        formatRecency.splice(formatRecency.begin(), formatRecency, cachedFormat->second.recency);
        return format;
    }

    ThrowIfFailed(directWriteFactory->CreateTextFormat(
                      font.familyName.c_str(), font.collection.Get(),
                      static_cast<DWRITE_FONT_WEIGHT>(command.style.fontWeight), DWRITE_FONT_STYLE_NORMAL,
                      DWRITE_FONT_STRETCH_NORMAL, command.style.fontSizePixels, L"ko-kr",
                      format.ReleaseAndGetAddressOf()),
                  "IDWriteFactory::CreateTextFormat");
    ThrowIfFailed(format->SetTextAlignment(ToDirectWrite(command.horizontalAlignment)),
                  "IDWriteTextFormat::SetTextAlignment");
    ThrowIfFailed(format->SetParagraphAlignment(ToDirectWrite(command.verticalAlignment)),
                  "IDWriteTextFormat::SetParagraphAlignment");
    ThrowIfFailed(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "IDWriteTextFormat::SetWordWrapping");

    if (formatCache.size() >= CachedLayoutLimit)
    {
        formatCache.erase(formatRecency.back());
        formatRecency.pop_back();
    }
    formatRecency.push_front(formatKey);
    try
    {
        formatCache.emplace(std::move(formatKey), FormatEntry{format, formatRecency.begin()});
    }
    catch (...)
    {
        formatRecency.pop_front();
        throw;
    }

    return format;
}

TextLayoutHandle TextRenderSystem::Impl::LayoutCache::CompileLayout(std::wstring_view text, const TextDrawCommand &command,
                               const ComPtr<IDWriteTextFormat> &format)
{
    ComPtr<IDWriteTextLayout> layout;
    ThrowIfFailed(directWriteFactory->CreateTextLayout(
                      text.empty() ? L"" : text.data(), static_cast<UINT32>(text.size()), format.Get(),
                      command.layoutSizePixels.x, command.layoutSizePixels.y, layout.ReleaseAndGetAddressOf()),
                  "IDWriteFactory::CreateTextLayout");
    auto prepared = std::make_shared<TextLayout::Impl>();
    prepared->font = command.style.font;
    ComPtr<GlyphRunCollector> collector;
    collector.Attach(new GlyphRunCollector(*prepared));
    const HRESULT result = layout->Draw(nullptr, collector.Get(), 0, 0);
    if (collector->Error())
        std::rethrow_exception(collector->Error());
    ThrowIfFailed(result, "IDWriteTextLayout::Draw");
    DWRITE_TEXT_METRICS metrics{};
    ThrowIfFailed(layout->GetMetrics(&metrics), "IDWriteTextLayout::GetMetrics");
    prepared->size = {metrics.widthIncludingTrailingWhitespace, metrics.height};
    return TextLayoutHandle(new TextLayout(std::move(prepared)));
}

[[nodiscard]] TextLayoutHandle TextRenderSystem::Impl::LayoutCache::FindOrCreateLayout(const std::wstring_view text, const TextDrawCommand &command, TextRenderStatistics& statistics)
{
    const LayoutKeyView lookup{command.style.font.get(),
                               text,
                               std::bit_cast<std::uint32_t>(command.style.fontSizePixels),
                               std::bit_cast<std::uint32_t>(command.layoutSizePixels.x),
                               std::bit_cast<std::uint32_t>(command.layoutSizePixels.y),
                               command.horizontalAlignment,
                               command.verticalAlignment,
                               command.style.fontWeight};
    if (const auto found = layoutCache.find(lookup); found != layoutCache.end())
    {
        layoutRecency.splice(layoutRecency.begin(), layoutRecency, found->second.recency);
        return found->second.layout;
    }
    LayoutKey key{command.style.font, std::wstring(text),         lookup.fontSizeBits,      lookup.widthBits,
                  lookup.heightBits,  lookup.horizontalAlignment, lookup.verticalAlignment, lookup.fontWeight};
    // Keep the working set across frames. Evict one least-recently
    // used layout; clearing everything made 129 static labels thrash.
    if (layoutCache.size() >= CachedLayoutLimit)
    {
        layoutCache.erase(*layoutRecency.back());
        layoutRecency.pop_back();
    }

    auto handle = CompileLayout(text, command, FindOrCreateFormat(command));
    ++statistics.layoutCreations;
    const auto inserted = layoutCache.emplace(std::move(key), LayoutEntry{handle, {}}).first;
    try
    {
        layoutRecency.push_front(&inserted->first);
        inserted->second.recency = layoutRecency.begin();
    }
    catch (...)
    {
        layoutCache.erase(inserted);
        throw;
    }
    return handle;
}

TextRenderSystem::TextRenderSystem()
    : implementation_(std::make_unique<Impl>())
{
}

TextRenderSystem::~TextRenderSystem()
{
    implementation_->Shutdown();
}

void TextRenderingBackend::Initialize(
    TextRenderSystem& text,
    ID3D12Device& device,
    const DXGI_FORMAT renderTargetFormat,
    const DXGI_FORMAT depthStencilFormat)
{
    text.implementation_->Initialize(
        device,
        renderTargetFormat,
        depthStencilFormat);
}

void TextRenderingBackend::Shutdown(TextRenderSystem& text) noexcept
{
    text.implementation_->Shutdown();
}

FontHandle TextRenderSystem::LoadSystemFont(
    const std::wstring_view familyName)
{
    return implementation_->LoadSystemFont(familyName);
}

FontHandle TextRenderSystem::LoadFontFile(
    const std::filesystem::path& fontFile)
{
    return implementation_->LoadFontFile(fontFile);
}

TextLayout::TextLayout(std::shared_ptr<Impl> implementation) : implementation_(std::move(implementation))
{
}
TextLayout::~TextLayout() = default;
DirectX::XMFLOAT2 TextLayout::SizePixels() const noexcept
{
    return implementation_->size;
}

TextLayoutHandle TextRenderSystem::CreateLayout(std::wstring_view text, const TextDrawCommand &command)
{
    return implementation_->CreateLayout(text, command);
}

void TextRenderSystem::Submit(const TextLayoutHandle &layout, const TextDrawCommand &command)
{
    implementation_->Submit(layout, command);
}

TextRenderStatistics TextRenderSystem::Statistics() const noexcept
{
    auto result = implementation_->statistics;
    result.atlasPages = implementation_->atlasPages.size();
    return result;
}

void TextRenderingBackend::BeginFrame(
    TextRenderSystem& text,
    const std::uint32_t frameIndex,
    const std::uint64_t renderIndex)
{
    text.implementation_->BeginFrame(frameIndex, renderIndex);
}

void TextRenderSystem::Submit(
    const std::wstring_view text,
    const TextDrawCommand& command)
{
    implementation_->Submit(text, command);
}

void TextRenderingBackend::Flush(
    TextRenderSystem& text,
    ID3D12GraphicsCommandList& commandList,
    const std::uint32_t viewportWidth,
    const std::uint32_t viewportHeight)
{
    text.implementation_->Flush(
        commandList,
        viewportWidth,
        viewportHeight);
}

void TextRenderingBackend::DrawCanvasBatch(
    TextRenderSystem& text,
    ID3D12GraphicsCommandList& commandList,
    const std::uint32_t viewportWidth,
    const std::uint32_t viewportHeight,
    const std::span<const CanvasDraw> draws)
{
    text.implementation_->DrawCanvasBatch(commandList, viewportWidth, viewportHeight, draws);
}

} // namespace PhantomCore
