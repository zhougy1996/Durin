#pragma once

#include "StaticMeshEditorAPI.h"

namespace Durin { class DStaticMesh; }

namespace Durin::Editor::StaticMesh
{
	// Owns one isolated world, mesh component, light, viewport, and camera.
	class STATICMESHEDITOR_API FStaticMeshPreview final
	{
	public:
		explicit FStaticMeshPreview(uint64 PreviewId);
		~FStaticMeshPreview();

		auto SetVisible(bool bVisible) -> void;
		auto Draw(DStaticMesh* Mesh, uint64 Revision, float PanelHeight = 0.0f) -> void;
		auto ResetView() -> void;
		auto SetWireframe(bool bWireframe) -> void;
		auto IsWireframe() const -> bool;

	private:
		class FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
