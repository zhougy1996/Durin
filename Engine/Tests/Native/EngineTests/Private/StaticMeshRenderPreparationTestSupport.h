#pragma once

#include "CoreMinimal.h"
#include <gtest/gtest.h>
#include "Asset/AssetCompilingManager.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin::Tests::Preparation
{
	inline auto MakeMaterial(Durin::EMaterialBlendMode BlendMode, bool bTwoSided = false, Durin::EMaterialDepthWritePolicy DepthWrite = Durin::EMaterialDepthWritePolicy::Automatic)
		-> Durin::FMaterialRenderProxyRef
	{
		const char* Name = BlendMode == Durin::EMaterialBlendMode::Masked
			? "PreparationMaskedMaterial"
			: (BlendMode == Durin::EMaterialBlendMode::Translucent
				? "PreparationTranslucentMaterial"
				: (bTwoSided ? "PreparationTwoSidedMaterial"
					: "PreparationOpaqueMaterial"));
		static Durin::FObjectKey RootHandle;
		auto* Root = Durin::Cast<Durin::DMaterial>(Durin::ResolveObjectKey(RootHandle));
		if (!Durin::IsValid(Root))
		{
			Root = Durin::NewObject<Durin::DMaterial>(nullptr, "PreparationVariantRoot");
			RootHandle = Durin::FObjectKey(Root);
		}
		// Earlier cases may have enabled asynchronous compilation. Capture only
		// accepted material programs, regardless of the case execution order.
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Root);
		auto* Material = Durin::NewObject<Durin::DMaterialInstance>(nullptr, Name);
		Durin::FMaterialPropertyOverrides Overrides;
		Overrides.bOverrideBlendMode = true;
		Overrides.bOverrideOpacityMaskThreshold = true;
		Overrides.bOverrideTwoSided = true;
		Overrides.bOverrideDepthWritePolicy = true;
		Overrides.Values.BlendMode = BlendMode;
		Overrides.Values.bTwoSided = bTwoSided;
		Overrides.Values.DepthWritePolicy = DepthWrite;
		Overrides.Values.OpacityMaskThreshold = 0.4f;
		EXPECT_TRUE(Material->SetParentAndPropertyOverrides(Root, Overrides));
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
		return Material->GetMaterialRenderProxy();
	}

	inline auto MakeRenderData() -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		auto Result = std::make_unique<Durin::FStaticMeshRenderData>();
		Result->MaterialSlots.resize(4);
		for (uint32 Index = 0; Index < 4; ++Index)
		{
			Result->MaterialSlots[Index].Name = std::format("Section{}", Index);
			Result->MaterialSlots[Index].SourceMaterialIndex = Index;
		}
		Result->LODResources.resize(1);
		auto& LOD = Result->LODResources[0];
		const std::vector<Durin::FVector3f> Positions{
			{-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}
		};
		LOD.VertexBuffers.PositionVertexBuffer.Init(Positions);
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<Durin::FVector3f>(3, {0.0f, 0.0f, 1.0f}),
			std::vector<Durin::FVector4f>(3, {1.0f, 0.0f, 0.0f, 1.0f})
		);
		std::array<std::vector<Durin::FVector2f>, Durin::MaxStaticMeshUVChannels> TexCoords;
		for (auto& Channel : TexCoords)
		{
			Channel.resize(3);
		}
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			std::move(TexCoords), 3, 1
		);
		LOD.VertexBuffers.ColorVertexBuffer.Init(
			std::vector<Durin::FVector4f>(3, Durin::FVector4f(1.0f)), 3
		);
		LOD.IndexBuffer.Init({0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2});
		for (uint32 SectionIndex = 0; SectionIndex < 4; ++SectionIndex)
		{
			LOD.Sections.push_back({.Name = std::format("Section{}", SectionIndex), .FirstIndex = SectionIndex * 3, .IndexCount = 3, .MinVertexIndex = 0, .MaxVertexIndex = 2, .MaterialSlotIndex = SectionIndex, .LocalBounds = Durin::FBox(Durin::FVector3(-1.0, -1.0, static_cast<double>(SectionIndex)), Durin::FVector3(1.0, 1.0, static_cast<double>(SectionIndex)))});
		}
		LOD.NumTexCoords = 1;
		LOD.bHasColorVertexData = true;
		Result->LODVertexFactories.resize(1);
		Result->RecalculateBounds();
		return Result;
	}

	struct FInitializePreparedStaticMeshResourcesCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "InitializePreparedStaticMeshResources";
		}
	};
	struct FCapturePreparedStaticMeshViewCommand
	{
		static constexpr auto GetName() -> const char* { return "CapturePreparedStaticMeshView"; }
	};

}
