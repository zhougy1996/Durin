#include "Import/MeshCoordinateImportModel.h"

#include "MonaImGui.h"

namespace Durin::Editor
{
	auto FMeshCoordinateImportModel::Reset() -> void
	{
		SetPreset(EPreset::Durin);
	}

	auto FMeshCoordinateImportModel::SetPreset(EPreset InPreset) -> void
	{
		Preset = InPreset;
		if (Preset == EPreset::Durin)
			Settings = FStaticMeshImportSettings::MakeDurin();
		else if (Preset == EPreset::YUpNegativeZForward)
			Settings = FStaticMeshImportSettings::MakeYUpNegativeZForward();
	}

	auto FMeshCoordinateImportModel::Draw() -> void
	{
		static constexpr const char* PresetNames[] = {
			"Durin (+X Forward, +Y Right, +Z Up)",
			"Y-Up / -Z Forward (+X Right)",
			"Custom"};
		static constexpr const char* AxisNames[] = {
			"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
		int PresetIndex = static_cast<int>(Preset);
		if (ImGui::Combo("Preset", &PresetIndex, PresetNames, std::size(PresetNames)))
		{
			SetPreset(static_cast<EPreset>(PresetIndex));
		}
		if (Preset == EPreset::Custom)
		{
			auto DrawAxis = [&](const char* Label, EStaticMeshImportAxis& Axis) {
				int Value = static_cast<int>(Axis);
				if (!ImGui::Combo(Label, &Value, AxisNames, std::size(AxisNames))) return;
				Axis = static_cast<EStaticMeshImportAxis>(Value);
			};
			DrawAxis("Forward", Settings.ForwardAxis);
			DrawAxis("Right", Settings.RightAxis);
			DrawAxis("Up", Settings.UpAxis);
		}
		else
		{
			ImGui::TextDisabled(
				"Source axes are baked into Durin's +X Forward / +Y Right / +Z Up basis.");
		}
	}
}
