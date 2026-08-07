#include "Editor/EditorAssetPicker.h"

#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin::EditorAssetPicker
{
	namespace
	{
		constexpr size_t MaxCachedSearches = 128;

		// Identifies a reusable asset candidate set by class and path policy.
		struct FCandidateCacheKey
		{
			const DClass* RequiredClass = nullptr;
			EEditorAssetClassPolicy ClassPolicy = EEditorAssetClassPolicy::Derived;
			std::string PathPrefix;

			auto operator==(const FCandidateCacheKey&) const -> bool = default;
		};

		// Hashes every field that participates in candidate-cache identity.
		struct FCandidateCacheKeyHash
		{
			auto operator()(const FCandidateCacheKey& Key) const -> size_t
			{
				size_t Hash = std::hash<const DClass*>{}(Key.RequiredClass);
				Hash ^= std::hash<uint8>{}(static_cast<uint8>(Key.ClassPolicy)) + 0x9e3779b9 + (Hash << 6) + (Hash >> 2);
				Hash ^= std::hash<std::string>{}(Key.PathPrefix) + 0x9e3779b9 + (Hash << 6) + (Hash >> 2);
				return Hash;
			}
		};

		// Retains one normalized search result set for a candidate generation.
		struct FSearchCacheEntry
		{
			FCandidateCacheKey CandidateKey;
			std::string SearchText;
			uint32 MaxSearchResults = 0;
			std::vector<const FAssetPath*> MatchingPaths;
			bool bTruncated = false;
			int LastUsedFrame = -1;
		};

		// Owns shared candidate and search caches across asset-picker instances.
		struct FAssetPickerCache
		{
			uint64 RegistryRevision = 0;
			std::unordered_map<FCandidateCacheKey, std::vector<FAssetPath>, FCandidateCacheKeyHash> Candidates;
			std::unordered_map<ImGuiID, FSearchCacheEntry> Searches;
		};

		auto GetPickerCache() -> FAssetPickerCache&
		{
			static FAssetPickerCache Cache;
			const uint64 RegistryRevision = Asset::GetAssetRegistry().GetRevision();
			if (Cache.RegistryRevision != RegistryRevision)
			{
				Cache.RegistryRevision = RegistryRevision;
				Cache.Candidates.clear();
				Cache.Searches.clear();
			}
			return Cache;
		}

		auto GetCandidatePaths(FAssetPickerCache& Cache, const FCandidateCacheKey& Key) -> const std::vector<FAssetPath>&
		{
			auto [Iterator, bInserted] = Cache.Candidates.try_emplace(Key);
			if (!bInserted) return Iterator->second;

			std::vector<FAssetPath>& Paths = Iterator->second;
			Paths.reserve(Asset::GetAssetRegistry().GetAssets().size());
			for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
			{
				if (Data.EntryKind == Asset::EAssetRegistryEntryKind::Redirector)
					continue;
				if (!MatchesPathPrefix(Path.GetView(), Key.PathPrefix)) continue;
				if (!MatchesClass(FindClassByQualifiedName(Data.AssetClassName), Key.RequiredClass, Key.ClassPolicy)) continue;
				Paths.push_back(Path);
			}
			std::ranges::sort(Paths, {}, &FAssetPath::GetView);
			return Paths;
		}

		auto GetMatchingPaths(
			FAssetPickerCache& Cache,
			ImGuiID PickerId,
			const FCandidateCacheKey& CandidateKey,
			std::string_view SearchText,
			uint32 MaxSearchResults
		) -> const FSearchCacheEntry&
		{
			auto SearchIterator = Cache.Searches.find(PickerId);
			if (SearchIterator == Cache.Searches.end())
			{
				if (Cache.Searches.size() >= MaxCachedSearches)
				{
					auto Oldest = Cache.Searches.begin();
					for (auto Iterator = std::next(Oldest); Iterator != Cache.Searches.end(); ++Iterator)
						if (Iterator->second.LastUsedFrame < Oldest->second.LastUsedFrame) Oldest = Iterator;
					Cache.Searches.erase(Oldest);
				}
				SearchIterator = Cache.Searches.try_emplace(PickerId).first;
			}
			FSearchCacheEntry& Search = SearchIterator->second;
			Search.LastUsedFrame = ImGui::GetFrameCount();
			if (Search.CandidateKey == CandidateKey && Search.SearchText == SearchText &&
				Search.MaxSearchResults == MaxSearchResults)
				return Search;

			Search.CandidateKey = CandidateKey;
			Search.SearchText = SearchText;
			Search.MaxSearchResults = MaxSearchResults;
			Search.MatchingPaths.clear();
			const std::vector<FAssetPath>& CandidatePaths = GetCandidatePaths(Cache, CandidateKey);
			Search.MatchingPaths.reserve(std::min<size_t>(CandidatePaths.size(), MaxSearchResults));
			Search.bTruncated = false;
			for (const FAssetPath& Path : CandidatePaths)
			{
				if (!StringUtils::ContainsInsensitive(Path.GetView(), SearchText)) continue;
				if (Search.MatchingPaths.size() == MaxSearchResults)
				{
					Search.bTruncated = true;
					break;
				}
				Search.MatchingPaths.push_back(&Path);
			}
			return Search;
		}

		// Reserves the action group using half-spacing at both edges and full spacing
		// between actions, keeping the group centered without an arbitrary trailing pad.
		auto GetTrailingActionsWidth(size_t ActionCount) -> float
		{
			if (ActionCount == 0) return 0.0f;
			const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;
			const float EdgePadding = ItemSpacing * 0.5f;
			const float InterActionSpacing = ItemSpacing;
			return EdgePadding * 2.0f
				+ static_cast<float>(ActionCount) * MonaImGui::GetCompactToolbarIconButtonWidth()
				+ static_cast<float>(ActionCount - 1) * InterActionSpacing;
		}
	}

	auto MatchesPathPrefix(std::string_view AssetPath, std::string_view PathPrefix) -> bool
	{
		return PathPrefix.empty() || AssetPath.starts_with(PathPrefix);
	}

	auto MatchesClass(const DClass* Candidate, const DClass* Required, EEditorAssetClassPolicy Policy) -> bool
	{
		if (!Candidate || !Required) return false;
		if (Policy == EEditorAssetClassPolicy::Exact) return Candidate == Required;
		// DClass::IsChildOf is intentionally not exported across runtime modules, so follow
		// the public superclass chain here rather than coupling the picker to its implementation.
		for (const DClass* Current = Candidate; Current != nullptr; Current = Current->GetSuperClass())
		{
			if (Current == Required) return true;
		}
		return false;
	}

	auto GetAssetPathOrNone(const DObject* Object, std::string_view NoneLabel) -> std::string
	{
		return Object && Object->GetPackage() ? Object->GetPackage()->GetPackagePath() : std::string(NoneLabel);
	}

	auto GetAssetPathOrNone(const DObject* Object, std::string_view ObjectPath, std::string_view NoneLabel) -> std::string
	{
		return ObjectPath.empty() ? GetAssetPathOrNone(Object, NoneLabel) : std::string(ObjectPath);
	}

	auto Draw(const FEditorAssetPickerConfig& Config) -> FEditorAssetPickerResult
	{
		FEditorAssetPickerResult PickerResult;
		const bool bPathAssignment = Config.AssignmentMode == EEditorAssetAssignmentMode::AssetPath;
		const bool bInvalidAction = Config.TrailingAction &&
			(!Config.TrailingAction->Icon || !Config.TrailingAction->ButtonId || !Config.TrailingAction->Execute);
		const bool bInvalidAdditionalAction = std::ranges::any_of(
			Config.AdditionalTrailingActions,
			[](const FEditorAssetPickerAction& Action) {
				return !Action.Icon || !Action.ButtonId || !Action.Execute;
			});
		if (!Config.RequiredClass || !Config.ComboId || !Config.SearchId || Config.SearchText.empty() ||
			(bPathAssignment ? !Config.AssignPathSelection : !Config.AssignSelection) || Config.MaxSearchResults == 0 ||
			Config.MaxSearchResults > static_cast<uint32>(std::numeric_limits<int>::max()) || bInvalidAction || bInvalidAdditionalAction)
		{
			PickerResult.Error = "The asset picker configuration is incomplete.";
			return PickerResult;
		}

		const std::string_view NoneLabel = Config.NoneLabel ? Config.NoneLabel : "None";
		const std::string CurrentPath = GetAssetPathOrNone(Config.CurrentSelection, Config.CurrentSelectionPath, {});
		std::string Preview = CurrentPath.empty() ? std::string(NoneLabel) : CurrentPath;
		if (!Config.CurrentSelectionStatus.empty()) Preview += std::format(" [{}]", Config.CurrentSelectionStatus);
		const size_t ActionCount = (Config.TrailingAction ? 1u : 0u) + Config.AdditionalTrailingActions.size();
		if (ActionCount > 0)
		{
			ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - GetTrailingActionsWidth(ActionCount)));
		}
		else ImGui::SetNextItemWidth(-FLT_MIN);

		const ImGuiID PickerId = ImGui::GetID(Config.ComboId);
		if (ImGui::BeginCombo(Config.ComboId, Preview.c_str()))
		{
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint(
				Config.SearchId,
				Config.SearchHint ? Config.SearchHint : "Search assets...",
				Config.SearchText.data(),
				Config.SearchText.size()
			);

			const auto AssignObject = [&](DObject* Selection) {
				std::string Error;
				if (!Config.AssignSelection(Selection, Error))
				{
					PickerResult.Error = Error.empty() ? "The selected asset was rejected." : std::move(Error);
					return;
				}
				const std::string SelectedPath = GetAssetPathOrNone(Selection, {});
				PickerResult.bSelectionChanged = SelectedPath != CurrentPath;
			};
			const auto AssignPath = [&](std::string_view SelectionPath) {
				std::string Error;
				if (!Config.AssignPathSelection(SelectionPath, Error))
				{
					PickerResult.Error = Error.empty() ? "The selected asset path was rejected." : std::move(Error);
					return;
				}
				PickerResult.bSelectionChanged = SelectionPath != CurrentPath;
			};
			if (Config.bAllowNone && ImGui::Selectable(NoneLabel.data(), CurrentPath.empty()))
			{
				if (bPathAssignment) AssignPath({});
				else AssignObject(nullptr);
			}

			const FCandidateCacheKey CandidateKey{
				.RequiredClass = Config.RequiredClass,
				.ClassPolicy = Config.ClassPolicy,
				.PathPrefix = std::string(Config.PathPrefixFilter)};
			FAssetPickerCache& Cache = GetPickerCache();
			const FSearchCacheEntry& Search = GetMatchingPaths(
				Cache,
				PickerId,
				CandidateKey,
				Config.SearchText.data(),
				Config.MaxSearchResults
			);
			ImGuiListClipper Clipper;
			Clipper.Begin(static_cast<int>(Search.MatchingPaths.size()));
			while (Clipper.Step())
			{
				for (int Index = Clipper.DisplayStart; Index < Clipper.DisplayEnd; ++Index)
				{
					const FAssetPath& Path = *Search.MatchingPaths[Index];
					const bool bSelected = CurrentPath == Path.GetView();
					if (!ImGui::Selectable(Path.ToString().c_str(), bSelected)) continue;
					if (bPathAssignment)
					{
						AssignPath(Path.GetView());
						continue;
					}
					DObject* LoadedAsset = nullptr;
					const Asset::FAssetResult LoadResult = Asset::LoadAsset(Path, LoadedAsset);
					if (!LoadResult || !LoadedAsset)
					{
						PickerResult.Error = LoadResult ? "The selected asset could not be loaded." : LoadResult.Message;
						continue;
					}
					AssignObject(LoadedAsset);
				}
			}
			if (Search.MatchingPaths.empty()) ImGui::TextDisabled("No matching assets.");
			else if (Search.bTruncated)
				ImGui::TextDisabled("Showing the first %u matches. Refine the search to see more.", Config.MaxSearchResults);
			ImGui::EndCombo();
		}

		bool bFirstAction = true;
		const auto DrawAction = [&](const FEditorAssetPickerAction& Action)
		{
			const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;
			ImGui::SameLine(0.0f, bFirstAction ? ItemSpacing * 0.5f : ItemSpacing);
			bFirstAction = false;
			ImGui::BeginDisabled(!Action.bEnabled);
			const bool bTriggered = MonaImGui::CompactToolbarIconButton(Action.Icon, Action.ButtonId);
			ImGui::EndDisabled();
			if (Action.Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", Action.Tooltip);
			if (bTriggered)
			{
				std::string Error;
				if (!Action.Execute(Error))
					PickerResult.Error = Error.empty() ? "The asset picker action failed." : std::move(Error);
				else PickerResult.bTrailingActionTriggered = true;
			}
		};
		if (Config.TrailingAction) DrawAction(*Config.TrailingAction);
		for (const FEditorAssetPickerAction& Action : Config.AdditionalTrailingActions) DrawAction(Action);
		return PickerResult;
	}
}
