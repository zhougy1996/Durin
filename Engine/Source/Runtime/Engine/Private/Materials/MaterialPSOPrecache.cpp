#include "Materials/MaterialPSOPrecache.h"
#include "RenderResourcePreparation.h"
#include "CoreGlobals.h"
#include "RenderingThread.h"

#include "Shader/ShaderCompilerCore.h"
#include "Materials/MaterialShaderMap.h"

namespace Durin
{
	static std::atomic<uint64> NextMaterialPSORequestId = 1;
	static auto AllocateMaterialPSORequestId() -> uint64
	{
		auto Current = NextMaterialPSORequestId.load();
		while (Current != std::numeric_limits<uint64>::max())
			if (NextMaterialPSORequestId.compare_exchange_weak(Current, Current + 1)) return Current;
		return 0;
	}
	enum class EPSOPhase : uint8 { Queued, Collecting, Compiling, Completed, Retired };
	struct FSubmittedMaterialPSO
	{
		std::shared_ptr<FPipelineState> Pipeline;
		bool bRequired = true;
		ERHIPipelineRequestState CompletedState = ERHIPipelineRequestState::Failed;
	};

	struct FMaterialPSOPrecacheWork
	{
		uint64 Id = 0, ShaderGeneration = 0;
		std::optional<std::string> Target;
		FMaterialPSOPrecacheParams Params;
		std::shared_ptr<const std::atomic<uint64>> PreparationConfigVersion;
		auto IsPreparationConfigCurrent() const -> bool
		{
			const auto Version = PreparationConfigVersion ? PreparationConfigVersion->load() : 0;
			return !Version || Version == Params.PreparationConfig.Version;
		}
		FVertexDeclarationElementList Elements{};
		FMaterialPSOPrecacheCollection Collection;
		std::vector<FSubmittedMaterialPSO> Submitted;
		std::optional<std::string> OptionalError;
		std::atomic<EPSOPhase> Phase = EPSOPhase::Queued;
		std::mutex ObserversMutex;
		std::vector<std::weak_ptr<FMaterialPSOPrecacheObserver>> Observers;
		auto Notify() -> void;
		using FResult = std::expected<void, std::string>;
		std::atomic<std::shared_ptr<const FResult>> Result;
		auto GetState() const -> EMaterialPSOPrecacheState
		{
			const auto CurrentPhase = Phase.load();
			const auto Completed = Result.load();
			if (Completed && !*Completed) return EMaterialPSOPrecacheState::Failed;
			if (CurrentPhase == EPSOPhase::Retired || !IsPreparationConfigCurrent()) return EMaterialPSOPrecacheState::Stale;
			if (CurrentPhase == EPSOPhase::Completed) return EMaterialPSOPrecacheState::Ready;
			return EMaterialPSOPrecacheState::Pending;
		}
		auto GetProgress() const -> FMaterialPSOPrecacheProgress
		{ return {GetState(), Pipelines.load(), Pending.load(), RequiredPending.load(), Failed.load(), RequiredReady.load()}; }
		std::atomic<uint32> Pipelines = 0, Pending = 0, RequiredPending = 0, Failed = 0;
		std::atomic<bool> RequiredReady = false;
		std::atomic<EPSOPrecachePriority> Priority = EPSOPrecachePriority::Medium;
		EPSOPrecachePriority ScheduledPriority = EPSOPrecachePriority::Medium;
		auto Retire() -> void
		{
			RequiredReady = false;
			Phase = EPSOPhase::Retired;
			Collection = {};
			Submitted.clear();
			Params.CompiledProgram.reset();
		}
		auto Fail(std::string Error) -> void
		{
			Result = std::make_shared<const FResult>(std::unexpected(std::move(Error)));
			Phase = EPSOPhase::Completed;
		}
	};
	struct FMaterialPSOPrecacheObserver
	{
		std::shared_ptr<FMaterialPSOPrecacheWork> Work;
		std::atomic<EMaterialPSOPrecacheState> Override = EMaterialPSOPrecacheState::Pending;
		std::mutex Mutex;
		std::vector<std::weak_ptr<const FMaterialPSOPrecacheNotification>> Notifications;
		auto Notify() -> void;
	};
	struct FMaterialPSOPrecacheNotification { std::function<void()> Changed; };
	auto FMaterialPSOPrecacheObserver::Notify() -> void
	{
		std::vector<FMaterialPSOPrecacheSubscription> Active;
		{
			std::lock_guard Lock(Mutex);
			std::erase_if(Notifications, [&](const auto& Weak) {
				if (auto Item = Weak.lock()) { Active.push_back(std::move(Item)); return false; }
				return true;
			});
		}
		for (const auto& Item : Active) if (Item->Changed) Item->Changed();
	}
	auto FMaterialPSOPrecacheWork::Notify() -> void
	{
		std::vector<std::shared_ptr<FMaterialPSOPrecacheObserver>> Active;
		{
			std::lock_guard Lock(ObserversMutex);
			std::erase_if(Observers, [&](const auto& Weak) {
				if (auto Item = Weak.lock()) { Active.push_back(std::move(Item)); return false; }
				return true;
			});
		}
		for (const auto& Item : Active) Item->Notify();
	}
	static auto AttachWork(const std::shared_ptr<FMaterialPSOPrecacheObserver>& Observer,
		std::shared_ptr<FMaterialPSOPrecacheWork> Work) -> void
	{
		Observer->Work = std::move(Work);
		std::lock_guard Lock(Observer->Work->ObserversMutex);
		Observer->Work->Observers.push_back(Observer);
	}
	struct FMaterialPSOPrecacheOwner::FState { std::vector<std::weak_ptr<FMaterialPSOPrecacheObserver>> Observers; };
	FMaterialPSOPrecacheOwner::FMaterialPSOPrecacheOwner() : State(std::make_unique<FState>()) {}
	FMaterialPSOPrecacheOwner::~FMaterialPSOPrecacheOwner() { Invalidate(); }
	auto FMaterialPSOPrecacheOwner::Observe(const FMaterialPSOPrecacheRequestID& Request) -> FMaterialPSOPrecacheRequestID
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Request.Observer) return {};
		FMaterialPSOPrecacheRequestID Result;
		Result.Observer = std::make_shared<FMaterialPSOPrecacheObserver>();
		AttachWork(Result.Observer, Request.Observer->Work);
		std::erase_if(State->Observers, [](const auto& Weak) { return Weak.expired(); });
		State->Observers.push_back(Result.Observer);
		return Result;
	}
	auto FMaterialPSOPrecacheOwner::Invalidate() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		auto Observers = std::exchange(State->Observers, {});
		for (const auto& Weak : Observers) if (auto Observer = Weak.lock())
		{
			auto Expected = EMaterialPSOPrecacheState::Pending;
			if (Observer->Override.compare_exchange_strong(Expected, EMaterialPSOPrecacheState::Stale)) Observer->Notify();
		}
	}
	struct FMaterialPSORequestManager::FState
	{
		std::mutex Mutex;
		std::vector<std::weak_ptr<FMaterialPSOPrecacheWork>> Entries;
		std::vector<std::shared_ptr<FMaterialPSOPrecacheWork>> Pending;
		bool Closed = false;
		FMaterialPSOSubmitter Submitter;
		std::shared_ptr<std::atomic<uint64>> PreparationConfigVersion = std::make_shared<std::atomic<uint64>>(0);
	};

	auto FMaterialPSOPrecacheRequestID::Rejected(std::string Error) -> FMaterialPSOPrecacheRequestID
	{
		FMaterialPSOPrecacheRequestID Result;
		Result.Observer = std::make_shared<FMaterialPSOPrecacheObserver>();
		AttachWork(Result.Observer, std::make_shared<FMaterialPSOPrecacheWork>());
		Result.Observer->Work->Fail(std::move(Error));
		return Result;
	}
	auto FMaterialPSOPrecacheRequestID::GetValue() const -> uint64 { return Observer ? Observer->Work->Id : 0; }
	auto FMaterialPSOPrecacheRequestID::IsCurrent() const -> bool
	{
		return Observer && Observer->Override == EMaterialPSOPrecacheState::Pending
			&& Observer->Work->Phase != EPSOPhase::Retired
			&& Observer->Work->IsPreparationConfigCurrent()
			&& (!Observer->Work->Id || Observer->Work->ShaderGeneration == GetShaderReloadGeneration());
	}
	auto FMaterialPSOPrecacheRequestID::GetProgress() const -> FMaterialPSOPrecacheProgress
	{
		if (!Observer) return {};
		const auto& Work = *Observer->Work;
		auto Status = Work.GetState();
		if (Status != EMaterialPSOPrecacheState::Failed)
		{
			if (const auto Override = Observer->Override.load(); Override != EMaterialPSOPrecacheState::Pending) Status = Override;
			else if (Work.Id && Work.ShaderGeneration != GetShaderReloadGeneration()) Status = EMaterialPSOPrecacheState::Stale;
		}
		return {Status, Work.Pipelines.load(), Work.Pending.load(), Work.RequiredPending.load(), Work.Failed.load(),
			IsCurrent() && Work.RequiredReady.load()};
	}
	auto FMaterialPSOPrecacheRequestID::GetDiagnostic() const -> std::string
	{
		if (!Observer) return "No material PSO request.";
		const auto Result = Observer->Work->Result.load();
		return Result && !*Result ? Result->error() : std::string{};
	}
	auto FMaterialPSOPrecacheRequestID::Cancel() const -> void
	{
		if (Observer && Observer->Override.exchange(EMaterialPSOPrecacheState::Canceled) != EMaterialPSOPrecacheState::Canceled) Observer->Notify();
	}
	auto FMaterialPSOPrecacheRequestID::GetPriority() const -> EPSOPrecachePriority
	{ return Observer ? Observer->Work->Priority.load() : EPSOPrecachePriority::NotSet; }
	auto FMaterialPSOPrecacheRequestID::BoostPriority(EPSOPrecachePriority Priority) const -> void
	{
		if (!Observer || Priority >= EPSOPrecachePriority::Num
			|| GetProgress().State != EMaterialPSOPrecacheState::Pending) return;
		auto Current = Observer->Work->Priority.load();
		while (Current < Priority && !Observer->Work->Priority.compare_exchange_weak(Current, Priority)) {}
	}
	auto FMaterialPSOPrecacheRequestID::Subscribe(std::function<void()> Changed) const -> FMaterialPSOPrecacheSubscription
	{
		if (!Observer || !Changed) return {};
		auto Subscription = std::make_shared<FMaterialPSOPrecacheNotification>(std::move(Changed));
		{ std::lock_guard Lock(Observer->Mutex); Observer->Notifications.push_back(Subscription); }
		// Subscription races with completion are covered by an immediate observation.
		Subscription->Changed();
		return Subscription;
	}

	FMaterialPSORequestManager::FMaterialPSORequestManager(FMaterialPSOSubmitter Submitter)
		: State(std::make_unique<FState>())
	{
		State->Submitter = Submitter ? std::move(Submitter) : FMaterialPSOSubmitter{
			[](const FPSOPrecacheData& Data) -> std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> {
				if (Data.Type == FPSOPrecacheData::EType::Graphics)
				{
					auto Pipeline = PipelineStateCache::PrecacheGraphicsPipelineState(Data.GraphicsInitializer, "MaterialPSOPrecache");
					if (!Pipeline) return std::unexpected(Pipeline.error());
					return std::move(*Pipeline);
				}
				auto Pipeline = PipelineStateCache::PrecacheComputePipelineState(Data.ComputeInitializer, "MaterialPSOPrecache");
				if (!Pipeline) return std::unexpected(Pipeline.error());
				return std::move(*Pipeline);
			}};
	}
	FMaterialPSORequestManager::~FMaterialPSORequestManager() { Close(); }
	auto FMaterialPSORequestManager::PrecachePSOs(FMaterialPSOPrecacheParams Params) -> FMaterialPSOPrecacheRequestID
	{
		const auto Elements = Params.VertexFactoryData.GetVertexElements();
		const auto& Options = Params.PrecachePSOParams;
		if (!Elements || Params.FeatureLevel > ERHIFeatureLevel::SM6 || Params.Priority >= EPSOPrecachePriority::Num
			|| (Params.PreparationConfig.Version && Params.PreparationConfig.FeatureLevel != Params.FeatureLevel)
			|| Options.Topology >= FGraphicsPipelineStateInitializer::EPrimitiveTopology::Count
			|| !std::isfinite(Params.Material.ShaderMap.OpacityMaskThreshold))
			return FMaterialPSOPrecacheRequestID::Rejected("Invalid material PSO precache parameters or unsupported Vertex Factory.");
		uint64 CodeBytes = 0;
		if (Params.CompiledProgram)
		{
			if (!*Params.CompiledProgram || Params.CompiledProgram->FeatureLevel != Params.FeatureLevel
				|| !ValidateMaterialCompilerResult(*Params.CompiledProgram)
				|| Params.CompiledProgram->GetShaderMap().ShaderMapId != Params.Material.ShaderMap.ProgramIdentity
				|| Params.CompiledProgram->GetShaderMap().Layout.Identity != Params.Material.ShaderMap.RenderLayout)
				return FMaterialPSOPrecacheRequestID::Rejected("Material PSO request requires a matching accepted program.");
			for (const auto& Shader : Params.CompiledProgram->GetShaderMap().CompiledShaders)
			{
				if (Shader.Code.size() > MaximumCodeBytes - CodeBytes)
					return FMaterialPSOPrecacheRequestID::Rejected("Material PSO code budget exceeded.");
				CodeBytes += Shader.Code.size();
			}
		}
		const auto Generation = GetShaderReloadGeneration();
		std::lock_guard Lock(State->Mutex);
		if (State->Closed) return FMaterialPSOPrecacheRequestID::Rejected("Material PSO request manager is closed.");
		FMaterialPSOPrecacheRequestID Result;
		Result.Observer = std::make_shared<FMaterialPSOPrecacheObserver>();
		std::erase_if(State->Entries, [](const auto& Entry) { return Entry.expired(); });
		const auto Target = Params.CompiledProgram ? std::optional{Params.CompiledProgram->GetShaderMap().Target} : std::nullopt;
		if (const auto Version = State->PreparationConfigVersion->load(); Version && Version != Params.PreparationConfig.Version)
			return FMaterialPSOPrecacheRequestID::Rejected("Material PSO preparation configuration is stale.");
		for (const auto& Weak : State->Entries)
			if (const auto Entry = Weak.lock(); Entry && Entry->Phase != EPSOPhase::Retired && Entry->ShaderGeneration == Generation
				&& Entry->Params.Material == Params.Material && Entry->Params.FeatureLevel == Params.FeatureLevel
				&& Entry->Params.PreparationConfig == Params.PreparationConfig
				&& Entry->Params.PrecachePSOParams == Options
				&& Entry->Params.VertexFactoryData.VertexFactoryType == Params.VertexFactoryData.VertexFactoryType
				&& Entry->Target == Target
				&& Entry->Elements == *Elements)
			{
				AttachWork(Result.Observer, Entry);
				Result.BoostPriority(Params.Priority);
				return Result;
			}
		auto Work = std::make_shared<FMaterialPSOPrecacheWork>();
		Work->Id = AllocateMaterialPSORequestId();
		if (!Work->Id) return FMaterialPSOPrecacheRequestID::Rejected("Material PSO request identity exhausted.");
		Work->ShaderGeneration = Generation; Work->Target = Target;
		Work->PreparationConfigVersion = State->PreparationConfigVersion;
		Work->Priority = Params.Priority;
		Work->Params = std::move(Params); Work->Elements = *Elements;
		Work->RequiredReady = false;
		State->Entries.push_back(Work);
		State->Pending.push_back(Work);
		AttachWork(Result.Observer, std::move(Work));
		return Result;
	}
	auto FMaterialPSORequestManager::Pump(const FMaterialPSOCollector& Collector) -> void
	{
		std::vector<std::shared_ptr<FMaterialPSOPrecacheWork>> Entries;
		{
			std::lock_guard Lock(State->Mutex);
			std::erase_if(State->Pending, [](const auto& Entry) { return Entry.use_count() == 1; });
			Entries = State->Pending;
		}
		for (const auto& Work : Entries) Work->ScheduledPriority = Work->Priority.load();
		std::stable_sort(Entries.begin(), Entries.end(), [](const auto& A, const auto& B) {
			return A->ScheduledPriority > B->ScheduledPriority;
		});
		size_t Active = std::ranges::count_if(Entries, [](const auto& Work) { return Work->Phase == EPSOPhase::Collecting || Work->Phase == EPSOPhase::Compiling; });
		for (auto& Work : Entries)
		{
			struct FNotifyProgress
			{
				FMaterialPSOPrecacheWork& Work;
				FMaterialPSOPrecacheProgress Before;
				~FNotifyProgress() { if (Before != Work.GetProgress()) Work.Notify(); }
			} Notify{*Work, Work->GetProgress()};
			if (Work->ShaderGeneration != GetShaderReloadGeneration() || !Work->IsPreparationConfigCurrent())
			{
				Work->Retire();
				continue;
			}
			if (Work->GetState() != EMaterialPSOPrecacheState::Pending) continue;
			if (Work->Phase == EPSOPhase::Queued)
			{
				if (Active >= MaximumActiveCollections) continue;
				Work->Phase = EPSOPhase::Collecting; ++Active;
			}
			if (Work->Phase == EPSOPhase::Collecting)
			{
				try
				{
					const bool bRequiredWasCollected = Work->Collection.bRequiredPSOsCollected;
					const auto PreviousCount = Work->Collection.PSOInitializers.size();
					const auto Result = Collector(Work->Params, Work->Collection);
					if (Work->GetState() != EMaterialPSOPrecacheState::Pending) { Work->Collection = {}; continue; }
					if (bRequiredWasCollected && (!Work->Collection.bRequiredPSOsCollected
						|| Work->Collection.PSOInitializers.size() < PreviousCount
						|| std::ranges::any_of(std::span(Work->Collection.PSOInitializers).subspan(PreviousCount),
							[](const auto& Data) { return Data.bRequired; })))
					{ Work->RequiredReady = false; Work->Fail("Required PSO collection changed after publication."); continue; }
					if (Result && *Result && Work->Collection.PSOInitializers.empty() && !Work->Collection.bRequiredPSOsCollected)
					{ Work->Fail("Material PSO collector completed without declaring coverage."); continue; }
					if (Work->Collection.PSOInitializers.size() > MaximumPipelinesPerRequest)
					{ Work->Fail("Material PSO collector exceeded its pipeline capacity."); Work->Collection = {}; continue; }
					if (!Result)
					{
						if (!Work->Collection.bRequiredPSOsCollected)
						{ Work->Fail(Result.error()); continue; }
						Work->OptionalError = Result.error();
						Work->Phase = EPSOPhase::Compiling;
					}
					else if (*Result) Work->Phase = EPSOPhase::Compiling;
				}
				catch (const std::exception& Error) { Work->Fail(Error.what()); continue; }
			}
			// Accepted entries are consumed in order. Backpressure retries only the
			// first unsubmitted initializer; compiled prefixes are never resubmitted.
			try
			{
				while (Work->Submitted.size() < Work->Collection.PSOInitializers.size())
				{
					auto& Data = Work->Collection.PSOInitializers[Work->Submitted.size()];
					auto RejectSubmission = [&](std::string Error) -> bool {
						if (Data.bRequired) { Work->Fail(std::move(Error)); return false; }
						if (!Work->OptionalError) Work->OptionalError = std::move(Error);
						// Retain the failed optional slot so required pipelines are still
						// observed, including those not yet compiled on this first pump.
						Work->Submitted.push_back({{}, false, ERHIPipelineRequestState::Failed});
						Data = {};
						return true;
					};
					if (!Data.IsValid())
					{
						if (RejectSubmission("Invalid PSO precache initializer or priority.")) continue;
						break;
					}
					const bool bRequired = Data.bRequired;
					const auto Priority = Data.Priority;
					if (!FRenderResourcePreparationScope::TryBeforePipeline()) break;
					std::expected<std::shared_ptr<FPipelineState>, ERHIPipelineRequestRejection> Pipeline;
					try { Pipeline = State->Submitter(Data); }
					catch (const std::exception& Error)
					{
						if (Work->GetState() != EMaterialPSOPrecacheState::Pending) break;
						if (RejectSubmission(Error.what())) continue;
						break;
					}
					if (Work->GetState() != EMaterialPSOPrecacheState::Pending) break;
					if (!Pipeline)
					{
						if (Pipeline.error() == ERHIPipelineRequestRejection::CapacityExceeded) break;
						if (RejectSubmission("Material PSO pipeline admission failed.")) continue;
						break;
					}
					if (!*Pipeline)
					{
						if (RejectSubmission("Material PSO submission returned no pipeline.")) continue;
						break;
					}
					(*Pipeline)->BoostPriority(std::max(Priority, Work->Priority.load()));
					Work->Submitted.push_back({*Pipeline, bRequired});
					Data = {}; // Release admitted initializer layouts; the request owns its inputs.
				}
			}
			catch (const std::exception& Error) { Work->Fail(Error.what()); }
			if (Work->GetState() != EMaterialPSOPrecacheState::Pending) continue;
			uint32 Pending = 0, RequiredPending = 0, Failed = 0, RequiredFailed = 0;
			for (auto& Pipeline : Work->Submitted)
			{
				if (Pipeline.Pipeline) Pipeline.Pipeline->BoostPriority(Work->Priority.load());
				const auto Status = Pipeline.Pipeline ? Pipeline.Pipeline->GetState() : Pipeline.CompletedState;
				if (Status != ERHIPipelineRequestState::Pending)
				{
					Pipeline.CompletedState = Status;
					Pipeline.Pipeline.reset();
				}
				if (Status == ERHIPipelineRequestState::Pending) { ++Pending; if (Pipeline.bRequired) ++RequiredPending; }
				else if (Status != ERHIPipelineRequestState::Ready) { ++Failed; if (Pipeline.bRequired) ++RequiredFailed; }
			}
			for (size_t Index = Work->Submitted.size(); Index < Work->Collection.PSOInitializers.size(); ++Index)
			{
				++Pending;
				if (Work->Collection.PSOInitializers[Index].bRequired) ++RequiredPending;
			}
			Work->Pipelines = uint32(Work->Collection.PSOInitializers.size());
			Work->Pending = Pending; Work->RequiredPending = RequiredPending; Work->Failed = Failed;
			Work->RequiredReady = (Work->Collection.bRequiredPSOsCollected || Work->Phase == EPSOPhase::Compiling)
				&& !RequiredPending && !RequiredFailed;
			if (Work->Phase == EPSOPhase::Compiling && !Pending)
			{
				if (Work->OptionalError) Work->Fail(*Work->OptionalError);
				else if (Failed) Work->Fail("One or more material PSO requests failed.");
				else
				{
					Work->Result = std::make_shared<const FMaterialPSOPrecacheWork::FResult>();
					Work->Phase = EPSOPhase::Completed;
				}
			}
		}
		// Terminal observations retain only identity and progress, never native PSOs
		// or shader preparation resources. Release on the resource-owning thread.
		std::lock_guard Lock(State->Mutex);
		std::erase_if(State->Pending, [](auto& Work) {
			if (Work->GetState() == EMaterialPSOPrecacheState::Pending) return false;
			Work->Collection = {}; Work->Submitted.clear(); Work->Params.CompiledProgram.reset();
			return true;
		});
	}
	auto FMaterialPSORequestManager::GetPendingRequestCount() const -> size_t
	{
		std::lock_guard Lock(State->Mutex);
		return State->Pending.size();
	}
	auto FMaterialPSORequestManager::Invalidate() -> void
	{
		std::vector<std::shared_ptr<FMaterialPSOPrecacheWork>> Retired;
		{
			std::lock_guard Lock(State->Mutex);
			for (const auto& Weak : State->Entries) if (auto Entry = Weak.lock()) Retired.push_back(std::move(Entry));
			State->Entries.clear(); State->Pending.clear();
		}
		for (const auto& Entry : Retired) { Entry->Retire(); Entry->Notify(); }
	}
	auto FMaterialPSORequestManager::SetPreparationConfigVersion(uint64 Version) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		require(Version && Version >= State->PreparationConfigVersion->load());
		State->PreparationConfigVersion->store(Version);
	}
	auto FMaterialPSORequestManager::InvalidateOutdatedPreparationConfigs() -> void
	{
		std::vector<std::shared_ptr<FMaterialPSOPrecacheWork>> Retired;
		{
			std::lock_guard Lock(State->Mutex);
			std::erase_if(State->Entries, [&](const auto& Weak) {
				const auto Work = Weak.lock();
				if (!Work) return true;
				if (Work->IsPreparationConfigCurrent()) return false;
				Retired.push_back(Work);
				return true;
			});
			std::erase_if(State->Pending, [](const auto& Work) { return !Work->IsPreparationConfigCurrent(); });
		}
		for (const auto& Entry : Retired) { Entry->Retire(); Entry->Notify(); }
	}

	auto FMaterialPSORequestManager::Close() -> void
	{
		{ std::lock_guard Lock(State->Mutex); State->Closed = true; }
		Invalidate();
	}
}
