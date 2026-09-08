#include "Texture/TextureRenderResource.h"
#include "RenderingThread.h"

namespace Durin
{
	FTextureResourceUpdate::FTextureResourceUpdate(std::unique_ptr<FTextureAssetResource> InCandidate)
		: Candidate(std::move(InCandidate)) { check(Candidate != nullptr); }
	FTextureResourceUpdate::~FTextureResourceUpdate() = default;

	auto FTextureResourceUpdate::Execute_RenderThread(FRHICommandListBase& Commands,
		FTextureReference& Reference, bool bInitializeReference) -> void
	{
		CheckRenderingThread();
		State.store(ETextureResourceUpdateState::Building, std::memory_order_release);
		try
		{
			// Accepted reference initialization must run even after close.
			if (bInitializeReference) Reference.InitResource(Commands);
			bool bInitialize = false;
			{
				std::lock_guard Lock(Mutex);
				bInitialize = !bClosed;
			}
			if (bInitialize)
			{
				Candidate->InitResource(Commands);
				Failure = Candidate->GetFailure_RenderThread();
				if (!Candidate->GetTextureRHI_RenderThread() && Failure == ETextureRenderFailure::None)
					Failure = ETextureRenderFailure::CreateOrUpload;
				if (Failure == ETextureRenderFailure::None)
				{
					auto Result = std::make_shared<FTextureResourceSnapshot>();
					Result->Texture = Candidate->GetTextureRHI_RenderThread();
					FTextureReference FixedReference(Result->Texture);
					Result->FixedReference = FixedReference.GetTextureReferenceRHI();
					std::lock_guard Lock(Mutex);
					if (!bClosed)
					{
						Candidate->PublishTexture_RenderThread();
						Snapshot = std::move(Result);
					}
				}
			}
		}
		catch (...)
		{
			// Every admitted CPU operation must terminalize so owner teardown can join it.
			Failure = ETextureRenderFailure::CreateOrUpload;
		}
		{
			std::lock_guard Lock(Mutex);
			State.store(bClosed ? ETextureResourceUpdateState::Closed
				: Snapshot ? ETextureResourceUpdateState::Succeeded
				: ETextureResourceUpdateState::Failed, std::memory_order_release);
			bComplete.store(true, std::memory_order_release);
		}
		CV.notify_all();
	}

	auto FTextureResourceUpdate::Reject(ETextureRenderFailure Reason) -> void
	{
		std::lock_guard Lock(Mutex);
		Failure = Reason;
		State.store(ETextureResourceUpdateState::Failed, std::memory_order_release);
		bComplete.store(true, std::memory_order_release);
		CV.notify_all();
	}

	auto FTextureResourceUpdate::Close() -> void
	{
		std::lock_guard Lock(Mutex);
		bClosed = true;
	}

	auto FTextureResourceUpdate::Wait() -> void
	{
		std::unique_lock Lock(Mutex);
		CV.wait(Lock, [this]() { return bComplete.load(std::memory_order_acquire); });
	}
}
