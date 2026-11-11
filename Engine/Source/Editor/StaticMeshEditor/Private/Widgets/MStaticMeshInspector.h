#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "StaticMeshEditorAPI.h"
#include "Widgets/StaticMeshPreview.h"

namespace Durin { class DStaticMesh; }

namespace Durin::StaticMeshEditor
{
	// Hosts read-only, per-resource StaticMesh inspection documents.
	class MStaticMeshInspector final : public FBaseAssetToolkit
	{
	public:
		auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		explicit MStaticMeshInspector(FEditorDocumentManager& InWorkspaceManager);
		STATICMESHEDITOR_API ~MStaticMeshInspector() override;
		STATICMESHEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		STATICMESHEDITOR_API auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		STATICMESHEDITOR_API auto ActivateDocument(const FEditorDocument& Document) -> void override;
		STATICMESHEDITOR_API auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		STATICMESHEDITOR_API auto OnSaveDocument(const FEditorDocument& Document) -> bool override { return DocumentModel.SaveDocument(Document); }
		STATICMESHEDITOR_API auto OnDiscardDocument(const FEditorDocument& Document) -> bool override { return DocumentModel.DiscardDocument(Document); }
		STATICMESHEDITOR_API auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool override { return DocumentModel.IsDocumentDirty(Document); }
		STATICMESHEDITOR_API auto OnCanSaveActiveDocument() const -> bool override { return DocumentModel.CanSaveActiveDocument(); }
		STATICMESHEDITOR_API auto OnSaveActiveDocument() -> bool override { return DocumentModel.SaveActiveDocument(); }
		STATICMESHEDITOR_API auto OnDrawToolkit(bool bActive) -> bool override;
		STATICMESHEDITOR_API auto ResetLayout() -> void override;
	protected:
		auto CanEditAssets() const -> bool override { return false; }

	private:
		struct FDocumentState
		{
			TObjectPtr<DStaticMesh> Mesh;
			std::unique_ptr<FStaticMeshPreview> Preview;
			uint32 SelectedLOD = 0;
			uint64 PreviewId = 0;
		};

		auto FindState(std::string_view ResourceId) -> FDocumentState*;
		auto FindState(std::string_view ResourceId) const -> const FDocumentState*;
		auto DrawDocument(const FEditorDocument& Document, FDocumentState& State) -> void;
		auto DrawDetails(const FEditorDocument& Document, FDocumentState& State, float Height) -> void;
		auto DrawPreview(FDocumentState& State, float Height) -> void;

		FEditorDocumentManager& DocumentManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		FReadOnlyAssetDocumentModel DocumentModel;
		std::string ErrorMessage;
		float PreviewPaneRatio = 0.70f;
	};
}
