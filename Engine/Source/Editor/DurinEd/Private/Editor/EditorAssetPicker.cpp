#include "Editor/EditorAssetPicker.h"

#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Misc/StringHelper.h"
#include "MonaImGui.h"

namespace Durin::EditorAssetPicker
{
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
		if (!Config.RequiredClass || !Config.ComboId || !Config.SearchId || Config.SearchText.empty() || !Config.AssignSelection)
		{
			PickerResult.Error = "The asset picker configuration is incomplete.";
			return PickerResult;
		}

		const std::string Preview = GetAssetPathOrNone(Config.CurrentSelection, Config.NoneLabel ? Config.NoneLabel : "None");
		if (!ImGui::BeginCombo(Config.ComboId, Preview.c_str())) return PickerResult;
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

		for (const auto& [Path, Data] : Asset::GetAssetRegistry().GetAssets())
		{
			DClass* AssetClass = FindClassByQualifiedName(Data.AssetClassName);
			const std::string PathString = Path.ToString();
			if (!MatchesClass(AssetClass, Config.RequiredClass, Config.ClassPolicy) ||
				!StringUtils::ContainsInsensitive(PathString, Config.SearchText.data()))
				continue;

			const bool bSelected = Config.CurrentSelection && Config.CurrentSelection->GetPackage() &&
				Config.CurrentSelection->GetPackage()->GetPackagePath() == PathString;
			if (!ImGui::Selectable(PathString.c_str(), bSelected)) continue;
			DObject* LoadedAsset = nullptr;
			const Asset::FAssetResult LoadResult = Asset::LoadAsset(Path, LoadedAsset);
			if (!LoadResult || !LoadedAsset)
			{
				PickerResult.Error = LoadResult ? "The selected asset could not be loaded." : LoadResult.Message;
				continue;
			}
			Assign(LoadedAsset);
		}
		ImGui::EndCombo();
		return PickerResult;
	}
}
