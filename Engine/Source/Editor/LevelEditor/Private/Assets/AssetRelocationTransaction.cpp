#include "Assets/AssetRelocationTransaction.h"

namespace Durin::Editor::Level
{
	FAssetRelocationTransaction::FAssetRelocationTransaction(
		Asset::FAssetRelocationBatchToken InToken)
		: Token(std::move(InToken))
	{
	}

	auto FAssetRelocationTransaction::GetDescription() const
		-> std::string_view
	{
		return Token.GetMappings().size() == 1
			? "Move Asset" : "Move Assets";
	}

	auto FAssetRelocationTransaction::GetDetails(
		::Durin::Editor::ETransactionOperation) const -> std::string
	{
		return LastResult.Message;
	}

	auto FAssetRelocationTransaction::Undo() -> bool
	{
		LastResult = Asset::RestoreAssetRelocationBatch(Token);
		return static_cast<bool>(LastResult);
	}

	auto FAssetRelocationTransaction::Redo() -> bool
	{
		LastResult = Asset::RevalidateAssetRelocationBatch(Token);
		if (LastResult)
			LastResult = Asset::ApplyAssetRelocationBatch(Token);
		return static_cast<bool>(LastResult);
	}
}
