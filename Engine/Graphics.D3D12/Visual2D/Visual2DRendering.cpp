#include "Visual2D/D3D12Visual2DRenderer.h"
#include "Visual2D/ImageCacheKey.h"
#include "Texture/D3D12TextureRendering.h"
#include "Renderer/D3D12Error.h"
#include "Renderer/D3D12Renderer.h"
#include "Mesh/MeshRendering.h"

#include "Primitive/RectangleShape.h"
#include "Shader/ShaderCompiler.h"
#include "Text/TextRendering.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;


namespace
{
    constexpr std::uint32_t MaximumScreenCanvasZOrder = 31;
    constexpr float ScreenUiDepthRange = 0.10F;
    constexpr float ScreenVisual2DCanvasDepthBand =
        ScreenUiDepthRange /
        static_cast<float>(MaximumScreenCanvasZOrder + 1);

    [[nodiscard]] DirectX::XMFLOAT4 ToFloat4(
        const Color color) noexcept
    {
        return {color.red, color.green, color.blue, color.alpha};
    }

    [[nodiscard]] TextHorizontalAlignment ToTextAlignment(
        const TextAlignment alignment) noexcept
    {
        switch (alignment)
        {
        case TextAlignment::Center:
            return TextHorizontalAlignment::Center;
        case TextAlignment::Trailing:
            return TextHorizontalAlignment::Trailing;
        default:
            return TextHorizontalAlignment::Leading;
        }
    }

    [[nodiscard]] DirectX::XMFLOAT4 ToPixelClipRect(
        const std::optional<Rect>& clipBounds,
        const Size canvasSize,
        const float scaleX,
        const float scaleY,
        const Point screenOrigin,
        const float targetWidth,
        const float targetHeight) noexcept
    {
        if (!clipBounds.has_value())
        {
            return {0.0F, 0.0F, targetWidth, targetHeight};
        }
        const Rect& clip = *clipBounds;
        const float left = screenOrigin.x +
            (canvasSize.width * 0.5F + clip.x) * scaleX;
        const float top = screenOrigin.y +
            (canvasSize.height * 0.5F - clip.y - clip.height) * scaleY;
        return {
            std::clamp(left, 0.0F, targetWidth),
            std::clamp(top, 0.0F, targetHeight),
            std::clamp(
                left + clip.width * scaleX,
                0.0F,
                targetWidth),
            std::clamp(
                top + clip.height * scaleY,
                0.0F,
                targetHeight)};
    }

