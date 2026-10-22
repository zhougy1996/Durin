#include "Materials/MaterialPSOPrecache.h"

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
	struct FMaterialPSOPrecacheWork
	{
		uint64 Id = 0, ShaderGeneration = 0, CodeBytes = 0;
		FMaterialPSOPrecacheParams Params;
		FVertexDeclarationElementList Elements{};
		FMaterialPSOPrecacheCollection Collection;
		bool Collected = false;
		std::atomic<EMaterialPSOPrecacheState> Status = EMaterialPSOPrecacheState::Pending;
		std::atomic<uint32> Pipelines = 0, Pending = 0, RequiredPending = 0, Failed = 0;
		std::atomic<bool> RequiredReady = false;
		std::atomic<EPSOPrecachePriority> Priority = EPSOPrecachePriority::Medium;
		EPSOPrecachePriority ScheduledPriority = EPSOPrecachePriority::Medium;
		std::mutex DiagnosticMutex;
		std::string Diagnostic;
		auto Fail(std::string Error) -> void
		{
			{ std::lock_guard Lock(DiagnosticMutex); Diagnostic = std::move(Error); }
			Status = EMaterialPSOPrecacheState::Failed;
		}
	};
	struct FMaterialPSOPrecacheObserver
	{
		std::shared_ptr<FMaterialPSOPrecacheWork> Work;
		std::atomic<bool> Canceled = false;
		std::shared_ptr<std::atomic<uint64>> Revision;
		uint64 ExpectedRevision = 0;
	};
	struct FMaterialPSORequestManager::FState
	{
		std::mutex Mutex;
		std::vector<std::shared_ptr<FMaterialPSOPrecacheWork>> Entries;
		uint64 CodeBytes = 0;
		size_t NextCollection = 0;
		bool Closed = false;
	};

	auto FMaterialPSOPrecacheRequestID::Rejected(std::string Error) -> FMaterialPSOPrecacheRequestID
	{
		FMaterialPSOPrecacheRequestID Result;
		Result.Observer = std::make_shared<FMaterialPSOPrecacheObserver>();
		Result.Observer->Work = std::make_shared<FMaterialPSOPrecacheWork>();
		Result.Observer->Work->Fail(std::move(Error));
		return Result;
	}
	auto FMaterialPSOPrecacheRequestID::GetValue() const -> uint64 { return Observer ? Observer->Work->Id : 0; }
	auto FMaterialPSOPrecacheRequestID::GetProgress() const -> FMaterialPSOPrecacheProgress
	{
		if (!Observer) return {};
		const auto& Work = *Observer->Work;
		auto Status = Work.Status.load();
		if (Observer->Canceled) Status = EMaterialPSOPrecacheState::Canceled;
		else if ((Observer->Revision && Observer->Revision->load() != Observer->ExpectedRevision)
			|| (Work.Id && Work.ShaderGeneration != GetShaderReloadGeneration())) Status = EMaterialPSOPrecacheState::Stale;
		return {Status, Work.Pipelines.load(), Work.Pending.load(), Work.RequiredPending.load(), Work.Failed.load(),
			Status != EMaterialPSOPrecacheState::Stale && Status != EMaterialPSOPrecacheState::Canceled && Work.RequiredReady.load()};
	}
	auto FMaterialPSOPrecacheRequestID::GetDiagnostic() const -> std::string
	{
		if (!Observer) return "No material PSO request.";
		std::lock_guard Lock(Observer->Work->DiagnosticMutex);
		return Observer->Work->Diagnostic;
	}
	auto FMaterialPSOPrecacheRequestID::Cancel() const -> void { if (Observer) Observer->Canceled = true; }
	auto FMaterialPSOPrecacheRequestID::GetPriority() const -> EPSOPrecachePriority
	{ return Observer ? Observer->Work->Priority.load() : EPSOPrecachePriority::NotSet; }
	auto FMaterialPSOPrecacheRequestID::BoostPriority(EPSOPrecachePriority Priority) const -> void
	{
		if (!Observer || Priority >= EPSOPrecachePriority::Num
			|| GetProgress().State != EMaterialPSOPrecacheState::Pending) return;
		auto Current = Observer->Work->Priority.load();
		while (Current < Priority && !Observer->Work->Priority.compare_exchange_weak(Current, Priority)) {}
	}
	auto FMaterialPSOPrecacheRequestID::WithOwnerRevision(const std::shared_ptr<std::atomic<uint64>>& Revision) const -> FMaterialPSOPrecacheRequestID
	{
		if (!Observer) return {};
		FMaterialPSOPrecacheRequestID Result;
		Result.Observer = std::make_shared<FMaterialPSOPrecacheObserver>();
		Result.Observer->Work = Observer->Work;
		Result.Observer->Revision = Revision;
		Result.Observer->ExpectedRevision = Revision ? Revision->load() : 0;
		return Result;
	}

	FMaterialPSORequestManager::FMaterialPSORequestManager() : State(std::make_unique<FState>()) {}
	FMaterialPSORequestManager::~FMaterialPSORequestManager() { Close(); }
	auto FMaterialPSORequestManager::PrecachePSOs(FMaterialPSOPrecacheParams Params) -> FMaterialPSOPrecacheRequestID
	{
		const auto Elements = Params.VertexFactoryData.GetVertexElements();
		const auto& Options = Params.PrecachePSOParams;
		if (!Elements || Params.Priority >= EPSOPrecachePriority::Num || Options.Passes == EMaterialPSOPrecachePass::None
			|| (uint8(Options.Passes) & ~uint8(63)) != 0
			|| !EnumHasAllFlags(Options.Passes, Options.RequiredPasses)
			|| Options.Topology >= FGraphicsPipelineStateInitializer::EPrimitiveTopology::Count
			|| !std::isfinite(Params.Material.ShaderMap.OpacityMaskThreshold))
			return FMaterialPSOPrecacheRequestID::Rejected("Invalid material PSO precache parameters or unsupported Vertex Factory.");
		uint64 CodeBytes = 0;
		if (Params.CompiledProgram)
		{
			if (!*Params.CompiledProgram || !ValidateMaterialCompilerResult(*Params.CompiledProgram)
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
		for (const auto& Entry : State->Entries)
			if (Entry->Status != EMaterialPSOPrecacheState::Stale && Entry->ShaderGeneration == Generation
				&& Entry->Params.Material == Params.Material && Entry->Params.PrecachePSOParams == Options
				&& Entry->Params.VertexFactoryData.VertexFactoryType == Params.VertexFactoryData.VertexFactoryType
				&& bool(Entry->Params.CompiledProgram) == bool(Params.CompiledProgram)
				&& (!Params.CompiledProgram || Entry->Params.CompiledProgram->GetShaderMap().Target == Params.CompiledProgram->GetShaderMap().Target)
				&& Entry->Elements == *Elements)
			{
				Result.Observer->Work = Entry;
				Result.BoostPriority(Params.Priority);
				return Result;
			}
		if (State->Entries.size() >= MaximumRequests || CodeBytes > MaximumCodeBytes - State->CodeBytes)
			return FMaterialPSOPrecacheRequestID::Rejected("Material PSO request capacity exceeded.");
		auto Work = std::make_shared<FMaterialPSOPrecacheWork>();
		Work->Id = AllocateMaterialPSORequestId();
		if (!Work->Id) return FMaterialPSOPrecacheRequestID::Rejected("Material PSO request identity exhausted.");
		Work->ShaderGeneration = Generation; Work->CodeBytes = CodeBytes;
		Work->Priority = Params.Priority;
		Work->Params = std::move(Params); Work->Elements = *Elements;
		State->Entries.push_back(Work); State->CodeBytes += CodeBytes;
		Result.Observer->Work = std::move(Work);
		return Result;
	}
	auto FMaterialPSORequestManager::Pump(const FMaterialPSOCollector& Collector) -> void
	{
		std::vector<std::shared_ptr<FMaterialPSOPrecacheWork>> Entries;
		{
			std::lock_guard Lock(State->Mutex);
			std::erase_if(State->Entries, [&](auto& Entry) {
				if (Entry.use_count() != 1) return false;
				State->CodeBytes -= Entry->CodeBytes;
				return true;
			});
			Entries = State->Entries;
			if (!Entries.empty())
			{
				std::rotate(Entries.begin(), Entries.begin() + State->NextCollection % Entries.size(), Entries.end());
				State->NextCollection = (State->NextCollection + 8) % Entries.size();
			}
		}
		for (const auto& Work : Entries) Work->ScheduledPriority = Work->Priority.load();
		std::stable_sort(Entries.begin(), Entries.end(), [](const auto& A, const auto& B) {
			return A->ScheduledPriority > B->ScheduledPriority;
		});
		size_t Collections = 0;
		for (auto& Work : Entries)
		{
			if (Work->ShaderGeneration != GetShaderReloadGeneration())
			{
				std::lock_guard Lock(State->Mutex);
				Work->RequiredReady = false; Work->Status = EMaterialPSOPrecacheState::Stale;
				Work->Collection = {}; Work->Params.CompiledProgram.reset();
				State->CodeBytes -= Work->CodeBytes; Work->CodeBytes = 0;
				continue;
			}
			if (Work->Status != EMaterialPSOPrecacheState::Pending) continue;
			if (!Work->Collected && Collections++ < 8)
			{
				try
				{
					const auto Result = Collector(Work->Params, Work->Collection);
					if (Work->Status != EMaterialPSOPrecacheState::Pending) { Work->Collection = {}; continue; }
					if (!Result) { Work->Fail(Result.error()); continue; }
					if (*Result && Work->Collection.Pipelines.empty())
					{ Work->Fail("Material PSO collector completed without covering a pipeline."); continue; }
					if (Work->Collection.Pipelines.size() > MaximumPipelinesPerRequest)
					{ Work->Fail("Material PSO collector exceeded its pipeline capacity."); Work->Collection = {}; continue; }
					Work->Collected = *Result;
				}
				catch (const std::exception& Error) { Work->Fail(Error.what()); continue; }
			}
			uint32 Pending = 0, RequiredPending = 0, Failed = 0, RequiredFailed = 0;
			for (const auto& Pipeline : Work->Collection.Pipelines)
			{
				if (Pipeline.Pipeline) Pipeline.Pipeline->BoostPriority(Work->Priority.load());
				const auto Status = Pipeline.Pipeline ? Pipeline.Pipeline->GetState() : ERHIPipelineRequestState::Failed;
				if (Status == ERHIPipelineRequestState::Pending) { ++Pending; if (Pipeline.bRequired) ++RequiredPending; }
				else if (Status != ERHIPipelineRequestState::Ready) { ++Failed; if (Pipeline.bRequired) ++RequiredFailed; }
			}
			Work->Pipelines = uint32(Work->Collection.Pipelines.size());
			Work->Pending = Pending; Work->RequiredPending = RequiredPending; Work->Failed = Failed;
			Work->RequiredReady = Work->Collected && !RequiredPending && !RequiredFailed;
			if (Work->Collected && !Pending)
			{
				if (Failed) Work->Fail("One or more material PSO requests failed.");
				else Work->Status = EMaterialPSOPrecacheState::Ready;
			}
		}
	}
	auto FMaterialPSORequestManager::Invalidate() -> void
	{
		std::lock_guard Lock(State->Mutex);
		for (auto& Entry : State->Entries)
		{
			Entry->RequiredReady = false; Entry->Status = EMaterialPSOPrecacheState::Stale;
			Entry->Collection = {}; Entry->Params.CompiledProgram.reset();
			Entry->CodeBytes = 0;
		}
		State->Entries.clear(); State->CodeBytes = 0;
	}
	auto FMaterialPSORequestManager::Close() -> void
	{
		{ std::lock_guard Lock(State->Mutex); State->Closed = true; }
		Invalidate();
	}
}
