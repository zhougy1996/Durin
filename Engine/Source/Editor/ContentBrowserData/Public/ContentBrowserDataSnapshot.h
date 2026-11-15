#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataCapture.h"
#include "ContentBrowserItem.h"
namespace Durin::ContentBrowser::Private
{
	using ContentBrowser::EEnumerationDiagnosticKind;
	using ContentBrowser::FEnumerationDiagnostic;
	using ContentBrowser::FEntryStatusQuery;
	// Value-owned capture; publication never mutates a snapshot held by a reader.
	template<typename TItem> struct TContentBrowserItemsSnapshot
	{
		std::shared_ptr<Tasks::FCancellationToken> Cancellation;
		std::shared_ptr<const FContentBrowserDataCompiledFilter> Filter;
		bool bStale = false;
		std::vector<TItem> Items;
		struct FContribution
		{
			TWeakObjectPtr<DContentBrowserDataSource> Source;
			size_t Begin = 0, End = 0;
		};
		std::vector<FContribution> Contributions;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};

	using FContentBrowserItemsCapture = TContentBrowserItemsSnapshot<FContentBrowserItemData>;
	using FContentBrowserItemsSnapshot = TContentBrowserItemsSnapshot<FContentBrowserItem>;

	// Children remain valid as long as the caller retains the published owner.
	struct FContentBrowserDirectorySnapshot
	{
		bool bStale = false;
		std::vector<std::filesystem::path> Children;
		std::shared_ptr<FContentBrowserItemsCapture> Capture;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};

}
