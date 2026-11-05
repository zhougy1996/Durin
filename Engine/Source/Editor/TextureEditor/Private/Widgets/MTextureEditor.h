#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "Texture/Texture.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "PropertyEditor/PropertyView.h"
#include "Source/SourceReferenceIndex.h"
#include "TextureEditorAPI.h"
#include "TexturePreview.h"

namespace Durin { class DTexture2D; struct FTexturePlatformData; }

namespace Durin::Editor::Texture
{
	// Hosts one texture document with preview, mip, and metadata controls.
	class MTextureEditor final : public IAssetEditorToolkit
	{
	public:
		explicit MTextureEditor(FEditorDocumentManager& InWorkspaceManager);
		TEXTUREEDITOR_API ~MTextureEditor() override;
		TEXTUREEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		TEXTUREEDITOR_API auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		TEXTUREEDITOR_API auto ActivateDocument(const FEditorDocument& Document) -> void override;
		TEXTUREEDITOR_API auto RequestDeactivate() -> bool override;
		TEXTUREEDITOR_API auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		TEXTUREEDITOR_API auto SaveDocument(const FEditorDocument& Document) -> bool override;
		TEXTUREEDITOR_API auto DiscardDocument(const FEditorDocument& Document) -> bool override;
		TEXTUREEDITOR_API auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		TEXTUREEDITOR_API auto IsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		TEXTUREEDITOR_API auto CanSaveActiveDocument() const -> bool override;
		TEXTUREEDITOR_API auto SaveActiveDocument() -> bool override;
		TEXTUREEDITOR_API auto CanUndo() const -> bool override;
		TEXTUREEDITOR_API auto CanRedo() const -> bool override;
		TEXTUREEDITOR_API auto GetUndoDescription() const -> std::string override;
		TEXTUREEDITOR_API auto GetRedoDescription() const -> std::string override;
		TEXTUREEDITOR_API auto Undo() -> bool override;
		TEXTUREEDITOR_API auto Redo() -> bool override;
		TEXTUREEDITOR_API auto DrawToolkit(bool bActive) -> bool override;
		TEXTUREEDITOR_API auto ResetLayout() -> void override;

	private:
		auto FindOpenTexture(std::string_view ResourceId) const -> DTexture2D*;
		auto GetActiveTexture() const -> DTexture2D*;
		auto SaveTexture(DTexture2D* Texture) -> bool;
		auto DrawDocument(const FEditorDocument& Document, DTexture2D* Texture) -> void;
		auto DrawToolbar(const FEditorDocument& Document, DTexture2D* Texture) -> void;
		auto DrawWideLayout(const std::string& ResourceId, DTexture2D* Texture) -> void;
		auto DrawNarrowLayout(const std::string& ResourceId, DTexture2D* Texture) -> void;
		auto DrawPreviewPanel(const std::string& ResourceId, DTexture2D* Texture, float Width, float Height) -> void;
		auto DrawDetailsPanel(DTexture2D* Texture, float Height) -> void;
		auto DrawFailureState(DTexture2D* Texture) -> void;
		auto DrawPayloadLifecycle(DTexture2D* Texture) -> void;
		auto DrawSourceData(DTexture2D* Texture) -> void;
		auto DrawBuildSettings(DTexture2D* Texture) -> void;
		auto ReimportSource(DTexture2D* Texture) -> void;
		auto ReimportFromFile(DTexture2D* Texture) -> void;
		auto FinishActivePropertyEdit(bool bCancel) -> bool;
		auto MakePropertyViewContext() -> FPropertyViewContext;
		auto SetError(std::string Message) -> void;

		FEditorDocumentManager& DocumentManager;
		std::unordered_map<std::string, TObjectPtr<DTexture2D>> OpenTextures;
		FEditableAssetDocumentModel Documents;
		std::string ErrorMessage;
		FPropertyView PropertyView;
		FSourceReferenceIndex SourceReferenceIndex;
		// Retains the preview selection independently for each open texture.
		struct FTexturePreviewState
		{
			std::unique_ptr<FTexturePreview> Preview = std::make_unique<FTexturePreview>();
			uint32 SelectedMipIndex = 0;
			uint32 LastUploadedMipIndex = UINT32_MAX;
			std::weak_ptr<const FTexturePlatformData> PlatformInput;
			FXxHash128 SourceIdentity{};
			float Zoom = 0.0f;
			bool bShowCheckerboard = true;
			bool bPreviewSource = false;
			ETexturePreviewInterpretation Interpretation = ETexturePreviewInterpretation::Auto;
			bool bLastUploadWasSource = false;
			ETexturePreviewChannel SelectedChannel = ETexturePreviewChannel::RGBA;
			FTexturePreviewOptions LastAppliedOptions;
		};
		std::unordered_map<std::string, FTexturePreviewState> PreviewStates;
		float PreviewPaneRatio = 0.70f;
	};
}
