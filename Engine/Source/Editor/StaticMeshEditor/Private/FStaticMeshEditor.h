#pragma once

#include "CoreMinimal.h"

#include "Toolkits/AssetEditorToolkit.h"
#include "StaticMeshEditorAPI.h"
#include "Widgets/StaticMeshPreview.h"

namespace Durin { class DStaticMesh; }

namespace Durin::StaticMeshEditor
{
	// Hosts read-only StaticMesh inspection and previews.
	class FStaticMeshEditor final : public FBaseAssetToolkit
	{
	public:
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		FStaticMeshEditor();
		STATICMESHEDITOR_API ~FStaticMeshEditor() override;
		STATICMESHEDITOR_API auto OnClose() -> void override;
		STATICMESHEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		STATICMESHEDITOR_API auto OnOpenAsset(DObject& Object) -> bool override;
		STATICMESHEDITOR_API auto OnCloseAsset(DObject& Object) -> void override;
		STATICMESHEDITOR_API auto OnDrawToolkit(bool bActive) -> bool override;
		STATICMESHEDITOR_API auto ResetLayout() -> void override;
	protected:
		auto CanEditAssets() const -> bool override { return false; }

	private:
		struct FMeshPreviewState
		{
			std::shared_ptr<FStaticMeshPreview> Preview;
			uint32 SelectedLOD = 0;
			uint64 PreviewId = 0;
		};

		auto FindState(std::string_view ResourceId) -> FMeshPreviewState*;
		auto FindState(std::string_view ResourceId) const -> const FMeshPreviewState*;
		auto DrawMesh(DStaticMesh& Mesh, FMeshPreviewState& State) -> void;
		auto DrawDetails(DStaticMesh& Asset, std::string_view Resource, FMeshPreviewState& State, float Height) -> void;
		auto DrawPreview(DStaticMesh& Mesh, FMeshPreviewState& State, float Height) -> void;

		std::unordered_map<std::string, FMeshPreviewState> MeshPreviews;
		std::string ErrorMessage;
		float PreviewPaneRatio = 0.70f;
	};
}
