#pragma once

#include "CoreMinimal.h"

#include "SceneView.h"
#include "Thumbnail/AssetThumbnailKey.h"
#include "DObject/Object.h"

#include "ThumbnailRenderer.gen.h"

namespace Durin
{
	class DThumbnailManager;
	class DThumbnailRenderer;
	class IThumbnailRendererSession;
	namespace ThumbnailPrivate { struct FAssetThumbnailGenerationLeaseState; }

	// Provides cooperative cancellation shared by scheduler and asynchronous completions.
	class FAssetThumbnailCancellation
	{
	public:
		DURINED_API FAssetThumbnailCancellation();

		DURINED_API auto Cancel() const -> void;
		DURINED_API auto IsCancelled() const -> bool;

	private:
		std::shared_ptr<std::atomic<bool>> State;
	};

	// Owns renderer-specific immutable data captured on the game thread.
	class IAssetThumbnailGenerationInput
	{
	public:
		virtual ~IAssetThumbnailGenerationInput() = default;
	};

	// Keeps renderer-owned request data behind a core-owned invalidation boundary.
	// Copies share only DurinEd state; renderer removal clears the input and session
	// from every copy before the scoped registration handle returns.
	class FAssetThumbnailGenerationLease
	{
	public:
		FAssetThumbnailGenerationLease() = default;

		DURINED_API auto IsActive() const -> bool;
		DURINED_API auto GetInput() const -> const IAssetThumbnailGenerationInput*;
		DURINED_API auto GetRenderedSession() const
			-> IThumbnailRendererSession*;
		DURINED_API auto ReleaseRenderedSession() const -> void;

	private:
		friend class DThumbnailManager;
		friend struct FAssetThumbnailGenerationRequest;
		explicit FAssetThumbnailGenerationLease(
			std::shared_ptr<ThumbnailPrivate::FAssetThumbnailGenerationLeaseState> InState)
			: State(std::move(InState))
		{
		}

		std::shared_ptr<ThumbnailPrivate::FAssetThumbnailGenerationLeaseState> State;
	};

	// Transfers an immutable renderer request across worker and render-thread boundaries.
	struct FAssetThumbnailGenerationRequest
	{
		FAssetThumbnailKeyInput KeyInput;
		// Capture-only transfer slot. The registry moves this into RendererLease
		// before a request enters renderer-neutral scheduling.
		std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
		FAssetThumbnailGenerationLease RendererLease;
		FAssetThumbnailCancellation Cancellation;
		uint64 RendererGeneration = 0;
		uint64 RequestSerial = 0;
		// Renderer-selected UI compositing policy; it does not affect persistent identity.
		bool bHasTransparency = true;

		DURINED_API auto GetInput() const -> const IAssetThumbnailGenerationInput*;
		DURINED_API auto BeginRenderedSession() const
			-> std::expected<IThumbnailRendererSession*, std::string>;
		DURINED_API auto GetRenderedSession() const
			-> IThumbnailRendererSession*;
		DURINED_API auto ReleaseRenderedSession() const -> void;
	};

	// Captures renderer-owned immutable generation data on the game thread.
	// Registration and removal are game-thread operations; removal first cancels
	// and drains captured requests.
	DCLASS(Abstract)
	class DThumbnailRenderer : public DObject
	{
		GENERATED_BODY()

	public:
		virtual ~DThumbnailRenderer() = default;

		virtual auto GetRegistration() const -> FThumbnailRenderingInfo = 0;
		virtual auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration) -> std::expected<FAssetThumbnailGenerationRequest, std::string> = 0;
		// Creates renderer-owned state for a persistent-cache miss.
		DURINED_API virtual auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest&,
			const IAssetThumbnailGenerationInput&)
			-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string>;
	protected:
		DURINED_API explicit DThumbnailRenderer(
			const FObjectInitializer& ObjectInitializer);
	};

	class DWorld;
	class AActor;

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
		// The scene owns reusable fixtures; sessions must detach asset bindings on reset.
		virtual auto GetReusableActor(std::string_view Key) -> AActor* { return nullptr; }
		virtual auto SetView(
			const FThumbnailPreviewView& View) -> std::expected<void, std::string> = 0;
		virtual auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment) -> std::expected<void, std::string> = 0;
		virtual auto SetImageRenderer(FThumbnailImageRenderer Renderer) -> std::expected<void, std::string>
		{
			return std::unexpected("This preview scene does not support image rendering.");
		}
	};

	// Owns all renderer-specific state for one persistent-cache miss.
	// Load submits nonblocking asset requests. PollResources captures loaded inputs once
	// and advances nonblocking resource requests until ready. PreparePreview
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
			IThumbnailPreviewScene& PreviewScene) -> std::expected<void, std::string> = 0;
		virtual auto ValidatePreparedInput() const -> std::expected<void, std::string> = 0;
		virtual auto ResetPreview() -> void = 0;
	};

} // namespace Durin
