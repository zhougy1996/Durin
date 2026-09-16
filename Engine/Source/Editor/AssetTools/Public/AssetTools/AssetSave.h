#pragma once

#include "AssetTools/AssetOperation.h"

namespace Durin
{
	enum class EAssetSaveMode : uint8
	{
		LoadedDirtyPackage,
		CanonicalResave,
	};

	struct FAssetSaveRequest
	{
		std::vector<FPackagePath> AssetPaths;
		EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage;
		FPublishAssetOperation Publish;
	};

	// Asynchronous disk staging for one loaded dirty package. Calls, completion
	// publication and destruction belong to the editor thread.
	class ASSETTOOLS_API FAssetSaveOperation
	{
	public:
		static auto Begin(const FAssetSaveRequest& Request, FAssetOperationResult& OutResult)
			-> std::unique_ptr<FAssetSaveOperation>;
		~FAssetSaveOperation();
		auto IsReady() const -> bool;
		auto Complete() -> FAssetOperationResult;
	private:
		FAssetSaveOperation();
		struct FState;
		std::unique_ptr<FState> State;
	};
}
