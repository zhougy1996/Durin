#pragma once

#include "CoreMinimal.h"

#include "Asset/Load.h"

namespace Durin
{
	class FAssetLoadRequestService;
	class FStreamableManager;
	enum class EAssetLoadState : uint8 { Pending, Loading, Succeeded, Failed, Cancelled };
	class FStreamableHandle;
	using FStreamableDelegate = std::function<void(FStreamableHandle&)>;

	// GameThread-only. Active handles retain their loaded package and selected object.
	// Release/destruction drops only this handle's references and schedules GC.
	class FStreamableHandle : public std::enable_shared_from_this<FStreamableHandle>
	{
	public:
		ENGINE_API ~FStreamableHandle();
		FStreamableHandle(const FStreamableHandle&) = delete;
		auto operator=(const FStreamableHandle&) -> FStreamableHandle& = delete;
		ENGINE_API auto GetState() const -> EAssetLoadState;
		ENGINE_API auto IsComplete() const -> bool;
		ENGINE_API auto IsActive() const -> bool;
		ENGINE_API auto HasLoadCompleted() const -> bool;
		ENGINE_API auto WasCanceled() const -> bool;
		ENGINE_API auto HasError() const -> bool;
		// None when no error; aggregate error is the first failed input slot.
		ENGINE_API auto GetError() const -> const FAssetReadError&;
		ENGINE_API auto GetTargetErrors() const -> std::vector<FAssetReadError>;
		ENGINE_API auto GetLoadReports() const -> std::vector<FAssetLoadReport>;
		ENGINE_API auto GetRequestedAssets() const -> const std::vector<FSoftObjectPath>&;
		// Input ordering and duplicates are preserved, including null failure slots.
		ENGINE_API auto GetLoadedAssets() const -> std::vector<DObject*>;
		ENGINE_API auto GetDebugName() const -> const std::string&;
		ENGINE_API auto GetOwningManager() const -> FStreamableManager*;
		ENGINE_API auto IsManaged() const -> bool;
		ENGINE_API auto GetReport() const -> const FAssetLoadReport&;
		ENGINE_API auto GetLoadedPackage() const -> DPackage*;
		ENGINE_API auto GetLoadedAsset() const -> DObject*;
		// During loading or queued completion, release is deferred until callback delivery.
		ENGINE_API auto ReleaseHandle() -> void;
		// Suppresses this request's completion and releases its retention; peers are unaffected.
		ENGINE_API auto CancelHandle() -> void;
	private:
		FStreamableHandle();
		auto ReleaseRetention() -> void;
		// Engine policy publication runs at load completion, before parent notification.
		// Public completion delegates remain independently deferred.
		ENGINE_API auto SetLoadCompletedCallback(FStreamableDelegate Callback) -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend class FAssetLoadRequestService;
		friend class FStreamableManager;
		friend class DAssetManager;
	};

	// EngineLoop pumps this automatically. Tools/tests must pump on GameThread.
	// Budget is checked between completions; object graph application is currently atomic.
	ENGINE_API auto ProcessAsyncLoading(double TimeBudgetMilliseconds = 2.0,
		uint32 MaximumCompletions = 1) -> void;
	// Cancels all requests/deliveries and drains background reads. Shutdown/tools only;
	// normal UI cancellation uses the individual handle's nonblocking CancelHandle().
	ENGINE_API auto CancelAsyncLoading() -> void;
}
