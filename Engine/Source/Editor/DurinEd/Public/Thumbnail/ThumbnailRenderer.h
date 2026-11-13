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
	class FThumbnailPreviewScene;
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

	// Immutable feature data; scheduler identity and lifetime stay in the manager.
	struct FThumbnailGenerationData
	{
		FAssetThumbnailOutputSettings Output;
		std::string PreviewFixtureIdentity;
		uint32 PreviewFixtureVersion = 0;
		uint32 ShaderContractVersion = 0;
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
		bool bHasTransparency = true;

		DURINED_API auto MakeKeyInput(const FAssetThumbnailPackageFingerprint& Asset,
			const FThumbnailRenderingInfo& Renderer) const -> FAssetThumbnailKeyInput;
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
		DURINED_API ~DThumbnailRenderer() override;
		DURINED_API auto BeginDestroy() -> void override;
		// Lazily constructed; never needed for a cache hit or a 2D image renderer.
		DURINED_API auto GetPreviewScene() -> FThumbnailPreviewScene&;

		virtual auto GetRegistration() const -> FThumbnailRenderingInfo = 0;
		virtual auto CaptureGenerationInput(const FAssetThumbnailPackageFingerprint& Asset) -> std::expected<FThumbnailGenerationData, std::string> = 0;
		// Creates renderer-owned state for a persistent-cache miss.
		DURINED_API virtual auto CreateGenerationSession(const IAssetThumbnailGenerationInput&)
			-> std::unique_ptr<IThumbnailRendererSession>;
	protected:
		DURINED_API explicit DThumbnailRenderer(
			const FObjectInitializer& ObjectInitializer);
	private:
		std::unique_ptr<FThumbnailPreviewScene> PreviewScene;
	};

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

	// Supplies capture output and view setup without owning renderer scenes.
	// Optional image renderers return SRGBA8 under the same capture/readback budget as scenes.
	using FThumbnailImageRenderer = std::function<FTextureRHIRef(
		FRHICommandListImmediate&, uint32, uint32)>;

	// A 3D session binds its renderer-owned scene; an image session needs no scene.
	class IThumbnailRenderContext
	{
	public:
		virtual ~IThumbnailRenderContext() = default;

		virtual auto SetScene(FThumbnailPreviewScene& Scene) -> std::expected<void, std::string> = 0;
		virtual auto SetView(
			const FThumbnailPreviewView& View) -> void = 0;
		virtual auto SetViewEnvironment(
			const FViewEnvironmentOverride& Environment) -> void = 0;
		virtual auto SetImageRenderer(FThumbnailImageRenderer Renderer) -> void
		{
			checkf(false, "This capture context does not support image rendering.");
		}
	};

	// Owns all renderer-specific state for one persistent-cache miss.
	// Update starts nonblocking asset requests once, captures loaded inputs once
	// and advances nonblocking resource requests until ready. PreparePreview
	// captures the resource inputs retained until ResetPreview; later polling must not
	// replace them. ValidatePreparedInput rejects missing, reset, or changed inputs.
	// Every method runs on the game thread. ResetPreview is idempotent and is called
	// before destruction. Reset is terminal; Update never restarts a reset session.
	class IThumbnailRendererSession
	{
	public:
		virtual ~IThumbnailRendererSession() = default;

		virtual auto Update() -> FThumbnailRendererSessionUpdate = 0;
		virtual auto PreparePreview(
			IThumbnailRenderContext& Context) -> std::expected<void, std::string> = 0;
		virtual auto ValidatePreparedInput() const -> std::expected<void, std::string> = 0;
		virtual auto ResetPreview() -> void = 0;
	};

} // namespace Durin
