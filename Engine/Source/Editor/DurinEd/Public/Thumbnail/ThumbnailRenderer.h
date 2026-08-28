#pragma once

#include "SceneView.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin
{
	class DWorld;

	namespace Editor
	{

	// Reports the renderer-owned cold-generation state observed by the shared core.
	enum class EThumbnailRendererSessionState : uint8
	{
		WaitingForResources,
		ReadyToRender,
		Failed
	};

	// Transfers only revisions and diagnostics across the extension boundary.
	struct FThumbnailRendererSessionUpdate
	{
		EThumbnailRendererSessionState State =
			EThumbnailRendererSessionState::Failed;
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;
		std::string Diagnostic;
	};

	// Describes a renderer-selected camera without exposing concrete asset types.
	struct FThumbnailPreviewView
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

	// Exposes the leased shared preview world and renderer-neutral view setup.
	// The extension may attach only session-owned content and must remove it in ResetPreview.
	class IThumbnailPreviewScene
	{
	public:
		virtual ~IThumbnailPreviewScene() = default;

		virtual auto GetWorld() -> DWorld* = 0;
		virtual auto SetView(
			const FThumbnailPreviewView& View,
			std::string& OutError) -> bool = 0;
		virtual auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment,
			std::string& OutError) -> bool = 0;
	};

	// Owns all renderer-specific state for one persistent-cache miss.
	// Every method runs on the game thread. ResetPreview is idempotent and is called
	// before the session can be destroyed by normal completion or renderer removal.
	class IThumbnailRendererSession
	{
	public:
		virtual ~IThumbnailRendererSession() = default;

		virtual auto Load() -> FThumbnailRendererSessionUpdate = 0;
		virtual auto PollResources() -> FThumbnailRendererSessionUpdate = 0;
		virtual auto PreparePreview(
			IThumbnailPreviewScene& PreviewScene,
			std::string& OutError) -> bool = 0;
		virtual auto ValidateRevisions(
			uint64 ExpectedAssetRevision,
			uint64 ExpectedResourceRevision,
			std::string& OutError) const -> bool = 0;
		virtual auto ResetPreview() -> void = 0;
	};

	} // namespace Editor
} // namespace Durin
