#include "Editor/AssetRelocation.h"

#include "Asset/Mutation.h"
#include "Editor/Transaction.h"

namespace Durin::Editor
{
	namespace
	{
		class FAssetRelocationEditorTransaction final : public ITransaction
		{
		public:
			explicit FAssetRelocationEditorTransaction(
				Asset::FAssetMutationTransaction InTransaction)
				: Transaction(std::move(InTransaction))
			{
			}

			auto GetDescription() const -> std::string_view override
			{
				return Transaction.GetSummary().GetScope().size() == 2
					? "Move Asset" : "Move Assets";
			}
			auto GetDetails(ETransactionOperation) const -> std::string override
			{
				return LastResult.Message;
			}
			auto MutatesMountedContent() const -> bool override { return true; }
			auto Undo() -> bool override
			{
				LastResult = Transaction.Undo();
				return static_cast<bool>(LastResult);
			}
			auto Redo() -> bool override
			{
				LastResult = Transaction.Redo();
				return static_cast<bool>(LastResult);
			}

		private:
			Asset::FAssetMutationTransaction Transaction;
			Asset::FAssetResult LastResult;
		};
	}

	auto ExecuteAssetRelocations(
		FTransactionManager& Transactions,
		std::span<const Asset::FAssetRelocationMapping> Mappings)
		-> Asset::FAssetResult
	{
		if (Mappings.empty()) return {};
		Asset::FAssetMutationSummary Summary;
		Asset::FAssetMutationTransaction Transaction;
		Asset::FAssetResult Result = Asset::PrepareAssetRelocationTransaction(
			Mappings, Summary, Transaction);
		if (!Result) return Result;
		Result = Transaction.Commit();
		if (!Result) return Result;
		Transactions.CommitApplied(
			std::make_unique<FAssetRelocationEditorTransaction>(
				std::move(Transaction)));
		return {};
	}
} // namespace Durin::Editor
