#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/AssetThumbnailTypes.h"

namespace Durin
{
	class DThumbnailRenderer;
	struct FAssetThumbnailGenerationRequest;
	class FAssetThumbnailPool;

	namespace ThumbnailPrivate
	{
		struct DThumbnailManagerState;
	}
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
			std::weak_ptr<ThumbnailPrivate::DThumbnailManagerState> InState,
			uint64 InRegistrationId)
			: State(std::move(InState))
			, RegistrationId(InRegistrationId)
		{
		}

		std::weak_ptr<ThumbnailPrivate::DThumbnailManagerState> State;
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

		// Unregister on GameThread after stopping thumbnail consumers and draining work,
		// before unloading renderer code. Removal invalidates outstanding sessions.
		DURINED_API auto Register(
			std::shared_ptr<DThumbnailRenderer> Renderer) -> std::expected<void, std::string>;
		DURINED_API auto RegisterScoped(
			std::unique_ptr<DThumbnailRenderer> Renderer)
			-> std::expected<FThumbnailRendererRegistrationHandle, std::string>;
		// Reports whether a registration was removed; absence needs no diagnostic.
		DURINED_API auto Unregister(std::string_view AssetClassName) -> bool;
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
			uint64 RendererGeneration) -> std::expected<FAssetThumbnailGenerationRequest, std::string>;

		std::shared_ptr<ThumbnailPrivate::DThumbnailManagerState> State;
		std::unique_ptr<FAssetThumbnailPool> SharedPool;
	};

	// Returns the process registry composed by MainFrame and shared by default caches.
	DURINED_API auto GetDefaultThumbnailManager()
		-> DThumbnailManager&;

} // namespace Durin
