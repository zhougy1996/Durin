#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DClass;
	class DObject;

	enum class EEditorAssetClassPolicy : uint8
	{
		Exact,
		Derived,
	};

	struct FEditorAssetPickerAction
	{
		const char* Icon = nullptr;
		const char* ButtonId = nullptr;
		const char* Tooltip = nullptr;
		bool bEnabled = true;
		std::function<bool(std::string&)> Execute;
	};

	struct FEditorAssetPickerConfig
	{
		const char* ComboId = "##Asset";
		const char* SearchId = "##AssetSearch";
		const char* SearchHint = "Search assets...";
		const DClass* RequiredClass = nullptr;
		EEditorAssetClassPolicy ClassPolicy = EEditorAssetClassPolicy::Derived;
		DObject* CurrentSelection = nullptr;
		std::span<char> SearchText;
		bool bAllowNone = true;
		const char* NoneLabel = "None";
		std::function<bool(DObject*, std::string&)> AssignSelection;
		// When present, the picker reserves stable trailing width and always draws
		// the action, including its disabled state.
		std::optional<FEditorAssetPickerAction> TrailingAction;
	};

	struct FEditorAssetPickerResult
	{
		bool bSelectionChanged = false;
		bool bTrailingActionTriggered = false;
		std::string Error;
	};

	namespace EditorAssetPicker
	{
		DURINED_API auto MatchesClass(
			const DClass* Candidate,
			const DClass* Required,
			EEditorAssetClassPolicy Policy
		) -> bool;
		DURINED_API auto GetAssetPathOrNone(const DObject* Object, std::string_view NoneLabel = "None") -> std::string;
		DURINED_API auto Draw(const FEditorAssetPickerConfig& Config) -> FEditorAssetPickerResult;
	}
}
