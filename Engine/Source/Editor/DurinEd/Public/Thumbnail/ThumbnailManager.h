#pragma once

#include "Thumbnail/AssetThumbnailKey.h"
#include "Modules/ModularFeature.h"
#include "DObject/Object.h"

#include "ThumbnailManager.gen.h"

namespace Durin
{
	namespace Editor
	{
		class IThumbnailRendererSession;
		class FAssetThumbnailPool;

		namespace Detail
		{
			struct FAssetThumbnailGenerationLeaseState;
			struct DThumbnailManagerState;
		}
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

	// Carries renderer-generated canonical RGBA8 pixels through the shared cache pipeline.
	struct FAssetThumbnailGeneratedPixels
	{
		FByteArray Pixels;
		uint32 Width = 0;
		uint32 Height = 0;
		uint64 AssetRevision = 0;
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
			std::shared_ptr<Detail::FAssetThumbnailGenerationLeaseState> InState)
			: State(std::move(InState))
		{
		}

		std::shared_ptr<Detail::FAssetThumbnailGenerationLeaseState> State;
	};

	// Transfers an immutable renderer request across worker and render-thread boundaries.
	struct FAssetThumbnailGenerationRequest
	{
		FAssetThumbnailKeyInput KeyInput;
		// Capture-only transfer slot. The registry moves this into RendererLease
		// before a request enters renderer-neutral scheduling.
		std::shared_ptr<const IAssetThumbnailGenerationInput> Input;
		std::shared_ptr<const FAssetThumbnailGeneratedPixels> GeneratedPixels;
		FAssetThumbnailGenerationLease RendererLease;
		FAssetThumbnailCancellation Cancellation;
		uint64 RendererGeneration = 0;
		uint64 RequestSerial = 0;
		// Renderer-selected UI compositing policy; it does not affect persistent identity.
		bool bHasTransparency = true;
		// Renderer snapshots revalidated after asset loading and before every resource-dependent publication.
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;

		DURINED_API auto GetInput() const -> const IAssetThumbnailGenerationInput*;
		DURINED_API auto BeginRenderedSession(std::string& OutError) const
			-> IThumbnailRendererSession*;
		DURINED_API auto GetRenderedSession() const
			-> IThumbnailRendererSession*;
		DURINED_API auto ReleaseRenderedSession() const -> void;
	};

	// Describes one exact-class renderer registration and its generator schema.
	struct FThumbnailRenderingInfo
	{
		std::string AssetClassName;
		std::string RendererName;
		uint32 GeneratorSchemaVersion = 0;
	};

	// Captures renderer-owned immutable generation data on the game thread.
	// Registration and removal are game-thread operations; removal first cancels
	// and drains captured requests.
	DCLASS(Abstract)
	class DThumbnailRenderer : public DObject
	{
		GENERATED_BODY()

	public:
		DURINED_API DThumbnailRenderer();
		virtual ~DThumbnailRenderer() = default;

		virtual auto GetRegistration() const -> FThumbnailRenderingInfo = 0;
		virtual auto CaptureGenerationRequest(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			std::string& OutError) -> bool = 0;
		// Renderers that require a preview scene override this cold-miss hook;
		// canonical-pixel renderers leave it unsupported.
		DURINED_API virtual auto CreateGenerationSession(
			const FAssetThumbnailGenerationRequest&,
			const IAssetThumbnailGenerationInput&,
			std::string& OutError) -> std::unique_ptr<IThumbnailRendererSession>;
	protected:
		DURINED_API explicit DThumbnailRenderer(
			const FObjectInitializer& ObjectInitializer);
	};

	// Retains one exact-class renderer together with the generation captured at registration.
	struct FThumbnailRendererHandle
	{
		uint64 Generation = 0;

		explicit operator bool() const { return Generation != 0; }
	};

