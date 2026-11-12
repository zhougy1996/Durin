#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItem.h"
#include "Threading/TaskRuntime.h"

namespace Durin::ContentBrowser
{
	enum class EEnumerationDiagnosticKind : uint8
	{
		Entry,
		Traversal
	};
	struct FEnumerationDiagnostic
	{
		EEnumerationDiagnosticKind Kind = EEnumerationDiagnosticKind::Entry;
		std::string PhysicalPath;
		std::string Message;
	};
	// Deterministic native I/O injection; absent in ordinary queries.
	using FEntryStatusQuery = std::function<std::filesystem::file_status(const std::filesystem::directory_entry&, std::error_code&)>;
	struct FContentBrowserDataCapture
	{
		std::vector<FContentBrowserItem> Items;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};
	// Prepared on the owner thread. Implementations own immutable values and never
	// dereference a reflected source while executing on a worker.
	class IContentBrowserDataCompiledFilter
	{
	  public:
		virtual ~IContentBrowserDataCompiledFilter() = default;
		virtual auto Capture(const Tasks::FCancellationToken&, const FEntryStatusQuery&) const -> FContentBrowserDataCapture = 0;
	};
} // namespace Durin::ContentBrowser
