#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "SkeletalMeshEditorAPI.h"
#include "Widgets/SkeletalAssetPreview.h"

namespace Durin { class DObject; }

namespace Durin::Editor::SkeletalMesh
{
	// Hosts bounded read-only documents for the three skeletal asset classes.
	class MSkeletalAssetInspector final : public ::Durin::Editor::IWorkspace
	{
	public:
		explicit MSkeletalAssetInspector(::Durin::Editor::FWorkspaceManager& InWorkspaceManager);
		SKELETALMESHEDITOR_API ~MSkeletalAssetInspector() override;
		SKELETALMESHEDITOR_API auto GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId& override;
		SKELETALMESHEDITOR_API auto OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult override;
		SKELETALMESHEDITOR_API auto ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void override;
		SKELETALMESHEDITOR_API auto RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult override;
		auto SaveDocument(const ::Durin::Editor::FDocumentTab&) -> bool override { return false; }
		auto DiscardDocument(const ::Durin::Editor::FDocumentTab&) -> bool override { return false; }
		auto IsDocumentDirty(const ::Durin::Editor::FDocumentTab&) const -> bool override { return false; }
		auto CanSaveActiveDocument() const -> bool override { return false; }
		auto SaveActiveDocument() -> bool override { return false; }
		SKELETALMESHEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		SKELETALMESHEDITOR_API auto ResetLayout() -> void override;

	private:
		struct FDocumentState
		{
			TObjectPtr<DObject> Asset;
			TObjectPtr<DObject> PreviewPeer;
			std::vector<std::string> PreviewPeerPaths;
			int32 SelectedPreviewPeer = 0;
			std::unique_ptr<FSkeletalAssetPreview> Preview;
		};

		auto FindState(std::string_view DocumentKey) -> FDocumentState*;
		auto DrawDocument(const ::Durin::Editor::FDocumentTab& Document, FDocumentState& State) -> void;

		::Durin::Editor::FWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, FDocumentState> Documents;
		::Durin::Editor::FWorkspaceDocumentHost DocumentHost;
		std::string ErrorMessage;
		uint64 NextPreviewId = 1;
	};
}
