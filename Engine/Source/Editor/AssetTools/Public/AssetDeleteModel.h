#pragma once
#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "PendingDelete.h"

namespace Durin
{
	class FAssetDeletionOperation;
	namespace ContentStorage { class FFileDeletionRequest; }
	class ASSETTOOLS_API FAssetDeleteModel
	{
	public:
		enum class EState : uint8 { Waiting, StartScanning, Scanning, UpdateActions, Finished };
		explicit FAssetDeleteModel(std::span<DObject* const> ObjectsToDelete);
		~FAssetDeleteModel();
		FAssetDeleteModel(const FAssetDeleteModel&) = delete;
		auto operator=(const FAssetDeleteModel&) -> FAssetDeleteModel& = delete;
		auto AddObjectToDelete(DObject* Object) -> void;
		auto GetPendingDeletedAssets() const -> std::span<const std::unique_ptr<FPendingDelete>>;
		auto Tick(float DeltaTime) -> void;
		auto CanDelete() const -> bool;
		auto DoDelete() -> bool;
		auto GetDeletedObjectCount() const -> int32;
		auto GetProgress() const -> float;
		auto GetProgressText() const -> std::string;
		auto GetState() const -> EState;
		auto OnStateChanged() -> TMulticastDelegate<void(EState)>& { return StateChanged; }
	private:
		friend class ContentStorage::FFileDeletionRequest;
		auto TakeWholePackageOperation() -> FAssetDeletionOperation;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		TMulticastDelegate<void(EState)> StateChanged;
		auto SetState(EState State) -> void;
	};
}
