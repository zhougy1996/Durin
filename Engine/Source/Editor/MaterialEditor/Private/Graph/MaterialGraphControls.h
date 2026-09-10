#pragma once

#include "MaterialGraphOperations.h"
#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	inline auto DrawNumericDragEditor(const char* Label,
		EMaterialProgramValueType Type, float* Value) -> bool
	{
		constexpr float DragSpeed = 0.01f;
		switch (Type)
		{
		case EMaterialProgramValueType::Float:
			return ImGui::DragFloat(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
		case EMaterialProgramValueType::Float2:
			return ImGui::DragFloat2(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
		case EMaterialProgramValueType::Float3:
			return ImGui::DragFloat3(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
		case EMaterialProgramValueType::Float4:
			return ImGui::DragFloat4(Label, Value, DragSpeed, 0.0f, 0.0f, "%.3f");
		case EMaterialProgramValueType::Texture2D:
		case EMaterialProgramValueType::Surface:
			return false;
		}
		return false;
	}

	inline auto DrawNumericInputEditor(const char* Label,
		EMaterialProgramValueType Type, float* Value) -> bool
	{
		constexpr ImGuiInputTextFlags Flags = ImGuiInputTextFlags_EnterReturnsTrue;
		switch (Type)
		{
		case EMaterialProgramValueType::Float:
			return ImGui::InputFloat(Label, Value, 0.0f, 0.0f, "%.3f", Flags);
		case EMaterialProgramValueType::Float2:
			return ImGui::InputFloat2(Label, Value, "%.3f", Flags);
		case EMaterialProgramValueType::Float3:
			return ImGui::InputFloat3(Label, Value, "%.3f", Flags);
		case EMaterialProgramValueType::Float4:
			return ImGui::InputFloat4(Label, Value, "%.3f", Flags);
		case EMaterialProgramValueType::Texture2D:
		case EMaterialProgramValueType::Surface:
			return false;
		}
		return false;
	}

	inline auto ReportCommand(
		const FMaterialGraphCommandResult& Result,
		const std::function<void(std::string)>& ReportError) -> void
	{
		if (Result || !ReportError) return;
		std::string Message = Result.Message;
		if (!Result.Diagnostics.empty())
		{
			if (!Message.empty()) Message += " ";
			Message += Result.Diagnostics.front().Message;
		}
		ReportError(Message.empty()
			? "The material graph command failed." : std::move(Message));
	}
}
