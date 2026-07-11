#include "Components/StaticMeshComponent.h"

#include "Engine/Engine.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
#include "Materials/MaterialInterface.h"
#include "RHICommandList.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	auto DStaticMeshComponent::SetStaticMesh(DStaticMesh* InStaticMesh) -> void
	{
		if (StaticMesh == InStaticMesh)
		{
			return;
		}

		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene())
			{
				Scene->RemovePrimitive(this);
			}
		}

		StaticMesh = InStaticMesh;
		MarkPackageDirty();

		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene())
			{
				Scene->AddPrimitive(this);
			}
		}
	}

	auto DStaticMeshComponent::GetStaticMesh() const -> DStaticMesh*
	{
		return StaticMesh.Get();
	}

	auto DStaticMeshComponent::SetMaterial(DMaterialInterface* InMaterial) -> void
	{
		if (Material == InMaterial) return;
		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene()) Scene->RemovePrimitive(this);
		}
		Material = InMaterial;
		MarkPackageDirty();
		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene()) Scene->AddPrimitive(this);
		}
	}

	auto DStaticMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return Material.Get();
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
	{
		if (StaticMesh == nullptr)
		{
			return nullptr;
		}

		FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (RenderData == nullptr || RenderData->IndexCount == 0)
		{
			return nullptr;
		}

		const FMaterialRenderData MaterialRenderData = Material != nullptr ? Material->GetRenderData() : FMaterialRenderData{};
		auto Proxy = std::make_unique<FStaticMeshSceneProxy>(RenderData, MaterialRenderData);
		Proxy->SetTransform(FRHICommandListImmediate::Get(), GetRenderMatrix(), FVector3(0.0));
		return Proxy;
	}
}
