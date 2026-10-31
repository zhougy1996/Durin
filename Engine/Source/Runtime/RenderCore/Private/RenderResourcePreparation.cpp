#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderCompilerCore.h"
#include "Profiling/Profiling.h"
#include "ShaderPipelineCache.h"

namespace Durin
{
	struct FRenderResourcePreparationState
	{
		std::atomic<ERenderResourcePreparationState> Status{ERenderResourcePreparationState::Pending};
		std::atomic_bool Canceled = false;
		uint64 ShaderGeneration = GetShaderReloadGeneration();
		std::atomic<uint64> Objects = 0, Bytes = 0, Shaders = 0, Pipelines = 0, UncoveredShaders = 0, UncoveredPipelines = 0;
		struct FRetained { std::shared_ptr<const void> Resource; uint64 Bytes; };
		std::vector<FRetained> Resources;
	};
	static thread_local FRenderResourceAdmissionScope* CurrentAdmission = nullptr;
	auto FRenderResourceAdmissionBudget::BeginFrame(uint64 InFrame) -> void
	{
		if (Frame == InFrame) return;
		Frame = InFrame; Shaders = Pipelines = Deferred = 0;
		if (bUseBatchMode)
		{
			const auto Policy = FShaderPipelineCache::GetBatchBudget();
			MaximumShaders = Policy.Shaders;
			MaximumPipelines = Policy.Pipelines;
			TimeBudget = Policy.Time;
		}
		Start = std::chrono::steady_clock::now();
	}
	auto FRenderResourceAdmissionBudget::Admit(bool bShader) -> bool
	{
		if ((bShader ? Shaders >= MaximumShaders : Pipelines >= MaximumPipelines)
			|| ((Shaders + Pipelines) != 0 && std::chrono::steady_clock::now() - Start >= TimeBudget))
		{
			if (Profiling::IsPostCommitStartupTimingActive())
				DURIN_DEBUG_CATEGORY("Startup", "Admission deferred: kind={} shaders={} pipelines={} elapsed_ms={:.3f}",
					bShader ? "shader" : "pipeline", Shaders, Pipelines,
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count());
			++Deferred; return false;
		}
		if (Shaders + Pipelines == 0) Start = std::chrono::steady_clock::now();
		if (bShader) ++Shaders; else ++Pipelines;
		return true;
	}
	FRenderResourceAdmissionScope::FRenderResourceAdmissionScope(FRenderResourceAdmissionBudget& InBudget, uint64 Frame)
		: Budget(InBudget), Previous(CurrentAdmission)
	{
		if (Budget.Frame != Frame) Budget.BeginFrame(Frame);
		CurrentAdmission = this;
	}
	FRenderResourceAdmissionScope::~FRenderResourceAdmissionScope() { CurrentAdmission = Previous; }
	auto FRenderResourceAdmissionScope::GetDeferredAdmissions() -> uint32
	{ return CurrentAdmission ? CurrentAdmission->Budget.Deferred : 0; }

