#pragma once

#include "AssetSystem.h"
#include "Editor/EditorTransaction.h"

namespace Durin
{
	// Retains one AssetCore relocation token in shared editor Undo/Redo history.
	class FAssetRelocationTransaction final : public IEditorTransaction
	{
	public:
		explicit FAssetRelocationTransaction(
			Asset::FAssetRelocationBatchToken InToken);

		auto GetDescription() const -> std::string_view override;
		auto GetDetails(EEditorTransactionOperation Operation) const
			-> std::string override;
		auto MutatesMountedContent() const -> bool override { return true; }
		auto Undo() -> bool override;
		auto Redo() -> bool override;

	private:
		Asset::FAssetRelocationBatchToken Token;
		Asset::FAssetResult LastResult;
	};
}
