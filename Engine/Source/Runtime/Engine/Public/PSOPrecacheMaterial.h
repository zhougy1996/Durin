#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Materials/MaterialRenderTypes.h"
#include "PSOPrecache.h"
#include "RHIFeatureLevel.h"
#include "PipelineStateCache.h"
#include "SceneRenderPreparationConfig.h"

namespace Durin
{

	// Immutable accepted material snapshot. No DObject or live proxy crosses threads.
	struct FMaterialPSOPrecacheParams
	{
		FMaterialPlanningPassIdentity Material;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
		FPSOPrecacheVertexFactoryData VertexFactoryData;
		FPSOPrecacheParams PrecachePSOParams;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
		FSceneRenderPreparationConfig PreparationConfig;
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
	struct FMaterialPSOPrecacheCollection
	{
		FPSOPrecacheDataArray PSOInitializers;
		// Once set, later collection pumps may append only optional descriptions.
		bool bRequiredPSOsCollected = false;
		std::shared_ptr<void> Resources;
	};
	// Append complete descriptions; the manager consumes entries after admission.
	// Return false while Shader/resource collection is pending. Never submit or wait here.
	using FMaterialPSOCollector = std::function<std::expected<bool, std::string>(
		const FMaterialPSOPrecacheParams&, FMaterialPSOPrecacheCollection&)>;

	using FMaterialPSOSubmitter = std::function<std::expected<std::shared_ptr<FPipelineState>,
		ERHIPipelineRequestRejection>(const FPSOPrecacheData&)>;


	class FMaterialPSORequestManager
	{
	public:
		// Default submission uses the shared device pipeline cache. Hosts may inject
		// another cache without changing collection or request progress semantics.
		ENGINE_API explicit FMaterialPSORequestManager(FMaterialPSOSubmitter Submitter = {});
		ENGINE_API ~FMaterialPSORequestManager();
		FMaterialPSORequestManager(const FMaterialPSORequestManager&) = delete;
		auto operator=(const FMaterialPSORequestManager&) -> FMaterialPSORequestManager& = delete;
		// Thread-safe admission. Resource collection and release are render-thread owned.
		ENGINE_API auto PrecachePSOs(FMaterialPSOPrecacheParams Params) -> FMaterialPSOPrecacheRequestID;
		ENGINE_API auto Pump(const FMaterialPSOCollector& Collector) -> void;
		ENGINE_API auto Invalidate() -> void;
		// Publish currentness on the GameThread; retire resources later on the render thread.
		ENGINE_API auto SetPreparationConfigVersion(uint64 Version) -> void;
		ENGINE_API auto InvalidateOutdatedPreparationConfigs() -> void;
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
	struct FMaterialResource;
	// Desktop Forward and Hybrid use Deferred; material FeatureLevel is independent.
	enum class EShadingPath : uint8 { Mobile, Deferred, Num };
	class IPSOCollector
	{
	public:
		explicit IPSOCollector(int32 InPSOCollectorIndex = -1) : PSOCollectorIndex(InPSOCollectorIndex) {}
		virtual ~IPSOCollector() = default;
		int32 PSOCollectorIndex;
		// Stateless append boundary: the session prepares and retains resources.
		virtual auto CollectPSOInitializers(const FSceneRenderPreparationConfig& SceneConfig,
			const FMaterialResource& Material, const FPSOPrecacheVertexFactoryData& VertexFactoryData,
			const FPSOPrecacheParams& PrecacheParams, FPSOPrecacheDataArray& PSOInitializers) -> void = 0;

	};
	using PSOCollectorCreateFunction = std::unique_ptr<IPSOCollector> (*)(ERHIFeatureLevel);
	class FPSOCollectorCreateManager
	{
	public:
		static constexpr int32 MaxPSOCollectorCount = 64;
		ENGINE_API static auto GetCreateFunction(EShadingPath ShadingPath, int32 Index) -> PSOCollectorCreateFunction;
		ENGINE_API static auto GetIndex(EShadingPath ShadingPath, std::string_view Name) -> int32;
		// Borrowed names and copied create functions require quiescent users at module removal.
		ENGINE_API static auto GetName(EShadingPath ShadingPath, int32 Index) -> std::string_view;
		ENGINE_API static auto GetPSOCollectorCount(EShadingPath ShadingPath) -> int32;
	};
	class FRegisterPSOCollectorCreateFunction
	{
	public:
		ENGINE_API FRegisterPSOCollectorCreateFunction(PSOCollectorCreateFunction Create, EShadingPath ShadingPath, std::string_view Name);
		ENGINE_API ~FRegisterPSOCollectorCreateFunction();
		FRegisterPSOCollectorCreateFunction(const FRegisterPSOCollectorCreateFunction&) = delete;
		auto operator=(const FRegisterPSOCollectorCreateFunction&) -> FRegisterPSOCollectorCreateFunction& = delete;
		auto IsRegistered() const -> bool { return Index >= 0; }
		auto GetIndex() const -> int32 { return Index; }
	private:
		PSOCollectorCreateFunction Create;
		EShadingPath ShadingPath;
		int32 Index = -1;
	};
}
