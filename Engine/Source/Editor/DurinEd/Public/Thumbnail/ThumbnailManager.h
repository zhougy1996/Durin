#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/AssetThumbnailTypes.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DThumbnailRenderer;
	class FThumbnailRenderJob;
	class DObject;
	class DClass;
	struct FThumbnailManagerTestAccess;
	struct FAssetThumbnailScheduledRequest;
	struct FThumbnailRenderingInfo;
	class FAssetThumbnailPool;

	namespace ThumbnailPrivate
	{
		struct DThumbnailManagerState;
	}
	// Owns one exact-class renderer registration. Reset first closes admission,
	// cancels queued work and notifies pools to retire draw jobs.
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

		auto IsValid() const -> bool { return !AssetClassName.empty() && Renderer.IsValid() && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		DURINED_API auto Reset() -> void;

	private:
		friend class DThumbnailManager;
		friend struct FThumbnailManagerTestAccess;
		DURINED_API FThumbnailRendererRegistrationHandle(
			std::weak_ptr<ThumbnailPrivate::DThumbnailManagerState> InState,
			std::string InAssetClassName, DThumbnailRenderer* InRenderer);
		std::weak_ptr<ThumbnailPrivate::DThumbnailManagerState> State;
		std::string AssetClassName;
		TWeakObjectPtr<DObject> Renderer;
	};

	// Owns exact-class renderer registrations and the shared resource pool.
	class DThumbnailManager
	{
	public:
		DURINED_API DThumbnailManager();
		DURINED_API ~DThumbnailManager();

		DThumbnailManager(const DThumbnailManager&) = delete;
		DThumbnailManager& operator=(const DThumbnailManager&) = delete;

		// Unregister on GameThread after stopping thumbnail consumers and draining work,
		// before unloading renderer code. Removal notifies pools to cancel outstanding work.
		// The manager creates a transient renderer of the registered reflected class.
		// Returns an empty handle on failure; successful handles own registration.
		DURINED_API auto RegisterCustomRenderer(DClass* AssetClass, DClass* RendererClass)
			-> FThumbnailRendererRegistrationHandle;
		DURINED_API auto UnregisterCustomRenderer(DClass* AssetClass) -> bool;
		DURINED_API auto GetRenderingInfo(DClass* AssetClass) const -> std::optional<FThumbnailRenderingInfo>;
		DURINED_API auto Find(std::string_view AssetClassName) const -> DThumbnailRenderer*;
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
		friend class FAssetThumbnailPool;
		auto AttachQueue(class FAssetThumbnailRequestQueue* Queue) -> void;
		auto DetachQueue(class FAssetThumbnailRequestQueue* Queue) -> void;
		auto AttachPool(FAssetThumbnailPool* Pool) -> void;
		auto DetachPool(FAssetThumbnailPool* Pool) -> void;
		DURINED_API auto CreateRenderJob(const FAssetThumbnailScheduledRequest& Request) -> std::unique_ptr<FThumbnailRenderJob>;
		DURINED_API auto SetLoadedObjectOverride(std::string_view Name, DObject* Object) -> void;
		friend struct FThumbnailManagerTestAccess;
		DURINED_API auto RegisterRendererInstance(DThumbnailRenderer* Renderer, std::string AssetClassName, DClass* AssetClass = nullptr)
			-> FThumbnailRendererRegistrationHandle;
		DURINED_API auto UnregisterRendererName(std::string_view AssetClassName) -> bool;

		std::shared_ptr<ThumbnailPrivate::DThumbnailManagerState> State;
		std::unique_ptr<FAssetThumbnailPool> SharedPool;
	};

	// Returns the process registry composed by MainFrame and shared by default caches.
	DURINED_API auto GetDefaultThumbnailManager()
		-> DThumbnailManager&;

} // namespace Durin
