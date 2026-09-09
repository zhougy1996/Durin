#pragma once

#include "EngineAPI.h"
#include "RenderResource.h"
#include "Threading/Task.h"
#include "Texture/Texture.h"

namespace Durin
{
	// Owns an active candidate and coalesces one uninitialized successor until terminal handoff.
	// No command captures the UObject. Mutex serializes publication against Close.
	class FTextureResourceUpdate final
	{
	public:
		ENGINE_API explicit FTextureResourceUpdate(std::unique_ptr<FTextureResource> InCandidate);
		ENGINE_API ~FTextureResourceUpdate();
		ENGINE_API auto Execute_RenderThread(FRHICommandListBase& Commands,
			FTextureReference& Reference, bool bInitializeReference) -> void;
		// GameThread installs the pre-admitted handoff; Close cancels it before owner destruction.
		auto SetCompletionTask(FTaskHandle Task) -> void { CompletionTask = std::move(Task); }
		ENGINE_API auto Reject() -> void;
		ENGINE_API auto Close() -> void;
		ENGINE_API auto Wait() -> void;
		auto IsComplete() const -> bool { return bComplete.load(std::memory_order_acquire); }
		auto GetState() const -> ETextureResourceUpdateState { return State.load(std::memory_order_acquire); }
		// Only after IsComplete/Wait has acquired the terminal handoff.
		auto TakeCandidate() -> std::unique_ptr<FTextureResource> { return std::move(Candidate); }
		ENGINE_API auto GetPublishedTexture() const -> FTextureRHIRef;
		// GameThread only. Replaces queued input without touching the executing candidate.
		ENGINE_API auto SetSuccessor(std::unique_ptr<FTextureResource> Resource) -> void;
		ENGINE_API auto TakeSuccessor() -> std::unique_ptr<FTextureResource>;
	private:
		FTaskHandle CompletionTask;
		std::unique_ptr<FTextureResource> Candidate;
		std::unique_ptr<FTextureResource> Successor;
		std::mutex Mutex;
		std::condition_variable CV;
		bool bClosed = false;
		std::atomic<bool> bComplete = false;
		std::atomic<ETextureResourceUpdateState> State = ETextureResourceUpdateState::Pending;
	};
}
