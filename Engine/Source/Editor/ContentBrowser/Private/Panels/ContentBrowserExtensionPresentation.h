#pragma once

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Panels/ContentBrowserModel.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Builds owned UI context from visible selection. Explicit directory targets replace
	// the selection with that directory; background callers supply an empty selection.
	auto BuildExtensionContext(std::span<const FContentBrowserItem> Items,
		const std::unordered_set<std::string>& Selection,
		std::string_view CurrentPhysicalDirectory, std::string_view CurrentVirtualDirectory,
		const FContentBrowserItem* PrimaryItem = nullptr,
		std::string_view TargetPhysicalDirectory = {},
		std::string_view TargetVirtualDirectory = {}) -> FExtensionContext;

	// Shared production menu presenter; the sink draws each stable-ID item and returns clicks.
	// Dispatch runs through the host queue and must check the current mutation policy again.
	auto PresentExtensionMenu(EExtensionCategory Category, const FExtensionContext& Context,
		bool bAllowContentMutation,
		const std::function<bool(const FExtensionDescriptor&, bool)>& MenuItem,
		const std::function<void(const FExtensionDescriptor&, const FExtensionContext&)>& Queue) -> void;
}
