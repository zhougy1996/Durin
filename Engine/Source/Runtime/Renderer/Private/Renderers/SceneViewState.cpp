#include "Renderers/SceneViewState.h"

#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto MatricesEqual(const FMatrix& Left, const FMatrix& Right) -> bool
		{
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					if (Left[Column][Row] != Right[Column][Row])
						return false;
			return true;
		}

		auto IsHistoryInvalidating(ESceneViewDiscontinuity Causes) -> bool
		{
			return Causes != ESceneViewDiscontinuity::None;
		}
	} // namespace

	auto FVolumetricCloudViewHistory::CanReproject(uint64 PolicyKey, uint64 CloudKey, uint32 Width, uint32 Height) const -> bool
	{
		return CommittedTexture != nullptr && CommittedPolicyKey == PolicyKey
			   && CommittedCloudKey == CloudKey
			   && CommittedTexture->GetSizeX() == Width
			   && CommittedTexture->GetSizeY() == Height;
	}

	auto FVolumetricCloudViewHistory::SetPending(FTextureRHIRef Texture, uint64 PolicyKey, uint64 CloudKey) -> void
	{
		PendingTexture = std::move(Texture);
		PendingPolicyKey = PolicyKey;
		PendingCloudKey = CloudKey;
		bPendingClear = false;
	}

	auto FVolumetricCloudViewHistory::SetPendingClear(
		uint64 PolicyKey, uint64 CloudKey
	) -> void
	{
		PendingTexture = nullptr;
		PendingPolicyKey = PolicyKey;
		PendingCloudKey = CloudKey;
		bPendingClear = true;
	}

	auto FVolumetricCloudViewHistory::TakeReusable(uint32 Width, uint32 Height)
		-> FTextureRHIRef
	{
		if (SpareTexture != nullptr && SpareTexture->GetSizeX() == Width
			&& SpareTexture->GetSizeY() == Height)
		{
			return std::exchange(SpareTexture, nullptr);
		}
		SpareTexture = nullptr;
		return nullptr;
	}

	auto FVolumetricCloudViewHistory::Commit() -> void
	{
		check(IsInRenderingThread());
		check(bSubmissionActive);
		check(Pending.has_value());
		if (PendingTexture == nullptr && !bPendingClear)
		{
			Abort();
			return;
		}
		Committed = std::move(Pending);
		LastSuccessfulRenderFrame = PendingRenderFrame;
		PendingRenderFrame = 0;
		PendingInvalidation = ESceneViewDiscontinuity::None;
		if (SuccessfulSequence != std::numeric_limits<uint64>::max())
			++SuccessfulSequence;
		bSubmissionActive = false;
		if (bPendingClear)
		{
			CommittedTexture = nullptr;
			SpareTexture = nullptr;
			CommittedPolicyKey = PendingPolicyKey;
			CommittedCloudKey = PendingCloudKey;
		}
		else if (PendingTexture != nullptr)
		{
			SpareTexture = std::move(CommittedTexture);
			CommittedTexture = std::move(PendingTexture);
			CommittedPolicyKey = PendingPolicyKey;
			CommittedCloudKey = PendingCloudKey;
		}
		PendingPolicyKey = 0;
		PendingCloudKey = 0;
		bPendingClear = false;
	}

	auto FVolumetricCloudViewHistory::Abort() -> void
	{
		check(IsInRenderingThread());
		Pending.reset();
		PendingRenderFrame = 0;
		bSubmissionActive = false;
		if (PendingTexture != nullptr)
			SpareTexture = std::move(PendingTexture);
		PendingPolicyKey = 0;
		PendingCloudKey = 0;
		bPendingClear = false;
	}

	auto FVolumetricCloudViewHistory::Reset() -> void
	{
		CommittedPolicyKey = 0;
		CommittedCloudKey = 0;
		PendingPolicyKey = 0;
		PendingCloudKey = 0;
		CommittedTexture = nullptr;
		PendingTexture = nullptr;
		SpareTexture = nullptr;
		bPendingClear = false;
	}

	auto FVolumetricCloudViewHistory::GetRetainedBytes() const -> uint64
	{
		auto Bytes = [](const FTextureRHIRef& Texture) -> uint64 {
			return Texture != nullptr ? static_cast<uint64>(Texture->GetSizeX()) * Texture->GetSizeY() * 8ull : 0;
		};
		return Bytes(CommittedTexture) + Bytes(PendingTexture) + Bytes(SpareTexture);
	}

	auto FVolumetricCloudViewHistory::Begin(
		const FSceneViewTemporalMetadata& Current,
		uint64 RenderFrame,
		bool bDiscardHistory
	) -> FSceneViewTemporalContext
	{
		check(IsInRenderingThread());
		FSceneViewTemporalContext Context;
		Context.OutputWidth = Current.OutputWidth;
		Context.OutputHeight = Current.OutputHeight;
		Context.RenderFrame = RenderFrame;
		Context.SuccessfulSequence = SuccessfulSequence;
		if (bSubmissionActive)
		{
			Context.Discontinuities =
				ESceneViewDiscontinuity::DuplicateSubmission;
			return Context;
		}

		bSubmissionActive = true;
		Pending = Current;
		PendingRenderFrame = RenderFrame;
		if (bDiscardHistory)
			PendingInvalidation |=
				ESceneViewDiscontinuity::ExplicitCameraCut;
		Context.Discontinuities = PendingInvalidation;

		if (!Committed)
		{
			Context.Discontinuities |= ESceneViewDiscontinuity::FirstUse;
		}
		else
		{
			Context.PreviousViewProjectionMatrix = Committed->ViewProjectionMatrix;
			Context.PreviousRenderFrame =
				LastSuccessfulRenderFrame;
			Context.FrameGap = RenderFrame >= LastSuccessfulRenderFrame ? RenderFrame - LastSuccessfulRenderFrame : std::numeric_limits<uint64>::max();
			if (Context.FrameGap > SceneViewStateInactiveFrameThreshold)
				Context.Discontinuities |=
					ESceneViewDiscontinuity::InactiveGapExpiry;
			if (Committed->Scene != Current.Scene)
				Context.Discontinuities |= ESceneViewDiscontinuity::SceneChange;
			if (Committed->OutputWidth != Current.OutputWidth
				|| Committed->OutputHeight != Current.OutputHeight)
				Context.Discontinuities |=
					ESceneViewDiscontinuity::OutputExtentChange;
			if (Committed->ViewportX != Current.ViewportX
				|| Committed->ViewportY != Current.ViewportY
				|| Committed->ViewportWidth != Current.ViewportWidth
				|| Committed->ViewportHeight != Current.ViewportHeight)
				Context.Discontinuities |=
					ESceneViewDiscontinuity::ViewportRectChange;
			if (!MatricesEqual(
					Committed->ProjectionMatrix, Current.ProjectionMatrix
				))
				Context.Discontinuities |=
					ESceneViewDiscontinuity::ProjectionChange;
			if (Committed->DepthConvention != Current.DepthConvention)
				Context.Discontinuities |=
					ESceneViewDiscontinuity::DepthConventionChange;
		}

		Context.bHistoryValid = Committed.has_value()
								&& !IsHistoryInvalidating(Context.Discontinuities);
		if (!Context.bHistoryValid)
		{
			// Keep the old texture until a replacement or clear is published.
			// A failed cloud reconstruction must not advance its camera metadata.
			SetPendingClear(0, 0);
		}
		return Context;
	}

	auto FVolumetricCloudViewHistory::Invalidate(ESceneViewDiscontinuity Cause) -> void
	{
		check(IsInRenderingThread());
		PendingInvalidation |= Cause;
		Reset();
	}

	auto FSceneViewStateRegistry::Add(FSceneViewStateId Id) -> bool
	{
		check(IsInRenderingThread());
		if (!Id.IsValid())
			return false;
		return States.try_emplace(Id).second;
	}

	auto FSceneViewStateRegistry::Remove(FSceneViewStateId Id) -> bool
	{
		check(IsInRenderingThread());
		return States.erase(Id) == 1;
	}

	auto FSceneViewStateRegistry::Find(FSceneViewStateId Id) -> FSceneViewState*
	{
		check(IsInRenderingThread());
		const auto Iterator = States.find(Id);
		return Iterator != States.end() ? &Iterator->second : nullptr;
	}

	auto FSceneViewStateRegistry::Invalidate(
		FSceneViewStateId Id,
		ESceneViewDiscontinuity Cause
	) -> bool
	{
		if (FSceneViewState* State = Find(Id))
		{
			State->Invalidate(Cause);
			return true;
		}
		return false;
	}

	auto FSceneViewStateRegistry::InvalidateAll(
		ESceneViewDiscontinuity Cause
	) -> void
	{
		check(IsInRenderingThread());
		for (auto& [Id, State] : States)
		{
			(void)Id;
			State.Invalidate(Cause);
		}
	}

	auto FSceneViewStateRegistry::ReleaseAll() -> size_t
	{
		check(IsInRenderingThread());
		const size_t Count = States.size();
		States.clear();
		return Count;
	}

	auto BuildSceneViewTemporalMetadata(
		const FSceneView& View,
		const FScene* Scene,
		uint32 OutputWidth,
		uint32 OutputHeight
	) -> FSceneViewTemporalMetadata
	{
		return {
			.ProjectionMatrix = View.ProjectionMatrix,
			.ViewProjectionMatrix = View.ViewProjectionMatrix,
			.ViewportX = View.ViewportX,
			.ViewportY = View.ViewportY,
			.ViewportWidth = View.ViewportWidth,
			.ViewportHeight = View.ViewportHeight,
			.OutputWidth = OutputWidth,
			.OutputHeight = OutputHeight,
			.DepthConvention = View.DepthConvention,
			.Scene = Scene,
		};
	}
} // namespace Durin
