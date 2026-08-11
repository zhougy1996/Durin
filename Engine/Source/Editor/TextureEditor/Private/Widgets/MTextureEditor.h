#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Editor/PropertyView.h"
#include "Source/SourceReferenceIndex.h"
#include "TextureEditorAPI.h"
#include "Widgets/TexturePreview.h"

namespace Durin
{
	class DTexture2D;

	// Hosts one texture document with preview, mip, and metadata controls.
	class MTextureEditor final : public Editor::IWorkspace
	{
	public:
		explicit MTextureEditor(Editor::FWorkspaceManager& InWorkspaceManager);
		TEXTUREEDITOR_API ~MTextureEditor() override;
		TEXTUREEDITOR_API auto GetWorkspaceType() const -> const Editor::FWorkspaceTypeId& override;
		TEXTUREEDITOR_API auto OpenDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentOpenResult override;
		TEXTUREEDITOR_API auto ActivateDocument(const Editor::FDocumentTab& Document) -> void override;
		TEXTUREEDITOR_API auto RequestDeactivate() -> bool override;
		TEXTUREEDITOR_API auto RequestCloseDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentCloseResult override;
		TEXTUREEDITOR_API auto SaveDocument(const Editor::FDocumentTab& Document) -> bool override;
		TEXTUREEDITOR_API auto DiscardDocument(const Editor::FDocumentTab& Document) -> bool override;
		TEXTUREEDITOR_API auto IsDocumentDirty(const Editor::FDocumentTab& Document) const -> bool override;
		TEXTUREEDITOR_API auto CanSaveActiveDocument() const -> bool override;
		TEXTUREEDITOR_API auto SaveActiveDocument() -> bool override;
		TEXTUREEDITOR_API auto CanUndo() const -> bool override;
		TEXTUREEDITOR_API auto CanRedo() const -> bool override;
		TEXTUREEDITOR_API auto GetUndoDescription() const -> std::string_view override;
		TEXTUREEDITOR_API auto GetRedoDescription() const -> std::string_view override;
		TEXTUREEDITOR_API auto Undo() -> bool override;
		TEXTUREEDITOR_API auto Redo() -> bool override;
		TEXTUREEDITOR_API auto DrawWorkspace(bool bActive) -> bool override;
		TEXTUREEDITOR_API auto ResetLayout() -> void override;

	private:
		auto FindOpenTexture(std::string_view ResourceId) const -> DTexture2D*;
		auto GetActiveTexture() const -> DTexture2D*;
		auto SaveTexture(DTexture2D* Texture) -> bool;
		auto DrawDocument(const Editor::FDocumentTab& Document, DTexture2D* Texture) -> void;
		auto DrawToolbar(const Editor::FDocumentTab& Document, DTexture2D* Texture) -> void;
		auto DrawWideLayout(const std::string& ResourceId, DTexture2D* Texture) -> void;
		auto DrawNarrowLayout(const std::string& ResourceId, DTexture2D* Texture) -> void;
		auto DrawPreviewPanel(const std::string& ResourceId, DTexture2D* Texture, float Width, float Height) -> void;
		auto DrawDetailsPanel(DTexture2D* Texture, float Height) -> void;
		auto DrawBuildReadiness(DTexture2D* Texture) -> void;
		auto DrawFailureState(DTexture2D* Texture) -> void;
		auto DrawSourceData(DTexture2D* Texture) -> void;
		auto DrawBuildSettings(DTexture2D* Texture) -> void;
		auto ReimportSource(DTexture2D* Texture) -> void;
		auto ChangeSourceReference(DTexture2D* Texture) -> void;
		auto IngestExternalSource(DTexture2D* Texture) -> void;
		auto RepairSource(DTexture2D* Texture) -> void;
		auto RequestSharedSourceReplacement(DTexture2D* Texture) -> void;
		auto DrawSharedSourceReplacementConfirmation(DTexture2D* Texture) -> void;
		auto RequestSharedSourceRelocation(DTexture2D* Texture) -> void;
		auto DrawSharedSourceRelocationConfirmation(DTexture2D* Texture) -> void;
		auto ChangeSourceLocation(DTexture2D* Texture) -> void;
		auto FinishActivePropertyEdit(bool bCancel) -> bool;
		auto MakePropertyViewContext() -> Editor::FPropertyViewContext;
		auto SetError(std::string Message) -> void;

		Editor::FWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, TObjectPtr<DTexture2D>> OpenTextures;
		Editor::FWorkspaceDocumentHost DocumentHost;
		std::string ActiveResourceId;
		std::string ErrorMessage;
		Editor::FPropertyView PropertyView;
		FSourceReferenceIndex SourceReferenceIndex;
		struct FPendingSourceReplacement
		{
			std::string SourceVirtualPath;
			std::string ReplacementPhysicalPath;
			std::vector<FSourceReference> AffectedAssets;
			bool bOpenRequested = false;
		};
		FPendingSourceReplacement PendingSourceReplacement;
		struct FPendingSourceRelocation
		{
			std::string OriginalSourceVirtualPath;
			std::string DestinationSourceVirtualPath;
			std::vector<FSourceReference> AffectedAssets;
			bool bOpenRequested = false;
		};
		FPendingSourceRelocation PendingSourceRelocation;
		// Retains the preview selection independently for each open texture.
		struct FTexturePreviewState
		{
			std::unique_ptr<FTexturePreview> Preview = std::make_unique<FTexturePreview>();
			uint32 SelectedMipIndex = 0;
			uint32 LastUploadedMipIndex = UINT32_MAX;
			uint64 LastObservedRevision = 0;
			float Zoom = 0.0f;
			bool bShowCheckerboard = true;
			bool bPreviewSource = false;
			bool bLastUploadWasSource = false;
			ETexturePreviewChannel SelectedChannel = ETexturePreviewChannel::RGBA;
			ETexturePreviewChannel LastAppliedChannel = ETexturePreviewChannel::RGBA;
		};
		std::unordered_map<std::string, FTexturePreviewState> PreviewStates;
		float PreviewPaneRatio = 0.70f;
	};
}
