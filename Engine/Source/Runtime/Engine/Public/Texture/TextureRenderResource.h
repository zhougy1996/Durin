#pragma once

#include "EngineAPI.h"
#include "RenderResource.h"
#include "Texture/Texture.h"

namespace Durin
{
	// Owns one immutable candidate through initialization, close and terminal transfer.
	// No command captures the UObject. Mutex serializes publication against Close.
	class FTextureResourceUpdate final
	{
	public:
		ENGINE_API explicit FTextureResourceUpdate(std::unique_ptr<FTextureResource> InCandidate);
		ENGINE_API ~FTextureResourceUpdate();
		ENGINE_API auto Execute_RenderThread(FRHICommandListBase& Commands,
			FTextureReference& Reference, bool bInitializeReference) -> void;
		ENGINE_API auto Reject() -> void;
		ENGINE_API auto Close() -> void;
		ENGINE_API auto Wait() -> void;
		auto IsComplete() const -> bool { return bComplete.load(std::memory_order_acquire); }
		auto GetState() const -> ETextureResourceUpdateState { return State.load(std::memory_order_acquire); }
		// Only after IsComplete/Wait has acquired the terminal handoff.
		auto TakeCandidate() -> std::unique_ptr<FTextureResource> { return std::move(Candidate); }
		auto GetPublishedTexture() const -> FTextureRHIRef { return PublishedTexture; }
	private:
		std::unique_ptr<FTextureResource> Candidate;
		FTextureRHIRef PublishedTexture;
		std::mutex Mutex;
		std::condition_variable CV;
		bool bClosed = false;
		std::atomic<bool> bComplete = false;
		std::atomic<ETextureResourceUpdateState> State = ETextureResourceUpdateState::Pending;
	};
}
