#pragma once

#include "Asset/Load.h"

namespace Durin
{
	class FAsyncAssetLoadService;
	enum class EAsyncLoadState : uint8 { Pending, Loading, Succeeded, Failed, Cancelled };
	struct FAsyncLoadedAssets
	{
		DPackage* Package = nullptr;
		DObject* Object = nullptr;
	};
	class FAsyncLoadHandle;
	using FAsyncLoadCallback = std::function<void(const FAsyncLoadHandle&)>;

	// GameThread-only, including observation, cancellation and destruction. A successful
	// handle retains its package and selected object until the handle is destroyed.
	class FAsyncLoadHandle
	{
	public:
		ENGINE_API ~FAsyncLoadHandle();
		FAsyncLoadHandle(const FAsyncLoadHandle&) = delete;
		auto operator=(const FAsyncLoadHandle&) -> FAsyncLoadHandle& = delete;
		ENGINE_API auto GetState() const -> EAsyncLoadState;
		ENGINE_API auto IsComplete() const -> bool;
		// Requires IsComplete(); pending/loading have no terminal result.
		[[nodiscard]] ENGINE_API auto GetResult() const -> const std::expected<FAsyncLoadedAssets, FAssetReadError>&;
		ENGINE_API auto GetReport() const -> const FAssetLoadReport&;
		ENGINE_API auto GetLoadedPackage() const -> DPackage*;
		ENGINE_API auto GetLoadedObject() const -> DObject*;
		// Suppresses completion delivery for this request; shared requests are unaffected.
		ENGINE_API auto Cancel() -> void;
	private:
		FAsyncLoadHandle();
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend class FAsyncAssetLoadService;
	};

	// Deferred, including resident hits and ordinary errors. Lifetime/guard rejection
	// returns a failed handle without a callback. Higher priority starts first.
	// Completion means object readiness, not compilation or render-resource readiness.
	ENGINE_API auto LoadPackageAsync(const FPackagePath& Path,
		FAsyncLoadCallback OnComplete = {}, int32 Priority = 0)
		-> std::shared_ptr<FAsyncLoadHandle>;
	ENGINE_API auto RequestAsyncLoad(const FObjectPath& Path,
		FAsyncLoadCallback OnComplete = {}, const DClass* ExpectedClass = nullptr,
		int32 Priority = 0) -> std::shared_ptr<FAsyncLoadHandle>;
	ENGINE_API auto RequestAsyncLoad(const FTopLevelAssetPath& Path,
		FAsyncLoadCallback OnComplete = {}, const DClass* ExpectedClass = nullptr,
		int32 Priority = 0) -> std::shared_ptr<FAsyncLoadHandle>;

	// EngineLoop pumps this automatically. Tools/tests must pump on GameThread.
	// Budget is checked between completions; object graph application is currently atomic.
	ENGINE_API auto ProcessAsyncLoading(double TimeBudgetMilliseconds = 2.0,
		uint32 MaximumCompletions = 1) -> void;
	// Cancels all requests/deliveries and drains background reads. Shutdown/tools only;
	// normal UI cancellation should use the individual handle's nonblocking Cancel().
	ENGINE_API auto CancelAsyncLoading() -> void;
}
