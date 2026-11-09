#pragma once

#include "CoreMinimal.h"

#include "Asset/Load.h"

namespace Durin
{
	class FAssetLoadRequestService;
	enum class EAssetLoadState : uint8 { Pending, Loading, Succeeded, Failed, Cancelled };
	struct FStreamableLoadResult
	{
		DPackage* Package = nullptr;
		DObject* Object = nullptr;
	};
	class FStreamableHandle;
	using FStreamableDelegate = std::function<void(FStreamableHandle&)>;

	// GameThread-only. Active handles retain their loaded package and selected object.
	// Release/destruction drops only this handle's references and schedules GC.
	class FStreamableHandle
	{
	public:
		ENGINE_API ~FStreamableHandle();
		FStreamableHandle(const FStreamableHandle&) = delete;
		auto operator=(const FStreamableHandle&) -> FStreamableHandle& = delete;
		ENGINE_API auto GetState() const -> EAssetLoadState;
		ENGINE_API auto IsComplete() const -> bool;
		ENGINE_API auto IsActive() const -> bool;
		// Requires IsComplete(); pending/loading have no terminal result.
		// Released success results observe weak identities; collected objects are null.
		[[nodiscard]] ENGINE_API auto GetResult() const -> std::expected<FStreamableLoadResult, FAssetReadError>;
		ENGINE_API auto GetReport() const -> const FAssetLoadReport&;
		ENGINE_API auto GetLoadedPackage() const -> DPackage*;
		ENGINE_API auto GetLoadedObject() const -> DObject*;
		// During loading or queued completion, release is deferred until callback delivery.
		ENGINE_API auto ReleaseHandle() -> void;
		// Suppresses this request's completion and releases its retention; peers are unaffected.
		ENGINE_API auto CancelHandle() -> void;
	private:
		FStreamableHandle();
		auto ReleaseRetention() -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend class FAssetLoadRequestService;
	};

	// Sync and async requests share the same retention contract. Neither releasing
	// a handle nor cancelling a request forces collection or unloads another owner.
	ENGINE_API auto RequestSyncLoad(const FPackagePath& Path) -> std::shared_ptr<FStreamableHandle>;
	ENGINE_API auto RequestSyncLoad(const FObjectPath& Path, const DClass* ExpectedClass = nullptr)
		-> std::shared_ptr<FStreamableHandle>;
	ENGINE_API auto RequestSyncLoad(const FTopLevelAssetPath& Path, const DClass* ExpectedClass = nullptr)
		-> std::shared_ptr<FStreamableHandle>;

	// Deferred, including resident hits and ordinary errors. Lifetime/guard rejection
	// returns a failed handle without a callback. Higher priority starts first.
	// Completion means object readiness, not compilation or render-resource readiness.
	ENGINE_API auto RequestAsyncLoad(const FPackagePath& Path,
		FStreamableDelegate OnComplete = {}, int32 Priority = 0)
		-> std::shared_ptr<FStreamableHandle>;
	ENGINE_API auto RequestAsyncLoad(const FObjectPath& Path,
		FStreamableDelegate OnComplete = {}, const DClass* ExpectedClass = nullptr,
		int32 Priority = 0) -> std::shared_ptr<FStreamableHandle>;
	ENGINE_API auto RequestAsyncLoad(const FTopLevelAssetPath& Path,
		FStreamableDelegate OnComplete = {}, const DClass* ExpectedClass = nullptr,
		int32 Priority = 0) -> std::shared_ptr<FStreamableHandle>;

	// EngineLoop pumps this automatically. Tools/tests must pump on GameThread.
	// Budget is checked between completions; object graph application is currently atomic.
	ENGINE_API auto ProcessAsyncLoading(double TimeBudgetMilliseconds = 2.0,
		uint32 MaximumCompletions = 1) -> void;
	// Cancels all requests/deliveries and drains background reads. Shutdown/tools only;
	// normal UI cancellation uses the individual handle's nonblocking CancelHandle().
	ENGINE_API auto CancelAsyncLoading() -> void;
}
