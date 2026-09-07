#pragma once

#include "Panels/ContentBrowserChanges.h"
#include "Panels/ContentBrowserQuery.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Selection identities survive captures; only a completed projection repairs visibility.
	struct FContentBrowserSelection
	{
		std::unordered_set<std::string> Selected;
		std::string Anchor;
		auto Repair(const FContentBrowserItemRange& Items) -> void
		{
			std::erase_if(Selected, [&](const auto& Id) {
				return std::ranges::none_of(Items, [&](const auto& Item) { return Item.StableId() == Id; });
			});
			if (!Selected.contains(Anchor)) Anchor.clear();
		}
		auto Select(const FContentBrowserItemRange& Items, size_t Index, bool Extend, bool Toggle) -> void
		{
			if (Index >= Items.size()) return;
			const auto& Id = Items[Index].StableId();
			if (Extend && !Anchor.empty())
			{
				const auto It = std::ranges::find_if(Items, [&](const auto& Item) { return Item.StableId() == Anchor; });
				if (It != Items.end())
				{
					const size_t Start = static_cast<size_t>(std::distance(Items.begin(), It));
					if (!Toggle) Selected.clear();
					for (size_t I = std::min(Start, Index); I <= std::max(Start, Index); ++I)
						Selected.insert(Items[I].StableId());
					return;
				}
			}
			if (Toggle) { if (!Selected.erase(Id)) Selected.insert(Id); }
			else { Selected.clear(); Selected.insert(Id); }
			Anchor = Id;
		}
		static auto Migrate(std::string& Id, const FContentChange& Change) -> void
		{
			if (Id.empty()) return;
			if (Change.Kind == EContentChangeKind::Renamed)
			{
				if (!Change.NewAssetPath.empty() && ContentBrowserChanges::MatchesAsset(Id, Change.OldAssetPath))
					Id = Change.NewAssetPath + Id.substr(Change.OldAssetPath.size());
				else if (Change.bDirectory && !Change.OldAssetPath.empty() && !Change.NewAssetPath.empty()
					&& ContentBrowserChanges::Within(Id, Change.OldAssetPath))
				{
					auto VirtualChange = Change;
					VirtualChange.OldPhysicalPath = Change.OldAssetPath;
					VirtualChange.NewPhysicalPath = Change.NewAssetPath;
					ContentBrowserChanges::RemapPhysical(Id, VirtualChange);
				}
				else ContentBrowserChanges::RemapPhysical(Id, Change);
			}
			else if (Change.Kind == EContentChangeKind::Removed
				&& (ContentBrowserChanges::MatchesAsset(Id, Change.OldAssetPath)
					|| ContentBrowserChanges::SamePath(Id, Change.OldPhysicalPath)
					|| (Change.bDirectory && (ContentBrowserChanges::Within(Id, Change.OldPhysicalPath)
						|| ContentBrowserChanges::Within(Id, Change.OldAssetPath))))) Id.clear();
		}
		auto Apply(const FContentChangeBatch& Batch) -> void
		{
			for (const auto& Change : Batch.Changes)
			{
				std::unordered_set<std::string> Next;
				for (auto Id : Selected) { Migrate(Id, Change); if (!Id.empty()) Next.insert(std::move(Id)); }
				Selected = std::move(Next);
				Migrate(Anchor, Change);
			}
		}
	};
}
