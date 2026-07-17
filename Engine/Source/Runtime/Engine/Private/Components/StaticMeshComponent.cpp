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
		SetMaterial(0, InMaterial);
	}

	auto DStaticMeshComponent::SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> void
	{
		if (SlotIndex < Materials.size() && Materials[SlotIndex] == InMaterial) return;
		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene()) Scene->RemovePrimitive(this);
		}
		if (Materials.size() <= SlotIndex) Materials.resize(static_cast<size_t>(SlotIndex) + 1);
		Materials[SlotIndex] = InMaterial;
		if (SlotIndex == 0) Material = InMaterial;
		MarkPackageDirty();
		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene()) Scene->AddPrimitive(this);
		}
	}

	auto DStaticMeshComponent::GetMaterial() const -> DMaterialInterface*
	{
		return GetMaterial(0);
	}

	auto DStaticMeshComponent::GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*
	{
		if (SlotIndex < Materials.size()) return Materials[SlotIndex].Get();
		return SlotIndex == 0 ? Material.Get() : nullptr;
	}

	auto DStaticMeshComponent::GetNumMaterials() const -> uint32
	{
		const FStaticMeshRenderData* RenderData = StaticMesh != nullptr ? StaticMesh->GetRenderData() : nullptr;
		return RenderData != nullptr ? static_cast<uint32>(RenderData->MaterialSlots.size()) : 0;
	}

	auto DStaticMeshComponent::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (Materials.empty() && Material != nullptr) Materials.push_back(Material);
		if (!Materials.empty()) Material = Materials[0];
		return true;
	}

	auto DStaticMeshComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
	{
		if (StaticMesh == nullptr)
		{
			return nullptr;
		}

		FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		if (RenderData == nullptr || RenderData->LODResources.empty() || RenderData->LODResources[0].Indices.empty())
		{
			return nullptr;
		}

		std::vector<FMaterialRenderData> MaterialRenderData;
		MaterialRenderData.reserve(RenderData->MaterialSlots.size());
		for (uint32 SlotIndex = 0; SlotIndex < RenderData->MaterialSlots.size(); ++SlotIndex)
		{
			DMaterialInterface* SlotMaterial = GetMaterial(SlotIndex);
			MaterialRenderData.push_back(SlotMaterial != nullptr ? SlotMaterial->GetRenderData() : FMaterialRenderData{});
		}
		auto Proxy = std::make_unique<FStaticMeshSceneProxy>(RenderData, std::move(MaterialRenderData));
		Proxy->SetTransform(FRHICommandListImmediate::Get(), GetRenderMatrix(), FVector3(0.0));
		return Proxy;
	}
}
