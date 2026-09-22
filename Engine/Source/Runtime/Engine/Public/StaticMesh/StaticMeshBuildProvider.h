#pragma once

#include <expected>

#include "EngineAPI.h"
#include "Asset/AssetBuildCacheWarning.h"
#include "Asset/AssetBuildTaskContext.h"
#include "Collision/CollisionGeometry.h"
#include "Modules/ModularFeature.h"
#include "Physics/BodySetupTypes.h"
#include "Physics/PhysicsTypes.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMesh/StaticMeshData.h"

namespace Durin
{
	enum class EStaticMeshRecipeError : uint8
	{
		None, MissingGeometry, VertexLimit, TriangleList, NonFinitePosition, IndexRange,
		WorkingSet, DuplicateMaterial, RenderLimits, MissingMaterial, EmptyGeometry,
		Bounds, Cancelled
	};
	struct FStaticMeshRecipeError
	{
		EStaticMeshRecipeError Code = EStaticMeshRecipeError::None;
		std::string MeshName;
		std::string SectionName;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 VertexCount = 0;
		uint64 IndexCount = 0;
		FVector3f Position = FVector3f(0);
		FBox Bounds;
	};

	ENGINE_API auto FormatStaticMeshRecipeError(const FStaticMeshRecipeError& Error) -> std::string;

	inline constexpr size_t MaximumStaticMeshBuildDiagnosticBytes = 4096;

	struct FStaticMeshBuildProviderDescriptor
	{
		std::string ProducerIdentity;
		uint32 RenderBuilderVersion = 0;

		[[nodiscard]] auto IsValid() const -> bool
		{
			return !ProducerIdentity.empty()
				&& RenderBuilderVersion != 0;
		}
	};

	// Fixed slot metadata only; material object bindings remain with the operation owner.
	struct FStaticMeshRecipeMaterialSlot
	{
		FName Name;
		std::string SourceName;
		uint32 SourceMaterialIndex = 0;
	};

	struct FStaticMeshRecipeBuildRequest
	{
		FStaticMeshGeometryReadHandle Geometry;
		std::span<const FStaticMeshRecipeMaterialSlot> MaterialSlots;
		float NormalizedSize = 1.5f;
	};

	// Owns complete CPU streams and metadata; Engine assembles runtime resources.
	struct FStaticMeshRecipeBuildProduct
	{
		std::vector<FStaticMeshBuildLOD> LODs;
		FBox LocalBounds;
	};

	// Pure StaticMesh render recipe seam.
	class IStaticMeshBuildProvider : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName =
			"Engine.StaticMeshBuildProvider";
		static constexpr uint32 FeatureVersion = 9;

		virtual auto GetDescriptor() const -> FStaticMeshBuildProviderDescriptor = 0;
		virtual auto BuildRender(
			const FStaticMeshRecipeBuildRequest& Request,
			const FAssetBuildTaskContext& Control = {}) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError> = 0;

	};

}
