#pragma once

#include "SceneView.h"
#include "RendererAPI.h"

namespace Durin
{
	class FScene;

	inline constexpr uint64 SceneViewStateInactiveSubmissionThreshold = 120;

	// Explains why committed metadata cannot be consumed as valid history.
	enum class ESceneViewDiscontinuity : uint16
	{
		None = 0,
		MissingState = 1 << 0,
		FirstUse = 1 << 1,
		ExplicitCameraCut = 1 << 2,
		SceneChange = 1 << 3,
		OutputExtentChange = 1 << 4,
		ViewportRectChange = 1 << 5,
		ProjectionChange = 1 << 6,
		DepthConventionChange = 1 << 7,
		InactiveGapExpiry = 1 << 8,
		DeviceInvalidation = 1 << 9,
		ManualInvalidation = 1 << 10,
		DuplicateSubmission = 1 << 11,
	};

	constexpr auto operator|(
		ESceneViewDiscontinuity Left,
		ESceneViewDiscontinuity Right
	) -> ESceneViewDiscontinuity
	{
		return static_cast<ESceneViewDiscontinuity>(
			static_cast<uint16>(Left) | static_cast<uint16>(Right)
		);
	}

	constexpr auto operator|=(
		ESceneViewDiscontinuity& Left,
		ESceneViewDiscontinuity Right
	) -> ESceneViewDiscontinuity&
	{
		Left = Left | Right;
		return Left;
	}

	constexpr auto HasAnyViewDiscontinuity(
		ESceneViewDiscontinuity Value,
		ESceneViewDiscontinuity Mask
	) -> bool
	{
		return (static_cast<uint16>(Value) & static_cast<uint16>(Mask)) != 0;
	}

	// Frozen final-fitted metadata published only by a successful outer view.
	struct FSceneViewTemporalMetadata
	{
		FMatrix ViewMatrix{1.0};
		FMatrix ProjectionMatrix{1.0};
		FMatrix ViewProjectionMatrix{1.0};
		FVector3 ViewLocation{0.0};
		uint32 ViewportX = 0;
		uint32 ViewportY = 0;
		uint32 ViewportWidth = 0;
		uint32 ViewportHeight = 0;
		uint32 OutputWidth = 0;
		uint32 OutputHeight = 0;
		ESceneDepthConvention DepthConvention =
			ESceneDepthConvention::ForwardZ;
		const FScene* Scene = nullptr;
	};

	// Immutable per-submission snapshot consumed by future feature renderers.
	struct FSceneViewTemporalContext
	{
		FSceneViewTemporalMetadata Current;
		FSceneViewTemporalMetadata Previous;
		uint64 SubmissionSerial = 0;
		uint64 PreviousSubmissionSerial = 0;
		uint64 SuccessfulSequence = 0;
		uint64 SubmissionGap = 0;
		ESceneViewDiscontinuity Discontinuities =
			ESceneViewDiscontinuity::None;
		bool bHasPrevious = false;
		bool bHistoryValid = false;
	};

	// Demonstrates the private strongly typed extension pattern used by future histories.
	struct FSceneViewHistoryProbe
	{
		uint64 CommittedRevision = 0;
		uint64 PendingRevision = 0;
		FTextureRHIRef CommittedTexture;
		FTextureRHIRef PendingTexture;

		RENDERER_API auto Commit() -> void;
		RENDERER_API auto Abort() -> void;
		RENDERER_API auto Reset() -> void;
	};

	// Private per-view cloud history. Candidate publication follows the outer
	// view transaction; aborted candidates become reusable scratch storage.
	struct FVolumetricCloudViewHistory
	{
		uint64 CommittedPolicyKey = 0;
		uint64 CommittedCloudKey = 0;
		uint64 PendingPolicyKey = 0;
		uint64 PendingCloudKey = 0;
		FTextureRHIRef CommittedTexture;
		FTextureRHIRef PendingTexture;
		FTextureRHIRef SpareTexture;
		bool bPendingClear = false;