    [[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(
        const std::size_t byteCount) noexcept
    {
        D3D12_RESOURCE_DESC result{};
        result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        result.Width = byteCount;
        result.Height = 1;
        result.DepthOrArraySize = 1;
        result.MipLevels = 1;
        result.SampleDesc.Count = 1;
        result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return result;
    }
}

struct D3D12Visual2DRenderer::Impl
{
    struct RectangleInstance
    {
        DirectX::XMFLOAT4 bounds{};
        DirectX::XMFLOAT4 color{};
        DirectX::XMFLOAT4X4 transform{};
        DirectX::XMFLOAT4 clipRectPixels{};
        DirectX::XMFLOAT4 roundedRectLocal{};
        float cornerRadiusLocal{};
        std::uint32_t padding[3]{};
    };

    struct VisualInstance
    {
        DirectX::XMFLOAT4 bounds{};
        DirectX::XMFLOAT4 color{};
        DirectX::XMFLOAT4X4 transform{};
        DirectX::XMFLOAT4 uvTransform{};
        DirectX::XMFLOAT4 clipRectPixels{};
        DirectX::XMFLOAT4 roundedRectLocal{};
        float cornerRadiusLocal{};
        std::uint32_t textureIndex{NoTextureIndex};
        std::uint32_t padding[2]{};
    };

    static_assert(sizeof(RectangleInstance) == 144);
    static_assert(sizeof(VisualInstance) == 160);

    struct FrameBufferPage
    {
        ComPtr<ID3D12Resource> resource;
        std::byte* mappedData{};
        std::size_t capacity{};
        std::size_t used{};
    };

    struct FrameUploadArena
    {
        std::vector<FrameBufferPage> pages;
        std::uint64_t renderIndex{};
    };

    struct ImageResource
    {
        std::uint32_t pageIndex{};
        std::uint32_t textureIndex{};
    };

    struct ImagePage
    {
        TextureSetHandle textures;
        MaterialInstanceHandle material;
    };

    struct CanvasDrawing
    {
        Size logicalSize;
        Point origin;
        float scaleX;
        float scaleY;
        std::uint32_t zOrder;
        std::vector<DrawPacket> commands;
    };

    struct CanvasTarget
    {
        ID3D12GraphicsCommandList& commands;
        std::uint32_t frameIndex;
        std::uint32_t width;
        std::uint32_t height;
        std::uint64_t renderIndex;
        bool screen;
    };

    struct FrameRenderTargetLifetime
    {
        std::vector<RenderTargetTextureHandle> targets;
        std::uint64_t renderIndex{};
    };

    ~Impl()
    {
        Shutdown();
    }

    [[nodiscard]] FontHandle ResolveFont(
        TextRenderSystem& rendering,
        const FontHandle& defaultFont,
        std::unordered_map<std::wstring, FontHandle>& cache,
        const TextFont& font)
    {
        if (font.source == TextFontSource::Default ||
            font.value.empty())
        {
            return defaultFont;
        }

        std::wstring key = font.source == TextFontSource::System
            ? L"system:"
            : L"file:";
        key += font.source == TextFontSource::File
            ? std::filesystem::path(font.value).lexically_normal().generic_wstring()
            : font.value;
        std::ranges::transform(
            key,
            key.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        if (const auto found = cache.find(key); found != cache.end())
        {
            return found->second;
        }

        FontHandle loaded = font.source == TextFontSource::System
            ? rendering.LoadSystemFont(font.value)
            : rendering.LoadFontFile(std::filesystem::path(font.value));
        cache.emplace(std::move(key), loaded);
        return loaded;
    }

    struct TextVariant
    {
        float fontSize{};
        DirectX::XMFLOAT2 area{};
        std::uint64_t lastUsed{};
        TextLayoutHandle layout;
    };
    struct RetainedText
    {
        std::weak_ptr<const TextContent> source;
        FontHandle font;
        std::vector<TextVariant> variants;
    };
    using TextCache = std::unordered_map<const TextContent *, RetainedText>;

    void PruneTextCaches(std::uint64_t renderIndex)
    {
        if (textPruneIndex == renderIndex)
            return;
        textPruneIndex = renderIndex;
        const auto expired = [](const auto &item) { return item.second.source.expired(); };
        std::erase_if(screenTextCache, expired);
        std::erase_if(textureTextCache, expired);
    }

    TextLayoutHandle ResolveLayout(TextRenderSystem &rendering, const FontHandle &defaultFont,
                                   std::unordered_map<std::wstring, FontHandle> &fonts, TextCache &cache,
                                   const TextContentHandle &source, TextDrawCommand &command,
                                   DirectX::XMFLOAT2 logicalScale, std::uint64_t renderIndex)
    {
        if (!source)
            throw std::invalid_argument("A Visual2D text packet requires content.");
        auto &retained = cache[source.get()];
        if (retained.source.expired())
        {
            // Publish only after font resolution succeeds, so a failed
            // file/font lookup cannot leave a partially initialized entry.
            auto font = ResolveFont(rendering, defaultFont, fonts, source->font);
            retained = {};
            retained.source = source;
            retained.font = std::move(font);
        }
        command.style.font = retained.font;
        command.horizontalAlignment = ToTextAlignment(source->alignment);
        command.style.fontWeight = source->fontWeight;
        for (auto &variant : retained.variants)
            if (variant.fontSize == command.style.fontSizePixels && variant.area.x == command.layoutSizePixels.x &&
                variant.area.y == command.layoutSizePixels.y)
            {
                variant.lastUsed = renderIndex;
                const auto size = variant.layout->SizePixels();
                source->measuredSize = Size{size.x / logicalScale.x, size.y / logicalScale.y};
                return variant.layout;
            }
        // Keep variants actually used together (e.g. combo header + popup),
        // while replacing obsolete resize variants rather than accumulating them.
        std::erase_if(retained.variants,
                      [renderIndex](const auto &variant) { return variant.lastUsed != renderIndex; });
        auto layout = rendering.CreateLayout(source->value, command);
        retained.variants.push_back({command.style.fontSizePixels, command.layoutSizePixels, renderIndex, layout});
        const auto size = layout->SizePixels();
        source->measuredSize = Size{size.x / logicalScale.x, size.y / logicalScale.y};
        return layout;
    }

    void Initialize(MeshRenderSystem &meshes, TextRenderSystem &text, const DXGI_FORMAT depthStencilFormat)
    {
        if (initialized)
        {
            throw std::logic_error("The D3D12 Visual2D renderer is already initialized.");
        }
        if (meshes.Device() == nullptr)
        {
            throw std::logic_error("The mesh renderer has no D3D12 device.");
        }

        meshRendering = &meshes;
        screenTextRendering = &text;
        device = meshes.Device();

        // Visual2D world matrices scale this mesh directly to the requested
        // pixel/logical Bounds. RectangleShape defaults to 2x2, so use an
        // explicit 1x1 unit quad to keep rendering and hit-test sizes equal.
        const RectangleShape rectangle(1.0F, 1.0F);
        rectangleMesh = meshes.CreateMesh<
            VertexPositionColor>(rectangle);
        imageMesh = meshes.CreateMesh<
            VertexPositionUvColor>(rectangle);
        rectangleMaterial = meshes.CreateMaterial(
            BuiltInMaterial::UnlitVertexColor);
        screenFont = text.LoadSystemFont(L"Segoe UI");

        textureTextRendering.Initialize(
            *device,
            DXGI_FORMAT_R8G8B8A8_UNORM);
        textureFont = textureTextRendering.LoadSystemFont(L"Segoe UI");
        CreateRectanglePipeline(depthStencilFormat);
        CreateVisualPipeline(depthStencilFormat);
        initialized = true;
    }

    void Shutdown() noexcept
    {
        for (FrameUploadArena& arena : rectangleArenas)
        {
            for (FrameBufferPage& page : arena.pages)
            {
                if (page.resource != nullptr && page.mappedData != nullptr)
                {
                    page.resource->Unmap(0, nullptr);
                }
            }
            arena = {};
        }
        for (FrameUploadArena& arena : visualArenas)
        {
            for (FrameBufferPage& page : arena.pages)
            {
                if (page.resource != nullptr && page.mappedData != nullptr)
                {
                    page.resource->Unmap(0, nullptr);
                }
            }
            arena = {};
        }
        screenCanvases.clear();
        screenRectanglePipeline.Reset();
        screenImagePipeline.Reset();
        rectanglePipeline.Reset();
        rectangleRootSignature.Reset();
        visualPipeline.Reset();
        visualRootSignature.Reset();
        textureFonts.clear();
        textureTextCache.clear();
        screenTextCache.clear();
        textPruneIndex = 0;
        textureFont.reset();
        textureTextRendering.Shutdown();
        screenFonts.clear();
        screenFont.reset();
        rectangleMaterial.reset();
        imagesByHandle.clear();
        imageHandlesByPath.clear();
        imagePages.clear();
        renderTargetsInFlight = {};
        nextImageHandle = 1;
        imageMesh.reset();
        rectangleMesh.reset();
        screenTextRendering = nullptr;
        meshRendering = nullptr;
        device = nullptr;
        initialized = false;
    }

    [[nodiscard]] DirectX::XMMATRIX QuadToLocal(
        const Rect bounds) noexcept
    {
        return DirectX::XMMatrixScaling(
                bounds.width,
                bounds.height,
                1.0F) *
            DirectX::XMMatrixTranslation(
                bounds.x + bounds.width * 0.5F,
                bounds.y + bounds.height * 0.5F,
                0.0F);
    }

    [[nodiscard]] Rect TransformBounds(
        const Rect bounds,
        const DirectX::XMFLOAT4X4& transform) noexcept
    {
        const DirectX::XMMATRIX matrix =
            DirectX::XMLoadFloat4x4(&transform);
        const DirectX::XMVECTOR corners[]{
            DirectX::XMVectorSet(bounds.x, bounds.y, 0.0F, 1.0F),
            DirectX::XMVectorSet(
                bounds.x + bounds.width, bounds.y, 0.0F, 1.0F),
            DirectX::XMVectorSet(
                bounds.x, bounds.y + bounds.height, 0.0F, 1.0F),
            DirectX::XMVectorSet(
                bounds.x + bounds.width,
                bounds.y + bounds.height,
                0.0F,
                1.0F)};
        float minimumX = std::numeric_limits<float>::max();
        float minimumY = std::numeric_limits<float>::max();
        float maximumX = std::numeric_limits<float>::lowest();
        float maximumY = std::numeric_limits<float>::lowest();
        for (const DirectX::XMVECTOR corner : corners)
        {
            const DirectX::XMVECTOR transformed =
                DirectX::XMVector3TransformCoord(corner, matrix);
            minimumX = std::min(minimumX, DirectX::XMVectorGetX(transformed));
            minimumY = std::min(minimumY, DirectX::XMVectorGetY(transformed));
            maximumX = std::max(maximumX, DirectX::XMVectorGetX(transformed));
            maximumY = std::max(maximumY, DirectX::XMVectorGetY(transformed));
        }
        return {minimumX, minimumY, maximumX - minimumX, maximumY - minimumY};
    }

    [[nodiscard]] ImageHandle LoadImage(
        const std::filesystem::path& path)
    {
        if (!initialized || meshRendering == nullptr || path.empty())
        {
            throw std::invalid_argument(
                "Loading a Visual2D image requires an initialized renderer and path.");
        }

        const std::filesystem::path normalized = path.lexically_normal();
        const std::wstring cacheKey = Visual2DDetail::ImageCacheKey(normalized);
        if (const auto existing = imageHandlesByPath.find(cacheKey);
            existing != imageHandlesByPath.end())
        {
            return ImageHandle{existing->second};
        }

        const bool requiresNewPage = imagePages.empty() ||
            imagePages.back().textures->Size() >= MaxTexturesPerSet;
        if (requiresNewPage)
        {
            const std::array paths{normalized};
            ImagePage page{};
            page.textures = meshRendering->Textures().LoadTextureSet(paths);
            page.material = meshRendering->CreateMaterial(
                BuiltInMaterial::
                    UnlitVertexColorTextureArrayAlphaBlend);
            page.material->SetTextureSet(page.textures);
            imagePages.push_back(std::move(page));
        }
        else
        {
            ImagePage& page = imagePages.back();
            page.textures = meshRendering->Textures().AppendTexture(
                page.textures,
                normalized);
        }

        const std::uint64_t handleValue = nextImageHandle++;
        const std::uint32_t pageIndex = static_cast<std::uint32_t>(
            imagePages.size() - 1);
        imageHandlesByPath.emplace(cacheKey, handleValue);
        imagesByHandle.emplace(
            handleValue,
            ImageResource{
                pageIndex,
                static_cast<std::uint32_t>(
                    imagePages.back().textures->Size() - 1)});
        return ImageHandle{handleValue};
    }

    [[nodiscard]] Size GetImageSize(
        const ImageHandle image) const noexcept
    {
        const auto found = imagesByHandle.find(image.value);
        if (!image || found == imagesByHandle.end())
        {
            return {};
        }

        const ImageResource& resource = found->second;
        const TextureInfo& info = imagePages[resource.pageIndex].textures->
            Info(resource.textureIndex);
        return {
            static_cast<float>(info.width),
            static_cast<float>(info.height)};
    }

    void CreateScreenPipeline(
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline,
        const DXGI_FORMAT depthStencilFormat,
        const D3D12_COMPARISON_FUNC depthComparison,
        ComPtr<ID3D12PipelineState>& state)
    {
        pipeline.DepthStencilState.DepthEnable = TRUE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = depthComparison;
        pipeline.DSVFormat = depthStencilFormat;
        D3D12Detail::ThrowIfFailed(
            device->CreateGraphicsPipelineState(
                &pipeline,
                IID_PPV_ARGS(state.ReleaseAndGetAddressOf())),
            "Create screen Canvas pipeline");
    }

    void CreateRectanglePipeline(const DXGI_FORMAT depthStencilFormat)
    {
        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.ShaderRegister = 0;
        parameters[1].Constants.Num32BitValues = 2;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = static_cast<UINT>(parameters.size());
        root.pParameters = parameters.data();
        root.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT serializationResult = D3D12SerializeRootSignature(
            &root,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        if (FAILED(serializationResult))
        {
            const std::string details = errors != nullptr
                ? std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize())
                : "No root-signature diagnostics were returned.";
            throw std::runtime_error(
                "UI rectangle root signature failed: " + details);
        }
        D3D12Detail::ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(
                    rectangleRootSignature.ReleaseAndGetAddressOf())),
            "Create UI rectangle root signature");

        const ComPtr<ID3DBlob> vertexShader = CompileShader(
            BuiltInShader::Visual2DRectangle,
            "VSMain",
            "vs_5_1");
        const ComPtr<ID3DBlob> pixelShader = CompileShader(
            BuiltInShader::Visual2DRectangle,
            "PSMain",
            "ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = rectangleRootSignature.Get();
        pipeline.VS = {
            vertexShader->GetBufferPointer(),
            vertexShader->GetBufferSize()};
        pipeline.PS = {
            pixelShader->GetBufferPointer(),
            pixelShader->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC& blend =
            pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.StencilEnable = FALSE;
        pipeline.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        D3D12Detail::ThrowIfFailed(
            device->CreateGraphicsPipelineState(
                &pipeline,
                IID_PPV_ARGS(rectanglePipeline.ReleaseAndGetAddressOf())),
            "Create UI rectangle pipeline");
        CreateScreenPipeline(
            pipeline, depthStencilFormat, D3D12_COMPARISON_FUNC_LESS, screenRectanglePipeline);
    }

    void CreateVisualPipeline(const DXGI_FORMAT depthStencilFormat)
    {
        D3D12_DESCRIPTOR_RANGE textureRange{};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = MaxTexturesPerSet;
        textureRange.BaseShaderRegister = 0;
        textureRange.OffsetInDescriptorsFromTableStart = 0;

        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.ShaderRegister = 0;
        parameters[1].Constants.Num32BitValues = 2;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[2].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &textureRange;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor =
            D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0F;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = static_cast<UINT>(parameters.size());
        root.pParameters = parameters.data();
        root.NumStaticSamplers = 1;
        root.pStaticSamplers = &sampler;
        root.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT serializationResult = D3D12SerializeRootSignature(
            &root,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        if (FAILED(serializationResult))
        {
            const std::string details = errors != nullptr
                ? std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize())
                : "No root-signature diagnostics were returned.";
            throw std::runtime_error(
                "Visual2D image root signature failed: " + details);
        }
        D3D12Detail::ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(visualRootSignature.ReleaseAndGetAddressOf())),
            "Create Visual2D image root signature");

        const ComPtr<ID3DBlob> vertexShader = CompileShader(
            BuiltInShader::Visual2DRectangle,
            "ImageVSMain",
            "vs_5_1");
        const ComPtr<ID3DBlob> pixelShader = CompileShader(
            BuiltInShader::Visual2DRectangle,
            "ImagePSMain",
            "ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = visualRootSignature.Get();
        pipeline.VS = {
            vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        pipeline.PS = {
            pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        D3D12_RENDER_TARGET_BLEND_DESC& blend =
            pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.StencilEnable = FALSE;
        pipeline.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        D3D12Detail::ThrowIfFailed(
            device->CreateGraphicsPipelineState(
                &pipeline,
                IID_PPV_ARGS(visualPipeline.ReleaseAndGetAddressOf())),
            "Create Visual2D image pipeline");
        CreateScreenPipeline(
            pipeline, depthStencilFormat, D3D12_COMPARISON_FUNC_LESS_EQUAL, screenImagePipeline);
    }

    [[nodiscard]] FrameBufferPage& AllocateUploadRange(
        FrameUploadArena& arena,
        const std::uint64_t renderIndex,
        const std::size_t required,
        const std::size_t instanceSize,
        const char* operation,
        std::size_t& firstInstance)
    {
        if (arena.renderIndex != renderIndex)
        {
            arena.renderIndex = renderIndex;
            for (FrameBufferPage& page : arena.pages)
            {
                page.used = 0;
            }
        }

        for (FrameBufferPage& page : arena.pages)
        {
            if (page.capacity - page.used >= required)
            {
                firstInstance = page.used;
                page.used += required;
                return page;
            }
        }

        std::size_t capacity = 64;
        while (capacity < required)
        {
            capacity *= 2;
        }
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC description = BufferDescription(
            capacity * instanceSize);
        FrameBufferPage page{};
        D3D12Detail::ThrowIfFailed(
            device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(page.resource.ReleaseAndGetAddressOf())),
            operation);
        D3D12Detail::ThrowIfFailed(
            page.resource->Map(
                0,
                nullptr,
                reinterpret_cast<void**>(&page.mappedData)),
            "Map Visual2D upload arena page");
        page.capacity = capacity;
        page.used = required;
        firstInstance = 0;
        arena.pages.push_back(std::move(page));
        return arena.pages.back();
    }

    void DrawRectangles(
        const CanvasTarget& target,
        const std::vector<RectangleInstance>& rectangles)
    {
        if (rectangles.empty())
        {
            return;
        }
        auto& commandList = target.commands;
        std::size_t firstInstance = 0;
        FrameBufferPage& page = AllocateUploadRange(
            rectangleArenas[target.frameIndex],
            target.renderIndex,
            rectangles.size(),
            sizeof(RectangleInstance),
            "Create Visual2D rectangle upload arena page",
            firstInstance);
        std::memcpy(
            page.mappedData + firstInstance * sizeof(RectangleInstance),
            rectangles.data(),
            rectangles.size() * sizeof(RectangleInstance));

        commandList.SetGraphicsRootSignature(
            rectangleRootSignature.Get());
        commandList.SetPipelineState(
            (target.screen ? screenRectanglePipeline : rectanglePipeline).Get());
        commandList.IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList.SetGraphicsRootShaderResourceView(
            0,
            page.resource->GetGPUVirtualAddress() +
                firstInstance * sizeof(RectangleInstance));
        const std::array constants{
            std::bit_cast<std::uint32_t>(static_cast<float>(target.width)),
            std::bit_cast<std::uint32_t>(static_cast<float>(target.height))};
        commandList.SetGraphicsRoot32BitConstants(
            1,
            static_cast<UINT>(constants.size()),
            constants.data(),
            0);
        commandList.DrawInstanced(
            6,
            static_cast<UINT>(rectangles.size()),
            0,
            0);
    }

    void DrawVisuals(
        const CanvasTarget& target,
        const TextureSetHandle& textures,
        const std::vector<VisualInstance>& visuals)
    {
        if (visuals.empty() || textures == nullptr)
        {
            return;
        }
        auto& commandList = target.commands;
        std::size_t firstInstance = 0;
        FrameBufferPage& page = AllocateUploadRange(
            visualArenas[target.frameIndex],
            target.renderIndex,
            visuals.size(),
            sizeof(VisualInstance),
            "Create Visual2D image upload arena page",
            firstInstance);
        std::memcpy(
            page.mappedData + firstInstance * sizeof(VisualInstance),
            visuals.data(),
            visuals.size() * sizeof(VisualInstance));

        ID3D12DescriptorHeap* descriptorHeaps[]{
            TextureRenderingBackend::DescriptorHeap(meshRendering->Textures())};
        commandList.SetDescriptorHeaps(1, descriptorHeaps);
        commandList.SetGraphicsRootSignature(visualRootSignature.Get());
        commandList.SetPipelineState(
            (target.screen ? screenImagePipeline : visualPipeline).Get());
        commandList.IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList.SetGraphicsRootShaderResourceView(
            0,
            page.resource->GetGPUVirtualAddress() +
                firstInstance * sizeof(VisualInstance));
        const std::array constants{
            std::bit_cast<std::uint32_t>(static_cast<float>(target.width)),
            std::bit_cast<std::uint32_t>(static_cast<float>(target.height))};
        commandList.SetGraphicsRoot32BitConstants(
            1,
            static_cast<UINT>(constants.size()),
            constants.data(),
            0);
        commandList.SetGraphicsRootDescriptorTable(
            2,
            TextureRenderingBackend::GpuDescriptorStart(meshRendering->Textures(), textures));
        commandList.DrawInstanced(
            6,
            static_cast<UINT>(visuals.size()),
            0,
            0);
    }

    void RecordCanvases(
        const CanvasTarget& target,
        const std::span<const CanvasDrawing> canvases)
    {
        enum class BatchType { None, Rectangles, Images, Text };
        BatchType batch = BatchType::None;
        std::uint32_t imagePage = 0;
        std::vector<RectangleInstance> rectangles;
        std::vector<VisualInstance> images;
        std::vector<TextRenderSystem::CanvasDraw> text;
        const auto flush = [&]()
        {
            switch (batch)
            {
            case BatchType::Rectangles:
                DrawRectangles(target, rectangles);
                rectangles.clear();
                break;
            case BatchType::Images:
                DrawVisuals(target, imagePages[imagePage].textures, images);
                images.clear();
                break;
            case BatchType::Text:
                if (target.screen)
                {
                    screenTextRendering->DrawCanvasBatch(
                        target.commands, target.width, target.height, text);
                }
                else
                {
                    textureTextRendering.BeginFrame(target.frameIndex, target.renderIndex);
                    for (const auto& draw : text)
                    {
                        textureTextRendering.Submit(draw.layout, draw.command);
                    }
                    textureTextRendering.Flush(target.commands, target.width, target.height);
                }
                text.clear();
                break;
            case BatchType::None:
                break;
            }
        };
        const auto selectBatch = [&](const BatchType next, const std::uint32_t page = 0)
        {
            if (batch != next || (next == BatchType::Images && imagePage != page))
            {
                flush();
                batch = next;
                imagePage = page;
            }
        };

        // Preserve the submitted Canvas order and each tree paint sequence.
        PruneTextCaches(target.renderIndex);
        for (const auto& canvas : canvases)
        {
            const Size size = canvas.logicalSize;
            const float scaleX = canvas.scaleX;
            const float scaleY = canvas.scaleY;
            const Point center{
                canvas.origin.x + size.width * scaleX * 0.5F,
                canvas.origin.y + size.height * scaleY * 0.5F};
            const auto pixelScale = DirectX::XMMatrixScaling(scaleX, -scaleY, 0.0001F);
            const float canvasFarDepth = ScreenUiDepthRange -
                static_cast<float>(std::min(canvas.zOrder, MaximumScreenCanvasZOrder)) * ScreenVisual2DCanvasDepthBand;
            const auto depthStep = target.screen
                ? ScreenVisual2DCanvasDepthBand / static_cast<float>(DrawPrimitiveCount(canvas.commands) + 1)
                : 0.0F;
            std::size_t primitiveIndex = 0;
            ForEachDrawPrimitive(canvas.commands, [&](const DrawPacket& command)
            {
                // Screen primitives test against the world at their existing
                // depths. Canvas overlap is resolved by paint order.
                const float depth = target.screen
                    ? canvasFarDepth - depthStep * static_cast<float>(++primitiveIndex)
                    : 0.0F;
                const auto bounds = command.bounds;
                if (bounds.width <= 0.0F || bounds.height <= 0.0F)
                {
                    return;
                }
                const auto clip = ToPixelClipRect(command.clipBounds, size, scaleX, scaleY,
                    canvas.origin, static_cast<float>(target.width), static_cast<float>(target.height));
                if (target.screen)
                {
                    // Skip fully clipped rows before shaping their text.
                    const auto logical = TransformBounds(bounds, command.nodeTransform);
                    const float left = center.x + logical.x * scaleX;
                    const float top = center.y - (logical.y + logical.height) * scaleY;
                    if (left >= clip.z || top >= clip.w || left + logical.width * scaleX <= clip.x ||
                        top + logical.height * scaleY <= clip.y)
                        return;
                }

                const auto localToPixels = DirectX::XMLoadFloat4x4(&command.nodeTransform) * pixelScale;
                if (command.type == DrawPacketType::Text)
                {
                    if (!command.text)
                    {
                        throw std::invalid_argument("A text packet requires content.");
                    }
                    selectBatch(BatchType::Text);
                    TextDrawCommand draw{};
                    draw.positionPixels = {(bounds.x + command.textOffset.x) * scaleX,
                        -(bounds.y + bounds.height + command.textOffset.y) * scaleY};
                    draw.layoutSizePixels = {bounds.width * scaleX, bounds.height * scaleY};
                    draw.depth = depth;
                    DirectX::XMStoreFloat4x4(&draw.transform,
                        DirectX::XMMatrixScaling(1.0F / scaleX, -1.0F / scaleY, 1.0F) *
                        localToPixels * DirectX::XMMatrixTranslation(center.x, center.y, 0.0F));
                    draw.verticalAlignment = TextVerticalAlignment::Center;
                    draw.style.fontSizePixels = command.text->fontSize * std::min(scaleX, scaleY);
                    draw.style.color = ToFloat4(command.color);
                    draw.clipRectPixels = clip;
                    const auto layout = target.screen
                        ? ResolveLayout(*screenTextRendering, screenFont, screenFonts, screenTextCache,
                                        command.text, draw, {scaleX, scaleY}, target.renderIndex)
                        : ResolveLayout(textureTextRendering, textureFont, textureFonts, textureTextCache,
                                        command.text, draw, {scaleX, scaleY}, target.renderIndex);
                    text.push_back({layout, std::move(draw)});
                    return;
                }

                DirectX::XMFLOAT4X4 transform{};
                DirectX::XMStoreFloat4x4(&transform,
                    localToPixels * DirectX::XMMatrixTranslation(center.x, center.y, depth));
                const DirectX::XMFLOAT4 instanceBounds{bounds.x, bounds.y, bounds.width, bounds.height};
                const DirectX::XMFLOAT4 roundedRect{0.0F, 0.0F, bounds.width, bounds.height};
                const float radius = std::min(command.cornerRadius, std::min(bounds.width, bounds.height) * 0.5F);
                if (command.type == DrawPacketType::Rectangle)
                {
                    selectBatch(BatchType::Rectangles);
                    rectangles.push_back({instanceBounds, ToFloat4(command.color), transform, clip, roundedRect, radius});
                    return;
                }
                const auto image = imagesByHandle.find(command.image.value);
                if (image == imagesByHandle.end())
                {
                    return;
                }
                selectBatch(BatchType::Images, image->second.pageIndex);
                images.push_back({instanceBounds, ToFloat4(command.color), transform,
                    {command.uvScale.x, -command.uvScale.y, command.uvOffset.x,
                     command.uvOffset.y + command.uvScale.y},
                    clip, roundedRect, radius, image->second.textureIndex});
            });
        }
        flush();
    }

    MeshRenderSystem* meshRendering{};
    TextRenderSystem* screenTextRendering{};
    ID3D12Device* device{};
    GpuMeshHandle rectangleMesh;
    GpuMeshHandle imageMesh;
    MaterialInstanceHandle rectangleMaterial;
    std::unordered_map<std::uint64_t, ImageResource> imagesByHandle;
    std::unordered_map<std::wstring, std::uint64_t> imageHandlesByPath;
    std::vector<ImagePage> imagePages;
    std::uint64_t nextImageHandle{1};
    FontHandle screenFont;
    std::unordered_map<std::wstring, FontHandle> screenFonts;
    TextRenderSystem textureTextRendering;
    FontHandle textureFont;
    std::unordered_map<std::wstring, FontHandle> textureFonts;
    TextCache screenTextCache;
    TextCache textureTextCache;
    std::uint64_t textPruneIndex{};
    std::vector<CanvasDrawing> screenCanvases;
    std::uint64_t screenRenderIndex{};
    ComPtr<ID3D12PipelineState> screenRectanglePipeline;
    ComPtr<ID3D12PipelineState> screenImagePipeline;
    ComPtr<ID3D12RootSignature> rectangleRootSignature;
    ComPtr<ID3D12PipelineState> rectanglePipeline;
    std::array<FrameUploadArena, D3D12Renderer::FrameCount>
        rectangleArenas{};
    ComPtr<ID3D12RootSignature> visualRootSignature;
    ComPtr<ID3D12PipelineState> visualPipeline;
    std::array<FrameUploadArena, D3D12Renderer::FrameCount> visualArenas{};
    std::array<FrameRenderTargetLifetime, D3D12Renderer::FrameCount>
        renderTargetsInFlight{};
    bool initialized{};
};

D3D12Visual2DRenderer::D3D12Visual2DRenderer()
    : implementation_(std::make_unique<Impl>())
{
}

D3D12Visual2DRenderer::~D3D12Visual2DRenderer() = default;

void D3D12Visual2DRenderer::Initialize(
    MeshRenderSystem& meshRendering,
    TextRenderSystem& textRendering,
    const DXGI_FORMAT depthStencilFormat)
{
    implementation_->Initialize(meshRendering, textRendering, depthStencilFormat);
}

void D3D12Visual2DRenderer::Shutdown() noexcept
{
    implementation_->Shutdown();
}

ImageHandle D3D12Visual2DRenderer::LoadImage(
    const std::filesystem::path& path)
{
    return implementation_->LoadImage(path);
}

Size D3D12Visual2DRenderer::GetImageSize(
    const ImageHandle image) const noexcept
{
    return implementation_->GetImageSize(image);
}

bool D3D12Visual2DRenderer::PrepareImages(std::span<const std::filesystem::path> paths, std::string &error)
{
    if (!IsInitialized())
        throw std::logic_error("Image preparation requires an initialized Visual2D renderer.");
    std::vector<std::filesystem::path> missing;
    for (const auto &path : paths)
        if (!implementation_->imageHandlesByPath.contains(Visual2DDetail::ImageCacheKey(path.lexically_normal())))
            missing.push_back(path);
    return implementation_->meshRendering->Textures().PrepareImages(missing, error);
}

void D3D12Visual2DRenderer::BeginScreenFrame(const std::uint64_t renderIndex)
{
    implementation_->screenCanvases.clear();
    implementation_->screenRenderIndex = renderIndex;
}

void D3D12Visual2DRenderer::SubmitScreen(
    const Visual2DCanvas& canvas,
    const RenderContext& context,
    const Point screenOrigin,
    const std::uint32_t canvasZOrder)
{
    Impl& state = *implementation_;
    if (!IsInitialized() || context.meshRendering != state.meshRendering ||
        context.textRendering != state.screenTextRendering || context.renderIndex != state.screenRenderIndex)
    {
        throw std::logic_error("The D3D12 Visual2D renderer is not initialized for this context.");
    }

    auto commands = canvas.BuildDrawList();
    if (!commands.empty())
    {
        const auto scale = canvas.PixelScale();
        state.screenCanvases.push_back(
            {canvas.LogicalSize(), screenOrigin, scale, scale, canvasZOrder, std::move(commands)});
    }
}

void D3D12Visual2DRenderer::DrawScreen(
    ID3D12GraphicsCommandList& commands,
    const std::uint32_t frameIndex,
    const std::uint32_t width,
    const std::uint32_t height)
{
    Impl& state = *implementation_;
    // Equal Z uses submission order. DrawList already contains tree paint order.
    std::stable_sort(state.screenCanvases.begin(), state.screenCanvases.end(),
        [](const auto& left, const auto& right) { return left.zOrder < right.zOrder; });
    if (!state.screenCanvases.empty())
    {
        state.RecordCanvases({commands, frameIndex, width, height, state.screenRenderIndex, true},
            state.screenCanvases);
        state.screenCanvases.clear();
    }
}

void D3D12Visual2DRenderer::SubmitPlane(
    const Visual2DCanvas& canvas,
    const RenderContext& context,
    const DirectX::XMFLOAT4X4& surfaceWorld,
    const Size surfaceWorldSize,
    const DirectX::XMFLOAT4X4& viewProjection)
{
    Impl& state = *implementation_;
    if (!IsInitialized() || context.meshRendering != state.meshRendering)
    {
        throw std::logic_error(
            "The D3D12 Visual2D renderer is not initialized for this context.");
    }
    if (surfaceWorldSize.width <= 0.0F || surfaceWorldSize.height <= 0.0F)
    {
        throw std::invalid_argument(
            "A world UI plane must have positive size.");
    }

    const Size canvasSize = canvas.LogicalSize();
    const std::vector<DrawPacket> commands = canvas.BuildDrawList();
    const DirectX::XMMATRIX surface =
        DirectX::XMLoadFloat4x4(&surfaceWorld);
    std::size_t primitiveIndex = 0;
    ForEachDrawPrimitive(commands, [&](const DrawPacket &command)
    {
        if (command.bounds.width <= 0.0F || command.bounds.height <= 0.0F)
        {
            return;
        }

        const float layer = -static_cast<float>(primitiveIndex++) * 0.0005F;
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(
            &world,
            state.QuadToLocal(command.bounds) *
            DirectX::XMLoadFloat4x4(&command.nodeTransform) *
            DirectX::XMMatrixScaling(
                surfaceWorldSize.width / canvasSize.width,
                surfaceWorldSize.height / canvasSize.height,
                1.0F) *
            DirectX::XMMatrixTranslation(
                0.0F,
                0.0F,
                layer) *
            surface);

        if (command.type == DrawPacketType::Image)
        {
            const auto image = state.imagesByHandle.find(
                command.image.value);
            if (image == state.imagesByHandle.end())
            {
                return;
            }

            state.meshRendering->Submit(
                state.imageMesh,
                state.imagePages[image->second.pageIndex].material,
                world,
                viewProjection,
                ToFloat4(command.color),
                command.uvScale,
                command.uvOffset,
                image->second.textureIndex);
            return;
        }

        if (command.type != DrawPacketType::Rectangle)
        {
            // Plane text is intentionally omitted. Curved surfaces use the
            // render-to-texture path below so glyphs can follow the mesh UVs.
            return;
        }

        state.meshRendering->Submit(
            state.rectangleMesh,
            state.rectangleMaterial,
            world,
            viewProjection,
            ToFloat4(command.color),
            {1.0F, 1.0F},
            {0.0F, 0.0F},
            NoTextureIndex);
    });
}

RenderTargetTextureHandle D3D12Visual2DRenderer::CreateCanvasRenderTarget(
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (!IsInitialized())
    {
        throw std::logic_error(
            "The D3D12 Visual2D renderer must be initialized first.");
    }
    return implementation_->meshRendering->Textures().
        CreateRenderTargetTexture(width, height);
}

void D3D12Visual2DRenderer::RenderToTexture(
    const Visual2DCanvas& canvas,
    const RenderTargetTextureHandle& target,
    const RenderContext& context)
{
    Impl& state = *implementation_;
    const auto* frame = RenderFrameContext::Find(context);
    if (!IsInitialized() || target == nullptr || frame == nullptr ||
        frame->commandList == nullptr ||
        context.frameIndex >= D3D12Renderer::FrameCount)
    {
        throw std::invalid_argument(
            "The Canvas texture pass received an invalid context.");
    }

    // A transient Scene may release its target immediately after this
    // frame is recorded. Retain it until this frame-ring slot is reused,
    // which happens only after D3D12Renderer has waited for its fence.
    Impl::FrameRenderTargetLifetime& retainedTargets =
        state.renderTargetsInFlight[context.frameIndex];
    if (retainedTargets.renderIndex != context.renderIndex)
    {
        retainedTargets.renderIndex = context.renderIndex;
        retainedTargets.targets.clear();
    }
    retainedTargets.targets.push_back(target);

    ID3D12GraphicsCommandList& commandList = *frame->commandList;
    TextureManager& textureManager = state.meshRendering->Textures();
    TextureRenderingBackend::BeginRenderTargetPass(
        textureManager, commandList,
        target,
        {0.0F, 0.0F, 0.0F, 0.0F});
    const std::uint32_t targetWidth = target->Width();
    const std::uint32_t targetHeight = target->Height();

    const Size canvasSize = canvas.LogicalSize();
    const Impl::CanvasDrawing drawing{canvasSize, {},
        static_cast<float>(targetWidth) / canvasSize.width,
        static_cast<float>(targetHeight) / canvasSize.height, 0, canvas.BuildDrawList()};
    state.RecordCanvases({commandList, context.frameIndex, targetWidth, targetHeight, context.renderIndex, false},
        std::span{&drawing, 1});

    TextureRenderingBackend::EndRenderTargetPass(textureManager, commandList, target);

    commandList.RSSetViewports(1, &frame->viewport);
    commandList.RSSetScissorRects(1, &frame->scissorRectangle);
    commandList.OMSetRenderTargets(1, &frame->renderTargetView, FALSE, &frame->depthStencilView);
}

bool D3D12Visual2DRenderer::IsInitialized() const noexcept
{
    return implementation_ != nullptr && implementation_->initialized;
}
