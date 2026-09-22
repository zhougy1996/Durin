#pragma once

#include <expected>

#include "EngineAPI.h"
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
		Bounds, CollisionMode, CollisionInput, CollisionBuild, Cancelled
	};
	enum class EStaticMeshRecipeKind : uint8 { Render, Collision };
	struct FStaticMeshRecipeError
	{
		EStaticMeshRecipeError Code = EStaticMeshRecipeError::None;
		EStaticMeshRecipeKind Kind = EStaticMeshRecipeKind::Render;
		std::string MeshName;
		std::string SectionName;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 VertexCount = 0;
		uint64 IndexCount = 0;
		FVector3f Position = FVector3f(0);
		FBox Bounds;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		std::optional<FCollisionGeometryBuildDiagnostics> CollisionCause;
	};

	ENGINE_API auto FormatStaticMeshRecipeError(const FStaticMeshRecipeError& Error) -> std::string;

	inline constexpr size_t MaximumStaticMeshBuildDiagnosticBytes = 4096;

	// Worker-local observations; no concurrent access is permitted during an invocation.
	struct FStaticMeshBuildExecutionMetrics
	{
		uint64 CancellationCheckpoints = 0;
	};

	// Borrowed invocation controls. Neither provider nor product may retain these values.
	struct FStaticMeshBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
		FStaticMeshBuildExecutionMetrics* Metrics = nullptr;
		uint64 ExpectedProviderRegistration = 0;
		// Whole-operation reservation. Providers must reject expansion before allocating scratch/products.
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();

		auto IsCancelled() const -> bool
		{
			if (Metrics) ++Metrics->CancellationCheckpoints;
			return ShouldCancel && ShouldCancel();
		}
	};

	// Checked conservative allocation envelope used before recipe and acceleration construction.
	struct FStaticMeshBuildMemoryEstimate
	{
		uint64 Limit;
		uint64 Bytes = 0;
		uint64 RejectedCount = 0;
		uint64 RejectedWidth = 0;
		auto Add(uint64 Count, uint64 Width) -> bool
		{
			if (Width == 0 || Count > (Limit - Bytes) / Width)
			{
				RejectedCount = Count;
				RejectedWidth = Width;
				return false;
			}
			Bytes += Count * Width;
			return true;
		}
	};

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

	// Owns complete CPU streams and metadata; Engine assembles runtime resources.
	struct FStaticMeshRecipeBuildProduct
	{
		std::vector<FStaticMeshBuildLOD> LODs;
		FBox LocalBounds;
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
		static constexpr uint32 FeatureVersion = 6;

		virtual auto GetDescriptor() const -> FStaticMeshBuildProviderDescriptor = 0;
		virtual auto BuildRender(
			const FStaticMeshRecipeBuildRequest& Request,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError> = 0;
		virtual auto BuildCollision(
			const FStaticMeshCollisionRecipeRequest& Request,
			const FStaticMeshBuildExecutionControl& Control = {}) -> std::expected<FStaticMeshCollisionRecipeProduct, FStaticMeshRecipeError> = 0;
	};

}
