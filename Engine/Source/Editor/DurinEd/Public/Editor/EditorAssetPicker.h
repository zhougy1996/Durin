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
	};

	struct FEditorAssetPickerResult
	{
		bool bSelectionChanged = false;
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
