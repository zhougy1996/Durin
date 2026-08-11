#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "StaticMeshEditorAPI.h"
#include "Widgets/StaticMeshPreview.h"

namespace Durin { class DStaticMesh; }

namespace Durin::Editor::StaticMesh
{
	// Hosts read-only, per-resource StaticMesh inspection documents.
	class MStaticMeshInspector final : public ::Durin::Editor::IWorkspace
	{
	public:
		explicit MStaticMeshInspector(::Durin::Editor::FWorkspaceManager& InWorkspaceManager);
		STATICMESHEDITOR_API ~MStaticMeshInspector() override;
		STATICMESHEDITOR_API auto GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId& override;
		STATICMESHEDITOR_API auto OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult override;
		STATICMESHEDITOR_API auto ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void override;
		STATICMESHEDITOR_API auto RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult override;
		STATICMESHEDITOR_API auto SaveDocument(const ::Durin::Editor::FDocumentTab&) -> bool override { return false; }
		STATICMESHEDITOR_API auto DiscardDocument(const ::Durin::Editor::FDocumentTab&) -> bool override { return false; }
		STATICMESHEDITOR_API auto IsDocumentDirty(const ::Durin::Editor::FDocumentTab&) const -> bool override { return false; }
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
		auto DrawDocument(const ::Durin::Editor::FDocumentTab& Document, FDocumentState& State) -> void;
		auto DrawDetails(const ::Durin::Editor::FDocumentTab& Document, FDocumentState& State, float Height) -> void;
		auto DrawPreview(FDocumentState& State, float Height) -> void;

		::Durin::Editor::FWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		::Durin::Editor::FWorkspaceDocumentHost DocumentHost;
		std::string ActiveResourceId;
		std::string ErrorMessage;
		float PreviewPaneRatio = 0.70f;
		uint64 NextPreviewId = 1;
	};
}
