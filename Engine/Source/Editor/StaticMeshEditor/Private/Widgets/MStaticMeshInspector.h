#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"
#include "StaticMeshEditorAPI.h"
#include "Widgets/StaticMeshPreview.h"

namespace Durin
{
	class DStaticMesh;

	// Hosts read-only, per-resource StaticMesh inspection documents.
	class MStaticMeshInspector final : public IEditorWorkspace
	{
	public:
		explicit MStaticMeshInspector(FEditorWorkspaceManager& InWorkspaceManager);
		STATICMESHEDITOR_API ~MStaticMeshInspector() override;
		STATICMESHEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		STATICMESHEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult override;
		STATICMESHEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		STATICMESHEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult override;
		STATICMESHEDITOR_API auto SaveDocument(const FEditorDocumentTab&) -> bool override { return false; }
		STATICMESHEDITOR_API auto DiscardDocument(const FEditorDocumentTab&) -> bool override { return false; }
		STATICMESHEDITOR_API auto IsDocumentDirty(const FEditorDocumentTab&) const -> bool override { return false; }
		STATICMESHEDITOR_API auto CanSaveActiveDocument() const -> bool override { return false; }
		STATICMESHEDITOR_API auto SaveActiveDocument() -> bool override { return false; }
		STATICMESHEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		STATICMESHEDITOR_API auto ResetLayout() -> void override;

	private:
		struct FDocumentState
		{
			TObjectPtr<DStaticMesh> Mesh;
			std::unique_ptr<FStaticMeshPreview> Preview;
			uint32 SelectedLOD = 0;
		};

		auto FindState(std::string_view ResourceId) -> FDocumentState*;
		auto FindState(std::string_view ResourceId) const -> const FDocumentState*;
		auto DrawDocument(const FEditorDocumentTab& Document, FDocumentState& State) -> void;
		auto DrawDetails(const FEditorDocumentTab& Document, FDocumentState& State, float Height) -> void;
		auto DrawPreview(FDocumentState& State, float Height) -> void;

		FEditorWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		FEditorWorkspaceDocumentHost DocumentHost;
		std::string ActiveResourceId;
		std::string ErrorMessage;
		float PreviewPaneRatio = 0.70f;
		uint64 NextPreviewId = 1;
	};
}