	// Owns one exact-class renderer registration. Reset first closes admission,
	// invalidates captured generations, and releases renderer-owned inputs/sessions.
	class FThumbnailRendererRegistrationHandle
	{
	public:
		FThumbnailRendererRegistrationHandle() = default;
		DURINED_API ~FThumbnailRendererRegistrationHandle();
		FThumbnailRendererRegistrationHandle(
			const FThumbnailRendererRegistrationHandle&) = delete;
		auto operator=(const FThumbnailRendererRegistrationHandle&)
			-> FThumbnailRendererRegistrationHandle& = delete;
		DURINED_API FThumbnailRendererRegistrationHandle(
			FThumbnailRendererRegistrationHandle&& Other) noexcept;
		DURINED_API auto operator=(
			FThumbnailRendererRegistrationHandle&& Other) noexcept
			-> FThumbnailRendererRegistrationHandle&;

		auto IsValid() const -> bool { return RegistrationId != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		DURINED_API auto Reset() -> void;

	private:
		friend class DThumbnailManager;
		FThumbnailRendererRegistrationHandle(
			std::weak_ptr<Detail::DThumbnailManagerState> InState,
			uint64 InRegistrationId)
			: State(std::move(InState))
			, RegistrationId(InRegistrationId)
		{
		}

		std::weak_ptr<Detail::DThumbnailManagerState> State;
		uint64 RegistrationId = 0;
	};

	// Owns exact-class renderer registration and monotonically increasing renderer generations.
	class DThumbnailManager
	{
	public:
		DURINED_API DThumbnailManager();
		DURINED_API ~DThumbnailManager();

		DThumbnailManager(const DThumbnailManager&) = delete;
		DThumbnailManager& operator=(const DThumbnailManager&) = delete;

		DURINED_API auto Register(
			std::shared_ptr<DThumbnailRenderer> Renderer,
			FModuleOwnedCallbackGate OwnerGate,
			std::string& OutError) -> bool;
		// Process-owned/test renderers only; unloadable modules must pass an owner gate.
		auto Register(std::shared_ptr<DThumbnailRenderer> Renderer,
			std::string& OutError) -> bool
		{
			return Register(std::move(Renderer), {}, OutError);
		}
		DURINED_API auto RegisterScoped(
			std::unique_ptr<DThumbnailRenderer> Renderer,
			FModuleOwnedCallbackGate OwnerGate,
			std::string& OutError) -> FThumbnailRendererRegistrationHandle;
		// Process-owned/test renderers only; unloadable modules must pass an owner gate.
		auto RegisterScoped(std::unique_ptr<DThumbnailRenderer> Renderer,
			std::string& OutError) -> FThumbnailRendererRegistrationHandle
		{
			return RegisterScoped(std::move(Renderer), {}, OutError);
		}
		DURINED_API auto Unregister(std::string_view AssetClassName, std::string& OutError) -> bool;
		DURINED_API auto Find(std::string_view AssetClassName) const -> FThumbnailRendererHandle;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto IsShuttingDown() const -> bool;
		DURINED_API auto Num() const -> size_t;
		// Lazily creates the manager-owned process pool. Tests may still inject a
		// local manager into an independently constructed pool.
		DURINED_API auto GetSharedPool() -> FAssetThumbnailPool&;
		// Drains and destroys the process pool without retiring renderer registrations.
		// MainFrame uses this after releasing browser references and unregistering modules.
		DURINED_API auto ResetSharedPool() -> void;

	private:
		friend class FAssetThumbnailRequestQueue;
		auto Capture(
			const FAssetThumbnailRequest& Request,
			uint64 RendererGeneration,
			FAssetThumbnailGenerationRequest& OutRequest,
			FThumbnailRenderingInfo& OutRegistration,
			std::string& OutError) -> bool;

		std::shared_ptr<Detail::DThumbnailManagerState> State;
		std::unique_ptr<FAssetThumbnailPool> SharedPool;
	};

	// Returns the process registry composed by MainFrame and shared by default caches.
	DURINED_API auto GetDefaultThumbnailManager()
		-> DThumbnailManager&;

	} // namespace Editor
} // namespace Durin
