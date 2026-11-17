#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "Shader/ShaderCompilerCore.h"
#include "PipelineStateCache.h"

namespace Durin
{
	struct FRenderResourcePreparationState
	{
		std::mutex Mutex;
		ERenderResourcePreparationState Status = ERenderResourcePreparationState::Pending;
		uint64 ShaderGeneration = GetShaderReloadGeneration();
		bool bHasOwner = false;
		std::weak_ptr<const void> Owner;
		std::vector<std::shared_ptr<FPipelineState>> Requests;

		static auto IsTerminal(ERenderResourcePreparationState Value) -> bool
		{
			return Value == ERenderResourcePreparationState::Failed || Value == ERenderResourcePreparationState::Canceled
				|| Value == ERenderResourcePreparationState::Stale;
		}
		auto Retire(ERenderResourcePreparationState Result) -> ERenderResourcePreparationState
		{
			Status = Result;
			Requests.clear();
			Owner.reset();
			return Status;
		}
		// Called under Mutex before every observation or state transition. A caller
		// need not poll first to preserve a completed failure or invalidation.
		auto Observe() -> ERenderResourcePreparationState
		{
			if (IsTerminal(Status)) return Status;
			bool bPending = false, bCanceled = false, bFailed = false;
			for (const auto& Request : Requests)
			{
				const auto Current = Request->GetState();
				bFailed |= Current == ERHIPipelineRequestState::Failed;
				bCanceled |= Current == ERHIPipelineRequestState::Canceled;
				bPending |= Current == ERHIPipelineRequestState::Pending;
			}
			if (bFailed) return Retire(ERenderResourcePreparationState::Failed);
			if (bCanceled || ShaderGeneration != GetShaderReloadGeneration() || (bHasOwner && Owner.expired()))
				return Retire(ERenderResourcePreparationState::Stale);
			return bPending ? ERenderResourcePreparationState::Pending : Status;
		}
	};
	auto FRenderResourcePreparation::Create() -> FRenderResourcePreparation
	{
		FRenderResourcePreparation Result;
		Result.State = std::make_shared<FRenderResourcePreparationState>();
		return Result;
	}
	auto FRenderResourcePreparation::GetProgress() const -> FRenderResourcePreparationProgress
	{
		if (!State) return {.State = ERenderResourcePreparationState::Failed};
		std::lock_guard Lock(State->Mutex);
		const auto Status = State->Observe();
		return {Status, State->Requests.size()};
	}
	auto FRenderResourcePreparation::NeedsPreparation() const -> bool
	{
		if (!State) return false;
		std::lock_guard Lock(State->Mutex);
		return !State->IsTerminal(State->Observe()) && State->Status == ERenderResourcePreparationState::Pending;
	}
	auto FRenderResourcePreparation::RequestCancel() const -> void
	{
		if (!State) return;
		std::lock_guard Lock(State->Mutex);
		if (!State->IsTerminal(State->Observe())) State->Retire(ERenderResourcePreparationState::Canceled);
	}
	auto FRenderResourcePreparation::Complete_RenderThread(ERenderResourcePreparationState Result,
		std::vector<std::shared_ptr<FPipelineState>> Requests, const std::shared_ptr<const void>& Owner) const -> void
	{
		CheckRenderingThread();
		if (!State) return;
		std::lock_guard Lock(State->Mutex);
		if (State->IsTerminal(State->Observe()) || State->Status == ERenderResourcePreparationState::Ready) return;
		require(Result == ERenderResourcePreparationState::Pending || Result == ERenderResourcePreparationState::Ready
			|| Result == ERenderResourcePreparationState::Failed);
		if (State->bHasOwner && State->Owner.lock() != Owner) Result = ERenderResourcePreparationState::Failed;
		State->Status = Result;
		State->Requests = Result == ERenderResourcePreparationState::Failed ? std::vector<std::shared_ptr<FPipelineState>>{} : std::move(Requests);
		State->Owner = Owner;
		State->bHasOwner = Owner != nullptr;
	}
	auto FRenderResourcePreparation::Retire_RenderThread(ERenderResourcePreparationState Reason) const -> void
	{
		CheckRenderingThread();
		if (!State) return;
		std::lock_guard Lock(State->Mutex);
		require(State->IsTerminal(Reason));
		if (!State->IsTerminal(State->Observe())) State->Retire(Reason);
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
