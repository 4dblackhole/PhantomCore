#include "System/MeshInstance.h"

#include <stdexcept>
#include <utility>

namespace PhantomCore
{
using namespace DirectX;


MeshInstance::MeshInstance(
    GpuMeshHandle mesh,
    MaterialInstanceHandle material)
{
    SetMesh(std::move(mesh));
    SetMaterial(std::move(material));
}

void MeshInstance::SetMesh(GpuMeshHandle mesh)
{
    if (mesh == nullptr)
    {
        throw std::invalid_argument(
            "A MeshInstance mesh cannot be null.");
    }
    mesh_ = std::move(mesh);
}

void MeshInstance::SetMaterial(
    MaterialInstanceHandle material)
{
    if (material == nullptr)
    {
        throw std::invalid_argument(
            "A MeshInstance material cannot be null.");
    }
    material_ = std::move(material);
}

void MeshInstance::Reset() noexcept
{
    material_.reset();
    mesh_.reset();
    color_ = {1.0F, 1.0F, 1.0F, 1.0F};
    uvScale_ = {1.0F, 1.0F};
    uvOffset_ = {0.0F, 0.0F};
    textureIndex_ = NoTextureIndex;
}

const GpuMeshHandle& MeshInstance::Mesh() const noexcept
{
    return mesh_;
}

const MaterialInstanceHandle&
    MeshInstance::Material() const noexcept
{
    return material_;
}

TransformNode& MeshInstance::Transform() noexcept
{
    return transform_;
}

const TransformNode& MeshInstance::Transform() const noexcept
{
    return transform_;
}

void MeshInstance::SetColor(const Float4& color) noexcept
{
    color_ = color;
}

const Float4& MeshInstance::Color() const noexcept
{
    return color_;
}

void MeshInstance::SetTextureIndex(
    const std::uint32_t textureIndex) noexcept
{
    textureIndex_ = textureIndex;
}

void MeshInstance::ClearTexture() noexcept
{
    textureIndex_ = NoTextureIndex;
    uvScale_ = {1.0F, 1.0F};
    uvOffset_ = {0.0F, 0.0F};
}

std::uint32_t MeshInstance::TextureIndex() const noexcept
{
    return textureIndex_;
}

void MeshInstance::SetUvTransform(
    const Float2& scale,
    const Float2& offset) noexcept
{
    uvScale_ = scale;
    uvOffset_ = offset;
}

void MeshInstance::SetUvTransform(
    const UvTransform& transform) noexcept
{
    SetUvTransform(transform.scale, transform.offset);
}

const Float2& MeshInstance::UvScale() const noexcept
{
    return uvScale_;
}

const Float2& MeshInstance::UvOffset() const noexcept
{
    return uvOffset_;
}

bool MeshInstance::IsReady() const noexcept
{
    return mesh_ != nullptr && material_ != nullptr;
}

const Sphere3D&
MeshInstance::LocalBoundingSphere() const
{
    if (mesh_ == nullptr)
    {
        throw std::logic_error(
            "MeshInstance must have a mesh before reading its bounds.");
    }
    return mesh_->LocalBoundingSphere();
}

Sphere3D MeshInstance::WorldBoundingSphere() const
{
    return TransformSphere(
        LocalBoundingSphere(),
        transform_.WorldMatrix());
}

bool MeshInstance::IsVisible(const Camera& camera) const
{
    return mesh_ != nullptr &&
        Classify(
            camera.Frustum(),
            WorldBoundingSphere()) !=
            VolumeIntersection::Outside;
}

void MeshInstance::Submit(
    const RenderContext& context,
    const Camera& camera)
{
    if (!IsReady())
    {
        throw std::logic_error(
            "MeshInstance must have a mesh and material before Submit.");
    }
    if (context.meshRendering == nullptr)
    {
        throw std::logic_error(
            "RenderContext has no mesh rendering service.");
    }

    const Float4x4& world = transform_.WorldMatrix();
    if (Classify(
            camera.Frustum(),
            TransformSphere(
                mesh_->LocalBoundingSphere(),
                world)) == VolumeIntersection::Outside)
    {
        return;
    }

    const Float4x4 viewProjection = camera.ViewProjectionMatrix();
    context.meshRendering->Submit(
        mesh_,
        material_,
        world,
        viewProjection,
        color_,
        uvScale_,
        uvOffset_,
        textureIndex_);
}

} // namespace PhantomCore
