#pragma once

#include "MonaImGui.h"

namespace Durin::Editor::Material
{
	struct FMaterialGraphViewportState
	{
		float Zoom = 1.0f;
		ImVec2 Pan{40.0f, 40.0f};
	};

	// Persists user-scoped material-editor layout and per-asset graph viewports.
	class FMaterialEditorSessionSettings
	{
	public:
		explicit FMaterialEditorSessionSettings(std::string InFileName = "MaterialEditorSession.yaml")
			: FileName(std::move(InFileName)) {}
		auto Load() -> bool;
		auto Save() const -> bool;

		auto FindViewport(std::string_view ResourceId) const
			-> const FMaterialGraphViewportState*;
		auto SetViewport(std::string_view ResourceId,
			const FMaterialGraphViewportState& State) -> void;
		auto MoveViewport(std::string_view OldResourceId,
			std::string_view NewResourceId) -> void;

		bool bAutoCompile = true;
		bool bPreviewVisible = true;
		bool bDetailsVisible = true;
		bool bParametersVisible = true;
		bool bDiagnosticsVisible = false;

	private:
		std::string FileName;
		std::unordered_map<std::string, FMaterialGraphViewportState> Viewports;
	};
}
