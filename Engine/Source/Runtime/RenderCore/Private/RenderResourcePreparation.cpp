#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderCompilerCore.h"

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
	static thread_local FRenderResourcePreparationScope* CurrentPreparation = nullptr;
	static thread_local FRenderResourcePreparationUseScope* CurrentUse = nullptr;
	FRenderResourcePreparationUseScope::FRenderResourcePreparationUseScope(const FRenderResourcePreparation& Preparation)
		: State(Preparation.State), Previous(CurrentUse)
	{
		CheckRenderingThread();
		if (Preparation.GetProgress().State != ERenderResourcePreparationState::Ready) State.reset();
		CurrentUse = this;
	}
	FRenderResourcePreparationUseScope::~FRenderResourcePreparationUseScope() { CurrentUse = Previous; }
	auto FRenderResourcePreparation::Create() -> FRenderResourcePreparation
	{
		FRenderResourcePreparation Result;
		Result.State = std::make_shared<FRenderResourcePreparationState>();
		return Result;
	}
	auto FRenderResourcePreparation::GetProgress() const -> FRenderResourcePreparationProgress
	{
		if (!State) return {.State = ERenderResourcePreparationState::Failed};
		return {State->Canceled ? ERenderResourcePreparationState::Canceled
			: State->ShaderGeneration != GetShaderReloadGeneration() ? ERenderResourcePreparationState::Stale : State->Status.load(), State->Objects.load(), State->Bytes.load(), State->Shaders.load(), State->Pipelines.load(), State->UncoveredShaders.load(), State->UncoveredPipelines.load()};
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
		State->Status = Reason;
	}
	FRenderResourcePreparationScope::FRenderResourcePreparationScope(const FRenderResourcePreparation& Preparation)
		: State(Preparation.State), Previous(CurrentPreparation), Start(std::chrono::steady_clock::now())
	{
		CheckRenderingThread();
		require(State != nullptr);
		State->Status = ERenderResourcePreparationState::Pending;
		CurrentPreparation = this;
	}
	FRenderResourcePreparationScope::~FRenderResourcePreparationScope() { CurrentPreparation = Previous; }
	auto FRenderResourcePreparationScope::IsActive() -> bool { return CurrentPreparation != nullptr; }
	auto FRenderResourcePreparationScope::Complete(bool Ready, bool Failed) -> void
	{
		if (State->Canceled) State->Status = ERenderResourcePreparationState::Canceled;
		else State->Status = Failed ? ERenderResourcePreparationState::Failed
			: Ready ? ERenderResourcePreparationState::Ready : ERenderResourcePreparationState::Pending;
	}
	auto FRenderResourcePreparationScope::Admit(bool Shader) -> void
	{
		if (State->Canceled || (Shader ? Shaders >= 8 : Pipelines >= 16)
			|| ((Shaders + Pipelines) != 0 && std::chrono::steady_clock::now() - Start >= std::chrono::milliseconds(2)))
			throw FRenderResourcePreparationYield{};
		if (Shader) { ++Shaders; ++State->Shaders; }
		else { ++Pipelines; ++State->Pipelines; }
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
	static thread_local uint64 PendingPreparedRequests = 0;
	FPreparedShaderMapScope::FPreparedShaderMapScope() : FPreparedShaderMapScope(false) {}
	FPreparedShaderMapScope::FPreparedShaderMapScope(bool bAllowAsyncRequests)
		: bAllowsAsyncRequests(bAllowAsyncRequests)
	{
		if (PreparedScopeDepth++ == 0) PendingPreparedRequests = 0;
		if (!bAllowsAsyncRequests) ++CaptureOnlyDepth;
	}
	FPreparedShaderMapScope::~FPreparedShaderMapScope()
	{
		if (!bAllowsAsyncRequests) --CaptureOnlyDepth;
		--PreparedScopeDepth;
	}
	auto FPreparedShaderMapScope::IsActive() -> bool { return PreparedScopeDepth != 0; }
	auto FPreparedShaderMapScope::AllowsAsyncRequests() -> bool { return IsActive() && CaptureOnlyDepth == 0; }
	auto FPreparedShaderMapScope::GetPendingRequestCount() -> uint64
	{
		return IsActive() ? PendingPreparedRequests : 0;
	}

	auto FPreparedShaderMapScope::RecordPendingRequest() -> void { ++PendingPreparedRequests; }
}
