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
		auto operator==(const FMaterialPSOPrecacheProgress&) const -> bool = default;
	};
	struct FMaterialPSOPrecacheWork;
	struct FMaterialPSOPrecacheObserver;
	class FMaterialPSORequestManager;
	struct FMaterialPSOPrecacheNotification;
	using FMaterialPSOPrecacheSubscription = std::shared_ptr<const FMaterialPSOPrecacheNotification>;
	class FMaterialPSOPrecacheOwner;

	// Copies alias one observer. Separate requests share work but cancel independently.
	class FMaterialPSOPrecacheRequestID
	{
	public:
		ENGINE_API static auto Rejected(std::string Error) -> FMaterialPSOPrecacheRequestID;
		ENGINE_API auto GetValue() const -> uint64;
		auto IsValid() const -> bool { return GetValue() != 0; }
		// Outcome diagnostics survive invalidation; currentness controls reuse.
		ENGINE_API auto IsCurrent() const -> bool;
		ENGINE_API auto GetProgress() const -> FMaterialPSOPrecacheProgress;
		ENGINE_API auto GetDiagnostic() const -> std::string;
		ENGINE_API auto Cancel() const -> void;
		ENGINE_API auto BoostPriority(EPSOPrecachePriority Priority) const -> void;
		ENGINE_API auto GetPriority() const -> EPSOPrecachePriority;
		// Callbacks may run on the resource thread; subscriptions never own a component.
		ENGINE_API auto Subscribe(std::function<void()> Changed) const -> FMaterialPSOPrecacheSubscription;
	private:
		std::shared_ptr<FMaterialPSOPrecacheObserver> Observer;
		friend class FMaterialPSORequestManager;
		friend class FMaterialPSOPrecacheOwner;
	};

	// GameThread owner. Invalidates only its observations, preserving shared work.
	class FMaterialPSOPrecacheOwner
	{
	public:
		ENGINE_API FMaterialPSOPrecacheOwner();
		ENGINE_API ~FMaterialPSOPrecacheOwner();
		FMaterialPSOPrecacheOwner(const FMaterialPSOPrecacheOwner&) = delete;
		auto operator=(const FMaterialPSOPrecacheOwner&) -> FMaterialPSOPrecacheOwner& = delete;
		ENGINE_API auto Observe(const FMaterialPSOPrecacheRequestID& Request) -> FMaterialPSOPrecacheRequestID;
		ENGINE_API auto Invalidate() -> void;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};

	struct FMaterialPSOPrecachePipeline
	{
		std::shared_ptr<FPipelineState> Pipeline;
		bool bRequired = true;
		// Preserve terminal progress after releasing the native pipeline observation.
		ERHIPipelineRequestState CompletedState = ERHIPipelineRequestState::Failed;
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
		ENGINE_API auto GetPendingRequestCount() const -> size_t;
		static constexpr size_t MaximumActiveCollections = 8;
		static constexpr size_t MaximumPipelinesPerRequest = 64;
		static constexpr uint64 MaximumCodeBytes = 64 * 1024 * 1024;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
