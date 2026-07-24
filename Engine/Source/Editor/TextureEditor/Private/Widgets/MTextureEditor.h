#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/EditorWorkspace.h"
#include "Editor/EditorWorkspaceRootWindow.h"
#include "Editor/ReflectedPropertyView.h"
#include "TextureEditorAPI.h"
#include "Widgets/TexturePreview.h"

namespace Durin
{
	class DTexture2D;

	// Hosts one texture document with preview, mip, and metadata controls.
	class MTextureEditor final : public IEditorWorkspace
	{
	public:
		explicit MTextureEditor(FEditorWorkspaceManager& InWorkspaceManager);
		TEXTUREEDITOR_API ~MTextureEditor() override;
		TEXTUREEDITOR_API auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& override;
		TEXTUREEDITOR_API auto OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult override;
		TEXTUREEDITOR_API auto ActivateDocument(const FEditorDocumentTab& Document) -> void override;
		TEXTUREEDITOR_API auto RequestDeactivate() -> bool override;
		TEXTUREEDITOR_API auto RequestCloseDocument(const FEditorDocumentTab& Document) -> bool override;
		TEXTUREEDITOR_API auto IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool override;
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
		auto DrawDocument(const FEditorDocumentTab& Document, DTexture2D* Texture) -> void;
		auto DrawFailureState(DTexture2D* Texture) -> void;
		auto DrawPreview(const std::string& ResourceId, DTexture2D* Texture) -> void;
		auto DrawSourceData(DTexture2D* Texture) -> void;
		auto DrawBuildSettings(DTexture2D* Texture) -> void;
		auto FinishActivePropertyEdit(bool bCancel) -> bool;
		auto MakePropertyViewContext() -> FReflectedPropertyViewContext;
		auto SetError(std::string Message) -> void;

		FEditorWorkspaceManager& WorkspaceManager;
		std::unordered_map<std::string, TObjectPtr<DTexture2D>> OpenTextures;
		std::unordered_map<uint64, FEditorWorkspaceRootWindow> DocumentWindows;
		std::string ActiveResourceId;
		std::string ErrorMessage;
		FReflectedPropertyView PropertyView;
		// Set when a dirty document close is deferred for user confirmation.
		FEditorDocumentId PendingCloseDocumentId;

		// Retains the preview selection independently for each open texture.
		struct FTexturePreviewState
		{
			std::unique_ptr<FTexturePreview> Preview = std::make_unique<FTexturePreview>();
			uint32 SelectedMipIndex = 0;
			uint32 LastUploadedMipIndex = UINT32_MAX;
			uint64 LastObservedRevision = 0;
		};
		std::unordered_map<std::string, FTexturePreviewState> PreviewStates;
	};
}
