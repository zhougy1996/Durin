#pragma once

#include "CoreMinimal.h"
#include "Asset/StreamableHandle.h"

namespace Durin
{
	struct FStreamableManagerState;

	// One consumer request over the shared package loader. Unsupported UE options
	// (stalled start, JIT, download and update delegates) are deliberately absent.
	struct FStreamableAsyncLoadParams
	{
		std::vector<FSoftObjectPath> TargetsToStream;
		FStreamableDelegate OnComplete;
		FStreamableDelegate OnCancel;
		int32 Priority = 0;
		bool bManageActiveHandle = false;
		std::string DebugName;
		// Durin extension: validate each selected object before exposing it.
		const DClass* ExpectedClass = nullptr;
	};

	// GameThread request/retention owner; all instances share one loading backend.
	// Async completion is deferred, including resident hits and ordinary errors.
	// Rejected admission returns a failed handle without callbacks. Release/cancel
	// never forces collection. Destruction detaches callbacks and cancels consumers,
	// never another manager's package work.
	class FStreamableManager
	{
	public:
		ENGINE_API FStreamableManager();
		ENGINE_API ~FStreamableManager();
		FStreamableManager(const FStreamableManager&) = delete;
		auto operator=(const FStreamableManager&) -> FStreamableManager& = delete;
		// Terminal canceled error, with no request registration or loading.
		ENGINE_API static auto CreateErrorHandle(UnifiedError::FError Error, FStreamableAsyncLoadParams Params = {}) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestAsyncLoad(FStreamableAsyncLoadParams Params) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestAsyncLoad(const FSoftObjectPath& Path,
			FStreamableDelegate OnComplete = {}, const DClass* ExpectedClass = nullptr,
			int32 Priority = 0) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestAsyncLoad(const FTopLevelAssetPath& Path,
			FStreamableDelegate OnComplete = {}, const DClass* ExpectedClass = nullptr,
			int32 Priority = 0) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestSyncLoad(std::span<const FSoftObjectPath> Targets,
			bool bManageActiveHandle = false) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestSyncLoad(const FSoftObjectPath& Path,
			const DClass* ExpectedClass = nullptr) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestSyncLoad(const FTopLevelAssetPath& Path,
			const DClass* ExpectedClass = nullptr) -> std::shared_ptr<FStreamableHandle>;
		// Package tools select a package, never an inferred asset export.
		ENGINE_API auto RequestAsyncLoadPackage(const FPackagePath& Path,
			FStreamableDelegate OnComplete = {}, int32 Priority = 0) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto RequestSyncLoadPackage(const FPackagePath& Path) -> std::shared_ptr<FStreamableHandle>;
		// Parents retain child observations and their own successful objects. Parent
		// cancellation/release never calls cancel/release on a shared child.
		ENGINE_API auto CreateCombinedHandle(std::span<const std::shared_ptr<FStreamableHandle>> Children,
			FStreamableDelegate OnComplete = {}, std::string DebugName = {},
			bool bManageActiveHandle = false, FStreamableDelegate OnCancel = {}) -> std::shared_ptr<FStreamableHandle>;
		ENGINE_API auto GetActiveHandles() const -> std::vector<std::shared_ptr<FStreamableHandle>>;
	private:
		ENGINE_API auto ClearRequests() -> void;
		friend class DAssetManager;
		auto Track(std::shared_ptr<FStreamableHandle> Handle, bool bManaged = false) -> std::shared_ptr<FStreamableHandle>;
		std::shared_ptr<FStreamableManagerState> State;
	};

	// Default owner for ordinary engine consumers; local managers can scope features.
	ENGINE_API auto GetStreamableManager() -> FStreamableManager&;
}
