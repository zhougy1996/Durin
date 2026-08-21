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
	}

	auto FSceneViewHistoryProbe::Commit() -> void
	{
		// A disabled consumer publishes no candidate and retains last-known-good
		// history until an explicit discontinuity or device reset.
		if (PendingRevision != 0 || PendingTexture != nullptr)
		{
			CommittedRevision = PendingRevision;
			CommittedTexture = std::move(PendingTexture);
		}
		PendingRevision = 0;
		PendingTexture = nullptr;
	}

	auto FSceneViewHistoryProbe::Abort() -> void
	{
		PendingRevision = 0;
		PendingTexture = nullptr;
	}

	auto FSceneViewHistoryProbe::Reset() -> void
	{
		CommittedRevision = 0;
		PendingRevision = 0;
		CommittedTexture = nullptr;
		PendingTexture = nullptr;
	}

	auto FSceneViewState::Begin(
		const FSceneViewTemporalMetadata& Current,
		uint64 SubmissionSerial,
		bool bDiscardHistory) -> FSceneViewTemporalContext
	{
		check(IsInRenderingThread());
		FSceneViewTemporalContext Context;
		Context.Current = Current;
		Context.SubmissionSerial = SubmissionSerial;
		Context.SuccessfulSequence = SuccessfulSequence;
		if (bSubmissionActive)
		{
			Context.Discontinuities =
				ESceneViewDiscontinuity::DuplicateSubmission;
			return Context;
		}

		bSubmissionActive = true;
		Pending = Current;
		PendingSubmissionSerial = SubmissionSerial;
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
			Context.bHasPrevious = true;
			Context.Previous = *Committed;
			Context.PreviousSubmissionSerial =
				LastSuccessfulSubmissionSerial;
			Context.SubmissionGap = SubmissionSerial >= LastSuccessfulSubmissionSerial
				? SubmissionSerial - LastSuccessfulSubmissionSerial
				: std::numeric_limits<uint64>::max();
			if (Context.SubmissionGap > SceneViewStateInactiveSubmissionThreshold)
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
					Committed->ProjectionMatrix, Current.ProjectionMatrix))
				Context.Discontinuities |=
					ESceneViewDiscontinuity::ProjectionChange;
			if (Committed->DepthConvention != Current.DepthConvention)
				Context.Discontinuities |=
					ESceneViewDiscontinuity::DepthConventionChange;
		}

		Context.bHistoryValid = Context.bHasPrevious
			&& !IsHistoryInvalidating(Context.Discontinuities);
		if (!Context.bHistoryValid)
			HistoryProbe.Reset();
		return Context;
	}

	auto FSceneViewState::Commit() -> void
	{
		check(IsInRenderingThread());
		check(bSubmissionActive);
		check(Pending.has_value());
		Committed = std::move(Pending);
		LastSuccessfulSubmissionSerial = PendingSubmissionSerial;
		PendingSubmissionSerial = 0;
		PendingInvalidation = ESceneViewDiscontinuity::None;
		if (SuccessfulSequence != std::numeric_limits<uint64>::max())
			++SuccessfulSequence;
		HistoryProbe.Commit();
		bSubmissionActive = false;
	}

	auto FSceneViewState::Abort() -> void
	{
		check(IsInRenderingThread());
		if (!bSubmissionActive)
			return;
		Pending.reset();
		PendingSubmissionSerial = 0;
		HistoryProbe.Abort();
		bSubmissionActive = false;
	}

	auto FSceneViewState::Invalidate(ESceneViewDiscontinuity Cause) -> void
	{
		check(IsInRenderingThread());
		PendingInvalidation |= Cause;
		HistoryProbe.Reset();
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
		ESceneViewDiscontinuity Cause) -> bool
	{
		if (FSceneViewState* State = Find(Id))
		{
			State->Invalidate(Cause);
			return true;
		}
		return false;
	}

	auto FSceneViewStateRegistry::InvalidateAll(
		ESceneViewDiscontinuity Cause) -> void
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
		uint32 OutputHeight) -> FSceneViewTemporalMetadata
	{
		return {
			.ViewMatrix = View.ViewMatrix,
			.ProjectionMatrix = View.ProjectionMatrix,
			.ViewProjectionMatrix = View.ViewProjectionMatrix,
			.ViewLocation = View.ViewLocation,
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
}
