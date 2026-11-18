#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "Texture/Texture.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/AssetEditorSession.h"
#include "PropertyEditor/PropertyView.h"
#include "Source/SourceReferenceIndex.h"
#include "TextureEditorAPI.h"
#include "Preview/TexturePreview.h"
#include "VolumeTexturePreview.h"

namespace Durin { class DTexture2D; class DVolumeTexture; struct FTexturePlatformData; struct FVolumeTexturePlatformData; }

namespace Durin::TextureEditor
{
	// Controls 2D and volume texture documents with family-specific preview state.
	enum class ETextureEditorMode : uint8 { Texture2D, Volume };

	class FTextureEditorToolkit final : public FBaseAssetToolkit
	{
	public:
		auto GetLastOperationError() const -> std::string override { return ErrorMessage; }
		explicit FTextureEditorToolkit(ETextureEditorMode InMode = ETextureEditorMode::Texture2D);
		TEXTUREEDITOR_API ~FTextureEditorToolkit() override;
		TEXTUREEDITOR_API auto OnClose() -> void override;
		TEXTUREEDITOR_API auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		TEXTUREEDITOR_API auto OnOpenAsset(DObject& Object) -> bool override;
		TEXTUREEDITOR_API auto OnActivateAsset(DObject& Object) -> void override;
		TEXTUREEDITOR_API auto RequestDeactivate() -> bool override;
		TEXTUREEDITOR_API auto OnCloseAsset(DObject& Object) -> void override;
		TEXTUREEDITOR_API auto OnSaveAsset(DObject& Object) -> bool override;
		TEXTUREEDITOR_API auto OnDiscardAsset(DObject& Object) -> bool override;
		TEXTUREEDITOR_API auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		TEXTUREEDITOR_API auto OnResourceIdRemapped(std::string_view Source, std::string_view Destination) -> void override;
		TEXTUREEDITOR_API auto OnIsAssetDirty(const DObject& Object) const -> bool override;
		TEXTUREEDITOR_API auto OnCanSaveActiveDocument() const -> bool override;
		TEXTUREEDITOR_API auto OnSaveActiveDocument() -> bool override;
		TEXTUREEDITOR_API auto OnCanUndo() const -> bool override;
		TEXTUREEDITOR_API auto OnCanRedo() const -> bool override;
		TEXTUREEDITOR_API auto GetUndoDescription() const -> std::string override;
		TEXTUREEDITOR_API auto GetRedoDescription() const -> std::string override;
		TEXTUREEDITOR_API auto OnUndo() -> bool override;
		TEXTUREEDITOR_API auto OnRedo() -> bool override;
		TEXTUREEDITOR_API auto OnDrawToolkit(bool bActive) -> bool override;
		TEXTUREEDITOR_API auto ResetLayout() -> void override;

	protected:
		auto PostInitAssetEditor() -> void override;
	private:
		friend struct FTextureEditorToolkitTestAccess;
		auto GetActiveTexture() const -> DTexture*;
		auto SaveTexture(DTexture* Texture) -> bool;
		auto CancelUnobservedCompilation(DTexture* Texture) -> void;
		auto DrawDocument(DTexture2D* Texture) -> void;
		auto DrawToolbar(DTexture2D* Texture) -> void;
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


		auto DrawDocument(DVolumeTexture* Texture) -> void;
		auto DrawPreview(const std::string& ResourceId, DVolumeTexture* Texture) -> void;
		auto DrawDetails(DVolumeTexture* Texture) -> void;
		const ETextureEditorMode Mode;
		struct FVolumePreviewState
		{
			std::unique_ptr<FTexturePreview> Preview = std::make_unique<FTexturePreview>();
			EVolumeTexturePreviewAxis Axis = EVolumeTexturePreviewAxis::XY;
			ETexturePreviewChannel Channel = ETexturePreviewChannel::RGBA;
			uint32 Mip = 0;
			uint32 Slice = 0;
			std::weak_ptr<const FVolumeTexturePlatformData> PlatformInput;
			FXxHash128 SourceIdentity{};
			uint64 SelectionKey = std::numeric_limits<uint64>::max();
			float Zoom = 0.0f;
		};
		std::unordered_map<std::string, FVolumePreviewState> VolumePreviewStates;

		FAssetEditorSession Session;
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