		[[nodiscard]] RENDERER_API auto CanReproject(uint64 PolicyKey, uint64 CloudKey, uint32 Width, uint32 Height) const -> bool;
		RENDERER_API auto SetPending(FTextureRHIRef Texture, uint64 PolicyKey, uint64 CloudKey) -> void;
		RENDERER_API auto SetPendingClear(
			uint64 PolicyKey, uint64 CloudKey
		) -> void;
		RENDERER_API auto TakeReusable(uint32 Width, uint32 Height)
			-> FTextureRHIRef;
		RENDERER_API auto Commit() -> void;
		RENDERER_API auto Abort() -> void;
		RENDERER_API auto Reset() -> void;
		[[nodiscard]] RENDERER_API auto GetRetainedBytes() const -> uint64;
	};

	// Owns transactional metadata and feature-local history for one logical stream.
	class FSceneViewState
	{
	public:
		RENDERER_API auto Begin(
			const FSceneViewTemporalMetadata& Current,
			uint64 SubmissionSerial,
			bool bDiscardHistory
		) -> FSceneViewTemporalContext;
		RENDERER_API auto Commit() -> void;
		RENDERER_API auto Abort() -> void;
		RENDERER_API auto Invalidate(ESceneViewDiscontinuity Cause) -> void;
		auto GetHistoryProbe() -> FSceneViewHistoryProbe& { return HistoryProbe; }
		auto GetVolumetricCloudHistory() -> FVolumetricCloudViewHistory&
		{
			return VolumetricCloudHistory;
		}
		auto IsSubmissionActive() const -> bool { return bSubmissionActive; }

	private:
		std::optional<FSceneViewTemporalMetadata> Committed;
		std::optional<FSceneViewTemporalMetadata> Pending;
		uint64 LastSuccessfulSubmissionSerial = 0;
		uint64 PendingSubmissionSerial = 0;
		uint64 SuccessfulSequence = 0;
		ESceneViewDiscontinuity PendingInvalidation =
			ESceneViewDiscontinuity::None;
		bool bSubmissionActive = false;
		FSceneViewHistoryProbe HistoryProbe;
		FVolumetricCloudViewHistory VolumetricCloudHistory;
	};

	// Render-thread-only registry rejects any identity it did not explicitly create.
	class FSceneViewStateRegistry
	{
	public:
		RENDERER_API auto Add(FSceneViewStateId Id) -> bool;
		RENDERER_API auto Remove(FSceneViewStateId Id) -> bool;
		RENDERER_API auto Find(FSceneViewStateId Id) -> FSceneViewState*;
		RENDERER_API auto Invalidate(
			FSceneViewStateId Id,
			ESceneViewDiscontinuity Cause
		) -> bool;
		RENDERER_API auto InvalidateAll(ESceneViewDiscontinuity Cause) -> void;
		RENDERER_API auto ReleaseAll() -> size_t;
		auto Num() const -> size_t { return States.size(); }

	private:
		std::map<FSceneViewStateId, FSceneViewState> States;
	};

	struct FSceneViewStateIdAccess
	{
		static auto GetValue(FSceneViewStateId Id) -> uint64 { return Id.Value; }
		static auto Make(uint64 Value) -> FSceneViewStateId
		{
			return FSceneViewStateId(Value);
		}
	};

	struct FSceneViewStateOwnerTestAccess
	{
		static auto Make(
			FSceneViewStateId Id,
			FSceneViewStateOwner::FReleaseViewState Release
		)
			-> FSceneViewStateOwner
		{
			return FSceneViewStateOwner(Id, Release);
		}
	};

	RENDERER_API auto BuildSceneViewTemporalMetadata(
		const FSceneView& View,
		const FScene* Scene,
		uint32 OutputWidth,
		uint32 OutputHeight
	) -> FSceneViewTemporalMetadata;
} // namespace Durin
