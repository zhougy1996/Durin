#pragma once

#include "CoreMinimal.h"

#include "MonaImGui.h"
#include "Asset/MutationExtensions.h"

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
		explicit FMaterialEditorSessionSettings(std::string InFileName = "MaterialEditorPreferences.yaml")
			: FileName(std::move(InFileName)) {}
		auto Load() -> bool;
		auto Save(bool bReplacePreferences = false) const -> bool;
		// Select one editor profile; an absent file starts from defaults.
		auto SelectStorage(std::string InFileName) -> bool;
		static auto RemapStoredViewports(std::span<const FAssetRelocationMapping> Mappings) -> void;

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
		auto Preferences() const -> std::array<bool, 5> { return {bAutoCompile, bPreviewVisible, bDetailsVisible, bParametersVisible, bDiagnosticsVisible}; }
		std::string FileName;
		mutable std::array<bool, 5> SavedPreferences{true, true, true, true, false};
		mutable std::unordered_set<std::string> DirtyViewports;
		std::unordered_map<std::string, FMaterialGraphViewportState> Viewports;
	};
}
