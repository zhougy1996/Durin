#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "StaticMeshEditorAPI.h"
#include "Widgets/StaticMeshPreview.h"

namespace Durin
{
	class DStaticMesh;

	// Hosts read-only, per-resource StaticMesh inspection documents.
	class MStaticMeshInspector final : public Editor::IWorkspace
	{
	public:
		explicit MStaticMeshInspector(Editor::FWorkspaceManager& InWorkspaceManager);
		STATICMESHEDITOR_API ~MStaticMeshInspector() override;
		STATICMESHEDITOR_API auto GetWorkspaceType() const -> const Editor::FWorkspaceTypeId& override;
		STATICMESHEDITOR_API auto OpenDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentOpenResult override;
		STATICMESHEDITOR_API auto ActivateDocument(const Editor::FDocumentTab& Document) -> void override;
		STATICMESHEDITOR_API auto RequestCloseDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentCloseResult override;
		STATICMESHEDITOR_API auto SaveDocument(const Editor::FDocumentTab&) -> bool override { return false; }
		STATICMESHEDITOR_API auto DiscardDocument(const Editor::FDocumentTab&) -> bool override { return false; }
		STATICMESHEDITOR_API auto IsDocumentDirty(const Editor::FDocumentTab&) const -> bool override { return false; }
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
		auto DrawDocument(const Editor::FDocumentTab& Document, FDocumentState& State) -> void;
		auto DrawDetails(const Editor::FDocumentTab& Document, FDocumentState& State, float Height) -> void;
		auto DrawPreview(FDocumentState& State, float Height) -> void;

		Editor::FWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		Editor::FWorkspaceDocumentHost DocumentHost;
		std::string ActiveResourceId;
		std::string ErrorMessage;
		float PreviewPaneRatio = 0.70f;
		uint64 NextPreviewId = 1;
	};
}
