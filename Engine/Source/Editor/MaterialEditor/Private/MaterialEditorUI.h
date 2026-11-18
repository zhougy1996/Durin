#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class DMaterial;
	class DMaterialInterface;
	class FAssetEditorToolkit;
	struct FMaterialCompileStatus;
	struct FMaterialProgramDiagnostic;
}

namespace Durin::MaterialEditor
{
	class FMaterialEditorProfileSettings;
	struct FMaterialParameterPanelEntry;
	struct FMaterialParameterGroup
	{
		std::string Label;
		std::string Path;
		std::vector<size_t> EntryIndices;
		std::vector<FMaterialParameterGroup> Children;
	};

	auto AddParameterToGroupTree(FMaterialParameterGroup& Root, const FMaterialParameterPanelEntry& Entry,
		size_t EntryIndex) -> void;
	auto DrawMaterialEditorPreferences(FMaterialEditorProfileSettings& Settings, std::string& OutError) -> bool;
	auto ResetMaterialEditorPreferences(FMaterialEditorProfileSettings& Settings) -> bool;
	auto FormatCompileState(const FMaterialCompileStatus& Status) -> const char*;
	using FMaterialDiagnosticNavigation = std::function<void(const FMaterialProgramDiagnostic&)>;
	auto DrawMaterialCompileStatus(DMaterialInterface* Material, const DMaterial* Source,
		const FMaterialDiagnosticNavigation& NavigateFunction, const FMaterialDiagnosticNavigation& NavigateGraph = {}) -> void;
	auto OpenReferencedMaterialAsset(FAssetEditorToolkit& Toolkit, std::string_view Path, std::string_view ClassName) -> bool;
	auto NavigateToFunctionNode(FAssetEditorToolkit& From, std::string_view Resource, const FGuid& NodeId) -> bool;
}
