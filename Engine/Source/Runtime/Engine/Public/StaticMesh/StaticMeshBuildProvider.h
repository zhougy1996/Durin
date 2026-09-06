#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "Physics/BodySetup.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	// Detached build outcome, independent of request supersession and resource readiness.
	enum class EStaticMeshBuildStatus : uint8
	{
		Succeeded,
		Failed,
		Cancelled
	};

	inline constexpr size_t MaximumStaticMeshBuildDiagnosticBytes = 4096;

	// Bounded operation diagnostic; products remain separately owned by the caller.
	struct FStaticMeshBuildOutcome
	{
		EStaticMeshBuildStatus Status = EStaticMeshBuildStatus::Failed;
		std::string Diagnostic;

		FStaticMeshBuildOutcome(EStaticMeshBuildStatus InStatus, std::string_view InDiagnostic = {})
			: Status(InStatus), Diagnostic(InDiagnostic.substr(0, MaximumStaticMeshBuildDiagnosticBytes)) {}

		explicit operator bool() const { return Status == EStaticMeshBuildStatus::Succeeded; }
	};

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
		auto Add(uint64 Count, uint64 Width) -> bool
		{
			if (Width == 0 || Count > (Limit - Bytes) / Width) return false;
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
		static constexpr uint32 FeatureVersion = 2;

		virtual auto GetDescriptor() const -> FStaticMeshBuildProviderDescriptor = 0;
		virtual auto BuildRender(
			const FStaticMeshRecipeBuildRequest& Request,
			FStaticMeshRecipeBuildProduct& OutProduct,
			std::string& OutError,
			const FStaticMeshBuildExecutionControl& Control = {}) -> FStaticMeshBuildOutcome = 0;
		virtual auto BuildCollision(
			const FStaticMeshCollisionRecipeRequest& Request,
			FStaticMeshCollisionRecipeProduct& OutProduct,
			std::string& OutError,
			const FStaticMeshBuildExecutionControl& Control = {}) -> FStaticMeshBuildOutcome = 0;
	};

}
