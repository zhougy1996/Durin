#pragma once

#include "AssetMutation.h"
#include "Editor/Transaction.h"

namespace Durin::Editor::Level
{
	// Retains one opaque AssetCore mutation in shared editor Undo/Redo history.
	class FAssetRelocationTransaction final : public ::Durin::Editor::ITransaction
	{
	public:
		explicit FAssetRelocationTransaction(
			Asset::FAssetMutationTransaction InTransaction);

		auto GetDescription() const -> std::string_view override;
		auto GetDetails(::Durin::Editor::ETransactionOperation Operation) const
			-> std::string override;
		auto MutatesMountedContent() const -> bool override { return true; }
		auto Undo() -> bool override;
		auto Redo() -> bool override;

	private:
		Asset::FAssetMutationTransaction Transaction;
		Asset::FAssetResult LastResult;
	};
}
