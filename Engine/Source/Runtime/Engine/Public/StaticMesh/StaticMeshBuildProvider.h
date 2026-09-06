#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	struct FStaticMeshBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 RenderBuilderVersion = 0;
		uint32 CollisionBuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty()
				&& RenderBuilderVersion != 0
				&& CollisionBuilderVersion != 0;
		}
	};

	// Stable slot metadata only; Engine retains and restores material bindings.
	struct FStaticMeshRecipeMaterialSlot
	{
		FName Name;
		std::string SourceName;
		uint32 SourceMaterialIndex = 0;
	};

	struct FStaticMeshRecipeBuildRequest
	{
		FStaticMeshGeometryReadHandle Geometry;
		std::span<const FStaticMeshRecipeMaterialSlot> PreviousMaterialSlots;
		float NormalizedSize = 1.5f;
	};

	struct FStaticMeshRecipeBuildProduct
	{
		std::unique_ptr<FStaticMeshRenderData> RenderData;
		std::vector<FStaticMeshRecipeMaterialSlot> MaterialSlots;
		bool bSlotMetadataChanged = false;
	};

	struct FStaticMeshCollisionRecipeRequest
	{
		std::span<const FVector3f> Positions;
		std::span<const uint32> Indices;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy Policy =
			EBodySetupCollisionQueryPolicy::SimpleAndComplex;
	};

	struct FStaticMeshCollisionRecipeProduct
	{
		FCollisionGeometryRef Geometry;
	};

	// Pure StaticMesh render and collision recipe seam.
	class IStaticMeshBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.StaticMeshBuildProvider";
		static constexpr uint32 FeatureVersion = 1;

		virtual auto GetDescriptor() const -> FStaticMeshBuildProviderDescriptor = 0;
		virtual auto BuildRender(
			const FStaticMeshRecipeBuildRequest& Request,
			FStaticMeshRecipeBuildProduct& OutProduct,
			std::string& OutError) -> bool = 0;
		virtual auto BuildCollision(
			const FStaticMeshCollisionRecipeRequest& Request,
			FStaticMeshCollisionRecipeProduct& OutProduct,
			std::string& OutError) -> bool = 0;
	};

}
