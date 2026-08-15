#include "Assets/AssetRelocationTransaction.h"

namespace Durin::Editor::Level
{
	FAssetRelocationTransaction::FAssetRelocationTransaction(
		Asset::FAssetMutationTransaction InTransaction)
		: Transaction(std::move(InTransaction))
	{
	}

	auto FAssetRelocationTransaction::GetDescription() const
		-> std::string_view
	{
		return Transaction.GetSummary().GetScope().size() == 2
			? "Move Asset" : "Move Assets";
	}

	auto FAssetRelocationTransaction::GetDetails(
		::Durin::Editor::ETransactionOperation) const -> std::string
	{
		return LastResult.Message;
	}

	auto FAssetRelocationTransaction::Undo() -> bool
	{
		LastResult = Transaction.Undo();
		return static_cast<bool>(LastResult);
	}

	auto FAssetRelocationTransaction::Redo() -> bool
	{
		LastResult = Transaction.Redo();
		return static_cast<bool>(LastResult);
	}
}
