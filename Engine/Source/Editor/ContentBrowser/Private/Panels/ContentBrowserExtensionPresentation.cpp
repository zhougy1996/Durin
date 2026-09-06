#include "Panels/ContentBrowserExtensionPresentation.h"

namespace Durin::Editor::ContentBrowser::Private
{
	namespace
	{
		auto DescribeItem(const FContentBrowserItem& Item) -> FExtensionItem
		{
			EExtensionItemKind Kind = EExtensionItemKind::File;
			switch (Item.Kind)
			{
			case EContentBrowserItemKind::Folder: Kind = EExtensionItemKind::Directory; break;
			case EContentBrowserItemKind::Asset: Kind = EExtensionItemKind::Asset; break;
			case EContentBrowserItemKind::Redirector: Kind = EExtensionItemKind::Redirector; break;
			default: break;
			}
			return {Kind, Item.StableId(), Item.VirtualPath, Item.PhysicalPath, Item.AssetClassName};
		}
	}

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
			if (Selection.contains(Item.StableId())) Context.Selection.push_back(DescribeItem(Item));
		if (PrimaryItem)
		{
			Context.PrimaryItem = DescribeItem(*PrimaryItem);
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
			Context.PrimaryItem = FExtensionItem{EExtensionItemKind::Directory,
				std::string(TargetPhysicalDirectory), std::string(TargetVirtualDirectory),
				std::string(TargetPhysicalDirectory), {}};
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
