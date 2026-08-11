#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DClass;
	class DObject;
}

namespace Durin::Editor
{
	// Selects exact-class or derived-class filtering for asset candidates.
	enum class EAssetClassPolicy : uint8
	{
		Exact,
		Derived,
	};

	// Selects whether a picker commits a loaded object or only its canonical asset path.
	enum class EAssetAssignmentMode : uint8
	{
		LoadedObject,
		AssetPath,
	};

	// Defines an optional trailing action rendered beside the asset picker.
	struct FAssetPickerAction
	{
		const char* Icon = nullptr;
		const char* ButtonId = nullptr;
		const char* Tooltip = nullptr;
		bool bEnabled = true;
		std::function<bool(std::string&)> Execute;
	};

	// Configures asset filtering, assignment, and retained search storage.
	struct FAssetPickerConfig
	{
		const char* ComboId = "##Asset";
		const char* SearchId = "##AssetSearch";
		const char* SearchHint = "Search assets...";
		const DClass* RequiredClass = nullptr;
		EAssetClassPolicy ClassPolicy = EAssetClassPolicy::Derived;
		EAssetAssignmentMode AssignmentMode = EAssetAssignmentMode::LoadedObject;
		DObject* CurrentSelection = nullptr;
		// Supplies the current asset identity when the owner stores a soft path
		// instead of keeping the asset loaded.
		std::string_view CurrentSelectionPath;
		// Optional state suffix such as "Unloaded" or "Missing" for path-backed previews.
		std::string_view CurrentSelectionStatus;
		std::span<char> SearchText;
		bool bAllowNone = true;
		const char* NoneLabel = "None";
		std::function<bool(DObject*, std::string&)> AssignSelection;
		std::function<bool(std::string_view, std::string&)> AssignPathSelection;
		// When present, the picker reserves stable trailing width and always draws
		// the action, including its disabled state.
		std::optional<FAssetPickerAction> TrailingAction;
		std::span<const FAssetPickerAction> AdditionalTrailingActions;
		// When non-empty, only paths that start with this prefix are shown.
		std::string_view PathPrefixFilter;
		// Bounds the cached matches for one search. ImGui virtualizes row submission,
		// so every cached match remains scrollable without per-frame traversal.
		uint32 MaxSearchResults = 10000;
	};

	// Reports selection, trailing-action, and assignment-error outcomes.
	struct FAssetPickerResult
	{
		bool bSelectionChanged = false;
		bool bTrailingActionTriggered = false;
		std::string Error;
	};

	namespace AssetPicker
	{
		// Returns whether an asset path passes an optional literal path-prefix filter.
		DURINED_API auto MatchesPathPrefix(std::string_view AssetPath, std::string_view PathPrefix) -> bool;
		DURINED_API auto MatchesClass(
			const DClass* Candidate,
			const DClass* Required,
			EAssetClassPolicy Policy
		) -> bool;
		DURINED_API auto GetAssetPathOrNone(const DObject* Object, std::string_view NoneLabel = "None") -> std::string;
		DURINED_API auto GetAssetPathOrNone(
			const DObject* Object,
			std::string_view ObjectPath,
			std::string_view NoneLabel
		) -> std::string;
		DURINED_API auto Draw(const FAssetPickerConfig& Config) -> FAssetPickerResult;
	}
}
