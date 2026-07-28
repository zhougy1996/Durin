#include "Texture/TextureRenderResource.h"

#include "RenderingThread.h"

namespace Durin
{
	auto FTextureResourceCompletion::BeginRequest(uint64 Revision) -> void
	{
		RequestedRevision.store(Revision, std::memory_order_release);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Pending, Revision);
	}

	auto FTextureResourceCompletion::MarkBuilding(uint64 Revision) -> bool
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return false;
		SetResourceState(ERenderResourceState::Building, Revision);
		return true;
	}

	auto FTextureResourceCompletion::MarkReady(uint64 Revision) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		AdvanceAppliedRevision(Revision);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Ready, Revision);
	}

	auto FTextureResourceCompletion::MarkFailed(
		uint64 Revision, ETextureRenderFailure Reason) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		FailureReason.store(Reason, std::memory_order_release);
		FailedRevision.store(Revision, std::memory_order_release);
		SetResourceState(ERenderResourceState::Failed, Revision);
	}

	auto FTextureResourceCompletion::MarkReleased(uint64 Revision) -> void
	{
		if (Revision != RequestedRevision.load(std::memory_order_acquire))
			return;
		AdvanceAppliedRevision(Revision);
		FailedRevision.store(0, std::memory_order_release);
		FailureReason.store(
			ETextureRenderFailure::None, std::memory_order_release);
		SetResourceState(ERenderResourceState::Released, Revision);
	}

	auto FTextureResourceCompletion::GetResourceState() const
		-> ERenderResourceState
	{
		std::lock_guard Lock(ResourceStateMutex);
		if (ResourceStateRevision
			!= RequestedRevision.load(std::memory_order_acquire))
		{
			return ERenderResourceState::Pending;
		}
		return ResourceState;
	}

	auto FTextureResourceCompletion::SetResourceState(
		ERenderResourceState State, uint64 Revision) -> void
	{
		std::lock_guard Lock(ResourceStateMutex);
		ResourceState = State;
		ResourceStateRevision = Revision;
	}

	auto FTextureResourceCompletion::AdvanceAppliedRevision(uint64 Revision) -> void
	{
		uint64 Current = AppliedRevision.load(std::memory_order_acquire);
		while (Current < Revision
			&& !AppliedRevision.compare_exchange_weak(
				Current, Revision, std::memory_order_release,
				std::memory_order_acquire))
		{
		}
	}

	FTextureAssetResource::FTextureAssetResource(
		FTextureReference* InTextureReference,
		uint64 InRevision,
		std::shared_ptr<FTextureResourceCompletion> InCompletion)
		: FTextureResource(InTextureReference)
		, Revision(InRevision)
		, Completion(std::move(InCompletion))
	{
		check(Completion != nullptr);
	}

	FTextureAssetResource::~FTextureAssetResource() = default;

	auto FTextureAssetResource::ReleaseRHI() -> void
	{
		check(IsInRenderingThread());
		FTextureResource::ReleaseRHI();
		Completion->MarkReleased(
			ReleaseRevision != 0 ? ReleaseRevision : Revision);
	}
}
