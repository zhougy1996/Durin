#pragma once

#include "EngineAPI.h"
#include "RenderResource.h"
#include "Texture/Texture.h"

namespace Durin
{
	// Family candidate with an initialization diagnostic; publication belongs to its update.
	class FTextureAssetResource : public FTextureResource
	{
	public:
		explicit FTextureAssetResource(FTextureReference* Reference) : FTextureResource(Reference) {}
		auto InitRHI(FRHICommandListBase& RHICmdList) -> void override = 0;
		auto GetFailure_RenderThread() const -> ETextureRenderFailure { return Failure; }
	protected:
		auto SetFailure_RenderThread(ETextureRenderFailure Value) -> void { Failure = Value; }
	private:
		ETextureRenderFailure Failure = ETextureRenderFailure::None;
	};

	// Owns one immutable candidate through initialization, close and terminal transfer.
	// No command captures the UObject. Mutex serializes publication against Close.
	class FTextureResourceUpdate final
	{
	public:
		ENGINE_API explicit FTextureResourceUpdate(std::unique_ptr<FTextureAssetResource> InCandidate);
		ENGINE_API ~FTextureResourceUpdate();
		ENGINE_API auto Execute_RenderThread(FRHICommandListBase& Commands,
			FTextureReference& Reference, bool bInitializeReference) -> void;
		ENGINE_API auto Reject(ETextureRenderFailure Reason) -> void;
		ENGINE_API auto Close() -> void;
		ENGINE_API auto Wait() -> void;
		auto IsComplete() const -> bool { return bComplete.load(std::memory_order_acquire); }
		auto GetState() const -> ETextureResourceUpdateState { return State.load(std::memory_order_acquire); }
		// Only after IsComplete/Wait has acquired the terminal handoff.
		auto TakeCandidate() -> std::unique_ptr<FTextureAssetResource> { return std::move(Candidate); }
		auto GetFailure() const -> ETextureRenderFailure { return Failure; }
		auto GetSnapshot() const -> std::shared_ptr<const FTextureResourceSnapshot> { return Snapshot; }
	private:
		std::unique_ptr<FTextureAssetResource> Candidate;
		std::shared_ptr<const FTextureResourceSnapshot> Snapshot;
		std::mutex Mutex;
		std::condition_variable CV;
		bool bClosed = false;
		std::atomic<bool> bComplete = false;
		std::atomic<ETextureResourceUpdateState> State = ETextureResourceUpdateState::Pending;
		ETextureRenderFailure Failure = ETextureRenderFailure::None;
	};
}
