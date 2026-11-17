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
		auto Retire = [&](ERenderResourcePreparationState Result) {
			State->Status = Result;
			State->Requests.clear();
			return FRenderResourcePreparationProgress{Result};
		};
		auto Status = State->Status;
		if (Status == ERenderResourcePreparationState::Failed || Status == ERenderResourcePreparationState::Canceled
			|| Status == ERenderResourcePreparationState::Stale) return {Status, State->Requests.size()};
		if (State->ShaderGeneration != GetShaderReloadGeneration() || (State->bHasOwner && State->Owner.expired()))
			return Retire(ERenderResourcePreparationState::Stale);
		for (const auto& Request : State->Requests)
		{
			const auto RequestState = Request->GetState();
			if (RequestState == ERHIPipelineRequestState::Failed) return Retire(ERenderResourcePreparationState::Failed);
			if (RequestState == ERHIPipelineRequestState::Canceled) return Retire(ERenderResourcePreparationState::Stale);
			if (RequestState == ERHIPipelineRequestState::Pending) Status = ERenderResourcePreparationState::Pending;
		}
		return {Status, State->Requests.size()};
	}
	auto FRenderResourcePreparation::NeedsPreparation() const -> bool
	{
		if (!State) return false;
		std::lock_guard Lock(State->Mutex);
		return State->Status == ERenderResourcePreparationState::Pending;
	}
	auto FRenderResourcePreparation::RequestCancel() const -> void
	{
		if (!State) return;
		std::lock_guard Lock(State->Mutex);
		if (State->Status != ERenderResourcePreparationState::Failed) State->Status = ERenderResourcePreparationState::Canceled;
		State->Requests.clear();
	}
	auto FRenderResourcePreparation::Complete_RenderThread(ERenderResourcePreparationState Result,
		std::vector<std::shared_ptr<FPipelineState>> Requests, const std::shared_ptr<const void>& Owner) const -> void
	{
		CheckRenderingThread();
		if (!State) return;
		std::lock_guard Lock(State->Mutex);
		if (State->Status == ERenderResourcePreparationState::Failed || State->Status == ERenderResourcePreparationState::Canceled
			|| State->Status == ERenderResourcePreparationState::Stale) return;
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
		if (State->Status != ERenderResourcePreparationState::Failed && State->Status != ERenderResourcePreparationState::Canceled)
			State->Status = Reason;
		State->Requests.clear();
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
