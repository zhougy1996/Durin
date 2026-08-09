#pragma once

#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	class DWorld;

	// Reports the provider-owned cold-generation state observed by the shared core.
	enum class ERenderedAssetThumbnailSessionState : uint8
	{
		WaitingForResources,
		ReadyToRender,
		Failed
	};

	// Transfers only revisions and diagnostics across the extension boundary.
	struct FRenderedAssetThumbnailSessionUpdate
	{
		ERenderedAssetThumbnailSessionState State =
			ERenderedAssetThumbnailSessionState::Failed;
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;
		std::string Diagnostic;
	};

	// Describes a provider-selected camera without exposing concrete asset types.
	struct FRenderedAssetThumbnailPreviewView
	{
		std::array<double, 3> CameraPosition{};
		std::array<double, 3> CameraForward{1.0, 0.0, 0.0};
		std::array<double, 3> CameraRight{0.0, 1.0, 0.0};
		std::array<double, 3> CameraUp{0.0, 0.0, 1.0};
		double VerticalFieldOfViewDegrees = 42.0;
		double NearClipDistance = 0.1;
		double FarClipDistance = 100.0;
		bool bForceLOD0 = false;
		float ClearRed = 0.0f;
		float ClearGreen = 0.0f;
		float ClearBlue = 0.0f;
		float ClearAlpha = 0.0f;
	};

	// Exposes the leased shared preview world and provider-neutral camera setup.
	// The extension may attach only session-owned content and must remove it in ResetPreview.
	class IRenderedAssetThumbnailPreviewScene
	{
	public:
		virtual ~IRenderedAssetThumbnailPreviewScene() = default;

		virtual auto GetWorld() -> DWorld* = 0;
		virtual auto SetView(
			const FRenderedAssetThumbnailPreviewView& View,
			std::string& OutError) -> bool = 0;
	};

	// Owns all provider-specific state for one persistent-cache miss.
	// Every method runs on the game thread. ResetPreview is idempotent and is called
	// before the session can be destroyed by normal completion or provider removal.
	class IRenderedAssetThumbnailGenerationSession
	{
	public:
		virtual ~IRenderedAssetThumbnailGenerationSession() = default;

		virtual auto Load() -> FRenderedAssetThumbnailSessionUpdate = 0;
		virtual auto PollResources() -> FRenderedAssetThumbnailSessionUpdate = 0;
		virtual auto PreparePreview(
			IRenderedAssetThumbnailPreviewScene& PreviewScene,
			std::string& OutError) -> bool = 0;
		virtual auto ValidateRevisions(
			uint64 ExpectedAssetRevision,
			uint64 ExpectedResourceRevision,
			std::string& OutError) const -> bool = 0;
		virtual auto ResetPreview() -> void = 0;
	};

	// Extends exact-class request capture with provider-owned cold-generation sessions.
	// Persistent hits never create a session.
	class IRenderedAssetThumbnailExtension : public IAssetThumbnailProvider
	{
	public:
		virtual auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest& Request,
			const IAssetThumbnailGenerationInput& Input,
			std::string& OutError)
			-> std::unique_ptr<IRenderedAssetThumbnailGenerationSession> = 0;
	};
} // namespace Durin
