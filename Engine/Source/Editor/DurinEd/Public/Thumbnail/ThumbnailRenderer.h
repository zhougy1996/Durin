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

	// Reports readiness and diagnostics; input ownership and validation stay in the session.
	struct FThumbnailRendererSessionUpdate
	{
		EThumbnailRendererSessionState State =
			EThumbnailRendererSessionState::Failed;

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
	// Optional image renderers return SRGBA8 under the same capture/readback budget as scenes.
	using FThumbnailImageRenderer = std::function<FTextureRHIRef(
		FRHICommandListImmediate&, uint32, uint32)>;

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
		virtual auto SetImageRenderer(FThumbnailImageRenderer Renderer,
			std::string& OutError) -> bool
		{
			OutError = "This preview scene does not support image rendering.";
			return false;
		}
	};

	// Owns all renderer-specific state for one persistent-cache miss.
	// Load captures authored inputs. PollResources only observes readiness. PreparePreview
	// captures the resource inputs retained until ResetPreview; later polling must not
	// replace them. ValidatePreparedInput rejects missing, reset, or changed inputs.
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
		virtual auto ValidatePreparedInput(
			std::string& OutError) const -> bool = 0;
		virtual auto ResetPreview() -> void = 0;
	};

	} // namespace Editor
} // namespace Durin
