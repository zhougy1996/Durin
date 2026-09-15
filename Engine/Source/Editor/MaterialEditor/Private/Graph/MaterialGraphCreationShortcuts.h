#pragma once

#include "MaterialGraphOperations.h"
#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	struct FMaterialGraphCreationShortcut
	{
		ImGuiKey Key;
		EMaterialProgramOpcode Opcode;
		EMaterialProgramValueType Type;
		const char* Hint;
	};

	inline constexpr FMaterialGraphCreationShortcut MaterialGraphCreationShortcuts[] = {
		{ImGuiKey_1, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, "1 + LMB"},
		{ImGuiKey_2, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2, "2 + LMB"},
		{ImGuiKey_3, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3, "3 + LMB"},
		{ImGuiKey_4, EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float4, "4 + LMB"},
		{ImGuiKey_A, EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float, "A + LMB"},
		{ImGuiKey_M, EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float, "M + LMB"},
		{ImGuiKey_L, EMaterialProgramOpcode::Lerp, EMaterialProgramValueType::Float, "L + LMB"},
		{ImGuiKey_U, EMaterialProgramOpcode::TextureCoordinates, EMaterialProgramValueType::Float2, "U + LMB"},
		{ImGuiKey_S, EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, "S + LMB"},
		{ImGuiKey_V, EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, "V + LMB"},
		{ImGuiKey_T, EMaterialProgramOpcode::TextureSampleParameter2D, EMaterialProgramValueType::Float4, "T + LMB"},
	};

	inline auto GetCreationShortcutHint(const FMaterialGraphCatalogEntry& Entry) -> const char*
	{
		if (Entry.Opcode == EMaterialProgramOpcode::Constant) return "1/2/3/4 + LMB";
		for (const auto& Shortcut : MaterialGraphCreationShortcuts)
			if (Entry.Opcode == Shortcut.Opcode && (IsMaterialAdaptiveNumeric(Entry.Opcode) || Entry.ResultType == Shortcut.Type))
				return Shortcut.Hint;
		return "";
	}
}
