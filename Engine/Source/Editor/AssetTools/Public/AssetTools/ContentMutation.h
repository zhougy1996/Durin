#pragma once
#include "CoreMinimal.h"
#include "AssetToolsAPI.h"
#include "AssetRegistry/ContentChanges.h"
#include "Misc/UnifiedError.h"
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

	ASSETTOOLS_API auto CanModifyContent(const FContentMutationPolicy&, std::string* Error = nullptr) -> bool;
	ASSETTOOLS_API auto MakeContentMutationFeedback(const FContentMutationPolicy&) -> FFeedbackContext;
	ASSETTOOLS_API auto NotifyContentMutation(const FContentMutationPolicy&, FContentChangeBatch) -> void;
	ASSETTOOLS_API auto ReportContentMutationFailure(const FContentMutationPolicy&, const UnifiedError::FError&) -> bool;
}
