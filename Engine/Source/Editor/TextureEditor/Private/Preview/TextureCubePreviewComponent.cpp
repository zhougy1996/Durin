#include "Preview/TextureCubePreviewComponent.h"

#include "Engine/FPrimitiveSceneProxy.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	DTextureCubePreviewComponent::DTextureCubePreviewComponent(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DTextureCubePreviewComponent::SetTextureCube(DTextureCube* InTextureCube) -> void
	{
		if (TextureCube.Get() == InTextureCube) return;
		TextureCube = InTextureCube;
		MarkRenderStateDirty();
	}

	auto DTextureCubePreviewComponent::CreateSceneProxy()
		-> std::unique_ptr<FPrimitiveSceneProxy>
	{
		DStaticMesh* Mesh = GetStaticMesh();
		if (Mesh == nullptr || TextureCube == nullptr)
			return nullptr;

		Mesh->InitResources();
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (RenderData == nullptr || RenderData->LODResources.empty())
			return nullptr;

		FRHITextureReferenceRef TextureReference = TextureCube->GetTextureReferenceRHI();
		if (TextureReference == nullptr) return nullptr;
		return std::make_unique<FTextureCubePreviewSceneProxy>(
			RenderData, std::move(TextureReference));
	}
} // namespace Durin
