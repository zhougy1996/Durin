#include "Texture/TextureRenderResource.h"
#include "RenderingThread.h"

namespace Durin
{
	FTextureResourceUpdate::FTextureResourceUpdate(std::unique_ptr<FTextureResource> InCandidate)
		: Candidate(std::move(InCandidate)) { check(Candidate != nullptr); }
	FTextureResourceUpdate::~FTextureResourceUpdate() = default;

	auto FTextureResourceUpdate::Execute_RenderThread(FRHICommandListBase& Commands,
		FTextureReference& Reference, bool bInitializeReference) -> void
	{
		CheckRenderingThread();
		State.store(ETextureResourceUpdateState::Building, std::memory_order_release);
		bool bPublished = false;
		try
		{
			// Accepted reference initialization must run even after close.
			if (bInitializeReference) Reference.InitResource(Commands);
			Reference.ResetToFallback_RenderThread();
			bool bInitialize = false;
			{
				std::lock_guard Lock(Mutex);
				bInitialize = !bClosed && !bDiscarded;
			}
			if (bInitialize)
			{
				Candidate->InitResource(Commands);
				if (Candidate->GetTextureRHI_RenderThread())
				{
					std::lock_guard Lock(Mutex);
					if (!bClosed && !bDiscarded)
					{
						Candidate->PublishTexture_RenderThread();
						bPublished = true;
					}
				}
			}
		}
		catch (...)
		{
			// Every admitted CPU operation must terminalize so owner teardown can join it.
			DURIN_WARN("Texture initialization threw an exception before publication.");
		}
		{
			std::lock_guard Lock(Mutex);
			State.store(bClosed || bDiscarded ? ETextureResourceUpdateState::Closed
				: bPublished ? ETextureResourceUpdateState::Succeeded
				: ETextureResourceUpdateState::Failed, std::memory_order_release);
			bComplete.store(true, std::memory_order_release);
		}
		CV.notify_all();
	}

	auto FTextureResourceUpdate::GetPublishedTexture() const -> FTextureRHIRef
	{
		CheckGameThread();
		check(IsComplete());
		return GetState() == ETextureResourceUpdateState::Succeeded && Candidate
			? Candidate->GetTextureRHI_GameThread() : FTextureRHIRef{};
	}

	auto FTextureResourceUpdate::SetSuccessor(std::unique_ptr<FTextureResource> Resource) -> void
	{
		CheckGameThread();
		check(!bClosed);
		Successor = std::move(Resource);
	}

	auto FTextureResourceUpdate::TakeSuccessor() -> std::unique_ptr<FTextureResource>
	{
		CheckGameThread();
		check(IsComplete());
		return std::move(Successor);
	}

	auto FTextureResourceUpdate::Reject() -> void
	{
		std::lock_guard Lock(Mutex);
		State.store(ETextureResourceUpdateState::Failed, std::memory_order_release);
		bComplete.store(true, std::memory_order_release);
		CV.notify_all();
	}

	auto FTextureResourceUpdate::Discard() -> void
	{
		CheckGameThread();
		Successor.reset();
		std::lock_guard Lock(Mutex);
		bDiscarded = true;
		if (IsComplete()) State.store(ETextureResourceUpdateState::Closed, std::memory_order_release);
	}

	auto FTextureResourceUpdate::Close() -> void
	{
		CheckGameThread();
		if (CompletionTask.IsValid()) CancelTask(CompletionTask);
		Successor.reset();
		std::lock_guard Lock(Mutex);
		bClosed = true;
	}

	auto FTextureResourceUpdate::Wait() -> void
	{
		std::unique_lock Lock(Mutex);
		CV.wait(Lock, [this]() { return bComplete.load(std::memory_order_acquire); });
	}
}
