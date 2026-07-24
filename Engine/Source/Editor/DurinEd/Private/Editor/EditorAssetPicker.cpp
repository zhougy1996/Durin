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

		struct FCandidateCacheKey
		{
			const DClass* RequiredClass = nullptr;
			EEditorAssetClassPolicy ClassPolicy = EEditorAssetClassPolicy::Derived;
			std::string PathPrefix;

			auto operator==(const FCandidateCacheKey&) const -> bool = default;
		};

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

		struct FSearchCacheEntry
		{
			FCandidateCacheKey CandidateKey;
			std::string SearchText;
			uint32 MaxVisibleResults = 0;
			std::vector<const FAssetPath*> VisiblePaths;
			bool bTruncated = false;
			int LastUsedFrame = -1;
		};

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
				if (!Key.PathPrefix.empty() && !Path.GetView().starts_with(Key.PathPrefix)) continue;
				if (!MatchesClass(FindClassByQualifiedName(Data.AssetClassName), Key.RequiredClass, Key.ClassPolicy)) continue;
				Paths.push_back(Path);
			}
			std::ranges::sort(Paths, {}, &FAssetPath::GetView);
			return Paths;
		}

		auto GetVisiblePaths(
			FAssetPickerCache& Cache,
			ImGuiID PickerId,
			const FCandidateCacheKey& CandidateKey,
			std::string_view SearchText,
			uint32 MaxVisibleResults
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
				Search.MaxVisibleResults == MaxVisibleResults)
				return Search;

			Search.CandidateKey = CandidateKey;
			Search.SearchText = SearchText;
			Search.MaxVisibleResults = MaxVisibleResults;
			Search.VisiblePaths.clear();
			Search.VisiblePaths.reserve(MaxVisibleResults);
			Search.bTruncated = false;
			for (const FAssetPath& Path : GetCandidatePaths(Cache, CandidateKey))
			{
				if (!StringUtils::ContainsInsensitive(Path.GetView(), SearchText)) continue;
				if (Search.VisiblePaths.size() == MaxVisibleResults)
				{
					Search.bTruncated = true;
					break;
				}
				Search.VisiblePaths.push_back(&Path);
			}
			return Search;
		}
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

	auto Draw(const FEditorAssetPickerConfig& Config) -> FEditorAssetPickerResult
	{
		FEditorAssetPickerResult PickerResult;
		const bool bInvalidAction = Config.TrailingAction &&
			(!Config.TrailingAction->Icon || !Config.TrailingAction->ButtonId || !Config.TrailingAction->Execute);
		if (!Config.RequiredClass || !Config.ComboId || !Config.SearchId || Config.SearchText.empty() ||
			!Config.AssignSelection || Config.MaxVisibleResults == 0 || bInvalidAction)
		{
			PickerResult.Error = "The asset picker configuration is incomplete.";
			return PickerResult;
		}

		const std::string Preview = GetAssetPathOrNone(Config.CurrentSelection, Config.NoneLabel ? Config.NoneLabel : "None");
		if (Config.TrailingAction)
		{
			const float ReservedWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
			ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - ReservedWidth));
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

			const auto Assign = [&](DObject* Selection) {
				std::string Error;
				if (!Config.AssignSelection(Selection, Error))
				{
					PickerResult.Error = Error.empty() ? "The selected asset was rejected." : std::move(Error);
					return;
				}
				PickerResult.bSelectionChanged = Selection != Config.CurrentSelection;
			};
			if (Config.bAllowNone && ImGui::Selectable(Config.NoneLabel ? Config.NoneLabel : "None", Config.CurrentSelection == nullptr))
				Assign(nullptr);

			const FCandidateCacheKey CandidateKey{
				.RequiredClass = Config.RequiredClass,
				.ClassPolicy = Config.ClassPolicy,
				.PathPrefix = std::string(Config.PathPrefixFilter)};
			FAssetPickerCache& Cache = GetPickerCache();
			const FSearchCacheEntry& Search = GetVisiblePaths(
				Cache,
				PickerId,
				CandidateKey,
				Config.SearchText.data(),
				Config.MaxVisibleResults
			);
			for (const FAssetPath* Path : Search.VisiblePaths)
			{
				const bool bSelected = Config.CurrentSelection && Config.CurrentSelection->GetPackage() &&
					Config.CurrentSelection->GetPackage()->GetPackagePath() == Path->GetView();
				if (!ImGui::Selectable(Path->ToString().c_str(), bSelected)) continue;
				DObject* LoadedAsset = nullptr;
				const Asset::FAssetResult LoadResult = Asset::LoadAsset(*Path, LoadedAsset);
				if (!LoadResult || !LoadedAsset)
				{
					PickerResult.Error = LoadResult ? "The selected asset could not be loaded." : LoadResult.Message;
					continue;
				}
				Assign(LoadedAsset);
			}
			if (Search.VisiblePaths.empty()) ImGui::TextDisabled("No matching assets.");
			else if (Search.bTruncated)
				ImGui::TextDisabled("Showing the first %u matches. Refine the search to see more.", Config.MaxVisibleResults);
			ImGui::EndCombo();
		}

		if (Config.TrailingAction)
		{
			const FEditorAssetPickerAction& Action = *Config.TrailingAction;
			ImGui::SameLine();
			ImGui::BeginDisabled(!Action.bEnabled);
			const bool bTriggered = MonaImGui::ToolbarIconButton(Action.Icon, Action.ButtonId);
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
		}
		return PickerResult;
	}
}
