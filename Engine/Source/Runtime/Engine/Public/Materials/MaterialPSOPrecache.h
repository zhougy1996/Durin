#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Materials/MaterialRenderTypes.h"
#include "PSOPrecache.h"
#include "PipelineStateCache.h"

namespace Durin
{
	enum class EMaterialPSOPrecachePass : uint8
	{
		None = 0, Forward = 1, ShadowDepth = 2, GBuffer = 4,
		HybridRetainedForward = 8, SortedTranslucency = 16, HitProxy = 32,
	};
	ENUM_CLASS_FLAGS(EMaterialPSOPrecachePass);

	struct FPSOPrecacheParams
	{
		EMaterialPSOPrecachePass Passes = EMaterialPSOPrecachePass::Forward;
		EMaterialPSOPrecachePass RequiredPasses = EMaterialPSOPrecachePass::Forward;
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
		bool bWireframe = false, bReverseCulling = false, bOrientationChanging = false, bReversedZ = false;
		auto operator==(const FPSOPrecacheParams&) const -> bool = default;
	};

	// Immutable accepted material snapshot. No DObject or live proxy crosses threads.
	struct FMaterialPSOPrecacheParams
	{
		FMaterialPlanningPassIdentity Material;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
		FPSOPrecacheVertexFactoryData VertexFactoryData;
		FPSOPrecacheParams PrecachePSOParams;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
	};

	enum class EMaterialPSOPrecacheState : uint8 { Pending, Ready, Failed, Canceled, Stale };
	struct FMaterialPSOPrecacheProgress
	{
		EMaterialPSOPrecacheState State = EMaterialPSOPrecacheState::Failed;
		uint32 Pipelines = 0, Pending = 0, RequiredPending = 0, Failed = 0;
		bool bRequiredReady = false;
	};
	struct FMaterialPSOPrecacheWork;
	struct FMaterialPSOPrecacheObserver;
	class FMaterialPSORequestManager;

	// Copies alias one observer. Separate requests share work but cancel independently.
	class FMaterialPSOPrecacheRequestID
	{
	public:
		ENGINE_API static auto Rejected(std::string Error) -> FMaterialPSOPrecacheRequestID;
		ENGINE_API auto GetValue() const -> uint64;
		auto IsValid() const -> bool { return GetValue() != 0; }
		ENGINE_API auto GetProgress() const -> FMaterialPSOPrecacheProgress;
		ENGINE_API auto GetDiagnostic() const -> std::string;
		ENGINE_API auto Cancel() const -> void;
		ENGINE_API auto BoostPriority(EPSOPrecachePriority Priority) const -> void;
		ENGINE_API auto GetPriority() const -> EPSOPrecachePriority;
		ENGINE_API auto WithOwnerRevision(const std::shared_ptr<std::atomic<uint64>>& Revision) const -> FMaterialPSOPrecacheRequestID;
	private:
		std::shared_ptr<FMaterialPSOPrecacheObserver> Observer;
		friend class FMaterialPSORequestManager;
	};

	struct FMaterialPSOPrecachePipeline
	{
		std::shared_ptr<FPipelineState> Pipeline;
		bool bRequired = true;
	};
	struct FMaterialPSOPrecacheCollection
	{
		std::vector<FMaterialPSOPrecachePipeline> Pipelines;
		std::shared_ptr<void> Resources;
	};
	// Return false while Shader/resource collection is pending. Never wait for PSOs.
	using FMaterialPSOCollector = std::function<std::expected<bool, std::string>(
		const FMaterialPSOPrecacheParams&, FMaterialPSOPrecacheCollection&)>;

	class FMaterialPSORequestManager
	{
	public:
		ENGINE_API FMaterialPSORequestManager();
		ENGINE_API ~FMaterialPSORequestManager();
		FMaterialPSORequestManager(const FMaterialPSORequestManager&) = delete;
		auto operator=(const FMaterialPSORequestManager&) -> FMaterialPSORequestManager& = delete;
		// Thread-safe admission. Resource collection and release are render-thread owned.
		ENGINE_API auto PrecachePSOs(FMaterialPSOPrecacheParams Params) -> FMaterialPSOPrecacheRequestID;
		ENGINE_API auto Pump(const FMaterialPSOCollector& Collector) -> void;
		ENGINE_API auto Invalidate() -> void;
		// Close before backend/module shutdown, on the resource-owning thread.
		ENGINE_API auto Close() -> void;
		static constexpr size_t MaximumRequests = 256;
		static constexpr size_t MaximumPipelinesPerRequest = 64;
		static constexpr uint64 MaximumCodeBytes = 64 * 1024 * 1024;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
