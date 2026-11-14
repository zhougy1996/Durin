#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataAPI.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin { struct FContentChangeBatch; class DContentBrowserDataSource; }
namespace Durin::ContentBrowser
{
	// Operation policy and feedback only. Discovery requires no browser services.
	struct FContentBrowserOperationContext
	{
		std::function<bool(const std::string&, const std::string&)> OpenAsset;
		std::function<void(FContentChangeBatch)> NotifyContentMutation;
		std::function<bool()> CanMutateContent;
		std::function<void(ELogLevel, std::string_view)> ReportDiagnostic;
		auto IsEmpty() const -> bool { return !OpenAsset && !NotifyContentMutation && !CanMutateContent && !ReportDiagnostic; }
	};
	// Feedback updates do not replace authority. Both existing and requested
	// restrictions must allow mutation; this function installs no lifetime fence.
	CONTENTBROWSERDATA_API auto ComposeOperationContext(const FContentBrowserOperationContext& Existing,
		const FContentBrowserOperationContext& Requested) -> FContentBrowserOperationContext;
	// One admission/notification fence, with source-specific committed-effect projection.
	CONTENTBROWSERDATA_API auto ScopeOperationContext(DContentBrowserDataSource&, FContentBrowserOperationContext,
		std::function<void(DContentBrowserDataSource&, const FContentChangeBatch&)> OnCommitted) -> FContentBrowserOperationContext;

}
