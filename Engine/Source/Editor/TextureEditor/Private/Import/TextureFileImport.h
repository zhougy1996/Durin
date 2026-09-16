#pragma once

#include "AssetTools/AssetSave.h"
#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::Editor::Texture
{
	// UI-independent first-import/save policy. Failed saves retain resident assets
	// and can be retried without reopening or decoding the source.
	class FTextureFileImport
	{
	public:
		using FSaveOperation = std::function<FAssetOperationResult(const FAssetSaveRequest&)>;
		explicit FTextureFileImport(FImportDialogCallbacks InCallbacks = {}, FSaveOperation InSave = {})
			: Callbacks(std::move(InCallbacks)), SaveOperation(std::move(InSave)) {}
		auto ImportFile(std::string_view Filename, std::string_view Directory)
			-> FAssetOperationResult;
		auto RetryPendingSaves() -> void;
		auto HasPendingSaves() const -> bool { return !PendingSaves.empty(); }

	private:
		auto Save(const FPackagePath& Path) -> FAssetOperationResult;
		FImportDialogCallbacks Callbacks;
		FSaveOperation SaveOperation;
		std::vector<FPackagePath> PendingSaves;
	};
}
