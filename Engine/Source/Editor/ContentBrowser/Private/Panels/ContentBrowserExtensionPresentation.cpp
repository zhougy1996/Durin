#include "Panels/ContentBrowserExtensionPresentation.h"

namespace Durin::ContentBrowser::Private
{
	auto BuildExtensionContext(std::span<const FContentBrowserItem> Items,
		const std::unordered_set<std::string>& Selection,
		std::string_view CurrentPhysicalDirectory, std::string_view CurrentVirtualDirectory,
		const FContentBrowserItem* PrimaryItem,
		std::string_view TargetPhysicalDirectory, std::string_view TargetVirtualDirectory)
		-> FExtensionContext
	{
		FExtensionContext Context;
		Context.CurrentPhysicalDirectory = CurrentPhysicalDirectory;
		Context.CurrentVirtualDirectory = CurrentVirtualDirectory;
		Context.PhysicalDirectory = CurrentPhysicalDirectory;
		Context.VirtualDirectory = CurrentVirtualDirectory;
		for (const auto& Item : Items)
			if (Selection.contains(Item.StableId())) Context.Selection.push_back(Item);
		if (PrimaryItem)
		{
			Context.PrimaryItem = *PrimaryItem;
			if (!Selection.contains(PrimaryItem->StableId())) Context.Selection = {*Context.PrimaryItem};
			if (PrimaryItem->Kind == EContentBrowserItemKind::Folder)
			{
				Context.PhysicalDirectory = PrimaryItem->PhysicalPath;
				Context.VirtualDirectory = PrimaryItem->VirtualPath;
			}
		}
		else if (!Context.Selection.empty()) Context.PrimaryItem = Context.Selection.front();
		if (!TargetPhysicalDirectory.empty())
		{
			Context.PhysicalDirectory = TargetPhysicalDirectory;
			Context.VirtualDirectory = TargetVirtualDirectory;
			Context.PrimaryItem = FContentBrowserItem{
				.Kind = EContentBrowserItemKind::Folder,
				.VirtualPath = std::string(TargetVirtualDirectory),
				.PhysicalPath = std::string(TargetPhysicalDirectory)};
			Context.Selection = {*Context.PrimaryItem};
		}
		return Context;
	}

	auto PresentExtensionMenu(EExtensionCategory Category, const FExtensionContext& Context,
		bool bAllowContentMutation,
		const std::function<bool(const FExtensionDescriptor&, bool)>& MenuItem,
		const std::function<void(const FExtensionDescriptor&, const FExtensionContext&)>& Queue) -> void
	{
		if (Category == EExtensionCategory::Details) return;
		for (const auto& Extension : CaptureExtensions(Category))
		{
			if (!Extension.IsApplicable(Context)) continue;
			const bool bEnabled = CanInvokeExtension(Extension, Context, bAllowContentMutation);
			if (MenuItem(Extension, bEnabled) && bEnabled) Queue(Extension, Context);
		}
	}
}
