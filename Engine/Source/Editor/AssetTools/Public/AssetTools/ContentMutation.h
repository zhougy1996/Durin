#pragma once
#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "AssetRegistry/ContentChanges.h"
#include "Asset/AssetWriteResult.h"
#include "Misc/FeedbackContext.h"
#include "Delegates/Delegate.h"

namespace Durin { class DObject; }
namespace Durin::ContentStorage
{
	struct FContentMutationPolicy
	{
		std::function<bool()> CanMutateContent;
		std::function<void(ELogLevel, std::string_view)> ReportDiagnostic;
		std::function<void(FContentChangeBatch)> NotifyContentMutation;
	};

	enum class EContentDirectoryChangeReason : uint8 { Mutation, Discovery };

	// Native directory effects are shared facts. Each subscriber owns its projection.
	ASSETTOOLS_API auto OnContentDirectoriesChanged() -> TMulticastDelegate<void(const FContentChangeBatch&, const DObject*, EContentDirectoryChangeReason)>&;
	ASSETTOOLS_API auto PublishContentDirectoryChanges(const FContentChangeBatch&, const DObject* Origin, EContentDirectoryChangeReason Reason = EContentDirectoryChangeReason::Mutation) -> void;

	class ASSETTOOLS_API FContentMutationOperation
	{
	public:
		explicit FContentMutationOperation(const FContentMutationPolicy&);
		auto CanModify(std::string* Error = nullptr) const -> bool;
	protected:
		auto Report(ELogLevel, std::string_view) const -> void;
		auto Feedback() const -> FFeedbackContext;
		auto NotifyItemsUpdated(FContentChangeBatch) -> void;
		auto Complete(const FAssetWriteResult&, FContentChangeBatch Changes = {}) -> bool;
	private:
		FContentMutationPolicy Policy;
	};
}
