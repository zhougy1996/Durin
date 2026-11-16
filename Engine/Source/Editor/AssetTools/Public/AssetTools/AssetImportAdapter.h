#pragma once
#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
namespace Durin
{
	class DObject;
	class DAssetImportTask;
	class IAssetImportTaskOperation;
	// Lifetime-scoped provider binding; retirement cancels and drains its accepted
	// tasks before releasing provider callbacks/code. No public request ledger.
	class FAssetImportAdapterRegistration
	{
	public:
		FAssetImportAdapterRegistration() = default;
		ASSETTOOLS_API explicit FAssetImportAdapterRegistration(
			std::function<bool(const DAssetImportTask&, bool)> Accepts,
			std::function<std::shared_ptr<IAssetImportTaskOperation>(const DAssetImportTask&)> Create);
		ASSETTOOLS_API ~FAssetImportAdapterRegistration();
		FAssetImportAdapterRegistration(const FAssetImportAdapterRegistration&) = delete;
		auto operator=(const FAssetImportAdapterRegistration&) -> FAssetImportAdapterRegistration& = delete;
		ASSETTOOLS_API auto Reset() -> void;
	private:
		uint64 Id = 0;
	};
	ASSETTOOLS_API auto SetAssetImportBrowserSync(std::function<void(std::span<DObject* const>)> Callback) -> void;
}
