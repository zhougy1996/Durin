#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataCapture.h"
namespace Durin::ContentBrowser::Private
{
	class FContentBrowserFileDataDiscovery
	{
	  public:
		auto CaptureItems(const std::string&, const std::string&, bool, const Tasks::FCancellationToken&) -> FContentBrowserDataCapture;
		auto SetEntryStatusQueryForTesting(FEntryStatusQuery Query) -> void { EntryStatusQuery = std::move(Query); }

	  private:
		auto QueryEntryStatus(const std::filesystem::directory_entry&, std::error_code&) const -> std::filesystem::file_status;
		auto AddEnumerationDiagnostic(EEnumerationDiagnosticKind, const std::filesystem::path&, std::string) -> void;
		FContentBrowserDataCapture Result;
		FEntryStatusQuery EntryStatusQuery;
	};
} // namespace Durin::ContentBrowser::Private