	static thread_local FRenderResourcePreparationScope* CurrentPreparation = nullptr;
	static thread_local FRenderResourcePreparationUseScope* CurrentUse = nullptr;
	FRenderResourcePreparationUseScope::FRenderResourcePreparationUseScope(const FRenderResourcePreparation& Preparation)
		: State(Preparation.State), Previous(CurrentUse)
	{
		CheckRenderingThread();
		if (Preparation.GetProgress().State != ERenderResourcePreparationState::Ready) State.reset();
		CurrentUse = this;
	}
	FRenderResourcePreparationUseScope::FRenderResourcePreparationUseScope(FRenderResourcePreparationScope& Preparation)
		: State(Preparation.State), Previous(CurrentUse), SuspendedPreparation(&Preparation)
	{
		CheckRenderingThread();
		require(CurrentPreparation == &Preparation);
		if (State->Canceled) throw FRenderResourcePreparationYield{};
		CurrentPreparation = nullptr;
		CurrentUse = this;
	}
	FRenderResourcePreparationUseScope::~FRenderResourcePreparationUseScope()
	{
		CurrentUse = Previous;
		if (SuspendedPreparation)
		{
			require(CurrentPreparation == nullptr);
			CurrentPreparation = SuspendedPreparation;
		}
	}
	auto FRenderResourcePreparation::Create() -> FRenderResourcePreparation
	{
		FRenderResourcePreparation Result;
		Result.State = std::make_shared<FRenderResourcePreparationState>();
		return Result;
	}
	auto FRenderResourcePreparation::GetProgress() const -> FRenderResourcePreparationProgress
	{
		if (!State) return {.State = ERenderResourcePreparationState::Failed};
		const auto Status = State->Status.load();
		const bool Terminal = Status == ERenderResourcePreparationState::Failed || Status == ERenderResourcePreparationState::Canceled;
		return {Terminal ? Status : State->Canceled ? ERenderResourcePreparationState::Canceled
			: State->ShaderGeneration != GetShaderReloadGeneration() ? ERenderResourcePreparationState::Stale : Status, State->Objects.load(), State->Bytes.load(), State->Shaders.load(), State->Pipelines.load(), State->UncoveredShaders.load(), State->UncoveredPipelines.load()};
	}
	auto FRenderResourcePreparation::RequestCancel() const -> void
	{
		if (State) State->Canceled = true;
	}
	auto FRenderResourcePreparation::Retire_RenderThread(ERenderResourcePreparationState Reason) const -> void
	{
		CheckRenderingThread();
		if (!State) return;
		State->Resources.clear();
		State->Objects = 0; State->Bytes = 0;
		const auto Current = State->Status.load();
		if (Current != ERenderResourcePreparationState::Failed && Current != ERenderResourcePreparationState::Canceled)
			State->Status = Reason;
	}
	FRenderResourcePreparationScope::FRenderResourcePreparationScope(const FRenderResourcePreparation& Preparation)
		: State(Preparation.State), Previous(CurrentPreparation)
	{
		CheckRenderingThread();
		require(State != nullptr);
		LocalBudget.BeginFrame(0);
		Budget = CurrentPreparation ? CurrentPreparation->Budget
			: CurrentAdmission ? &CurrentAdmission->Budget : &LocalBudget;
		if (State->Status == ERenderResourcePreparationState::Ready)
		{
			State->Resources.clear();
			State->Objects = 0; State->Bytes = 0;
		}
		State->Status = ERenderResourcePreparationState::Pending;
		CurrentPreparation = this;
	}
	FRenderResourcePreparationScope::~FRenderResourcePreparationScope() { CurrentPreparation = Previous; }
	auto FRenderResourcePreparationScope::IsActive() -> bool { return CurrentPreparation != nullptr; }
	auto FRenderResourcePreparationScope::Complete(ERenderResourcePreparationState Result) -> void
	{
		require(Result == ERenderResourcePreparationState::Pending || Result == ERenderResourcePreparationState::Ready
			|| Result == ERenderResourcePreparationState::Failed);
		if (State->Canceled) Result = ERenderResourcePreparationState::Canceled;
		if (Result == ERenderResourcePreparationState::Failed || Result == ERenderResourcePreparationState::Canceled)
		{
			State->Resources.clear();
			State->Objects = 0; State->Bytes = 0;
		}
		State->Status = Result;
	}
	auto FRenderResourcePreparationScope::Admit(bool Shader) -> void
	{
		if (State->Canceled || !Budget->Admit(Shader))
		{
			if (Profiling::IsPostCommitStartupTimingActive())
				DURIN_DEBUG_CATEGORY("Startup", "Preparation yielded: canceled={} next={} shaders={} pipelines={}",
					State->Canceled.load(), Shader ? "shader" : "pipeline", Budget->Shaders, Budget->Pipelines);
			throw FRenderResourcePreparationYield{};
		}
		if (Shader) ++State->Shaders;
		else ++State->Pipelines;
	}
	auto FRenderResourcePreparationScope::BeforeShader() -> void
	{
		if (CurrentPreparation) CurrentPreparation->Admit(true);
		else if (CurrentUse && CurrentUse->State) ++CurrentUse->State->UncoveredShaders;
	}
	auto FRenderResourcePreparationScope::BeforePipeline() -> void
	{
		if (CurrentPreparation) CurrentPreparation->Admit(false);
		else if (CurrentUse && CurrentUse->State) ++CurrentUse->State->UncoveredPipelines;
	}
	auto FRenderResourcePreparationScope::TryBeforeShader() -> bool
	{
		if (!CurrentPreparation && CurrentAdmission && !CurrentAdmission->Budget.Admit(true)) return false;
		BeforeShader(); return true;
	}
	auto FRenderResourcePreparationScope::TryBeforePipeline() -> bool
	{
		if (!CurrentPreparation && CurrentAdmission && !CurrentAdmission->Budget.Admit(false)) return false;
		BeforePipeline(); return true;
	}

	auto FRenderResourcePreparationScope::Retain(std::shared_ptr<const void> Resource, uint64 Bytes) -> void
	{
		if (!CurrentPreparation || !Resource) return;
		auto& State = *CurrentPreparation->State;
		if (State.Canceled) throw FRenderResourcePreparationYield{};
		for (auto& Existing : State.Resources)
			if (Existing.Resource.get() == Resource.get())
			{
				if (Bytes <= Existing.Bytes) return;
				const auto Additional = Bytes - Existing.Bytes;
				if (Additional > 256ull * 1024 * 1024 - State.Bytes.load()) throw FRenderResourcePreparationCapacityExceeded{};
				Existing.Bytes = Bytes; State.Bytes += Additional;
				return;
			}
		if (State.Resources.size() >= 4096 || Bytes > 256ull * 1024 * 1024 - State.Bytes.load())
			throw FRenderResourcePreparationCapacityExceeded{};
		State.Resources.push_back({std::move(Resource), Bytes});
		State.Objects = State.Resources.size();
		State.Bytes += Bytes;
	}
	static thread_local uint32 PreparedScopeDepth = 0;
	static thread_local uint32 CaptureOnlyDepth = 0;
	FPreparedShaderMapScope::FPreparedShaderMapScope() : FPreparedShaderMapScope(false) {}
	FPreparedShaderMapScope::FPreparedShaderMapScope(bool bAllowAsyncRequests)
		: bAllowsAsyncRequests(bAllowAsyncRequests)
	{
		++PreparedScopeDepth;
		if (!bAllowsAsyncRequests) ++CaptureOnlyDepth;
	}
	FPreparedShaderMapScope::~FPreparedShaderMapScope()
	{
		if (!bAllowsAsyncRequests) --CaptureOnlyDepth;
		--PreparedScopeDepth;
	}
	auto FPreparedShaderMapScope::IsActive() -> bool { return PreparedScopeDepth != 0; }
	auto FPreparedShaderMapScope::AllowsAsyncRequests() -> bool { return IsActive() && CaptureOnlyDepth == 0; }
}
