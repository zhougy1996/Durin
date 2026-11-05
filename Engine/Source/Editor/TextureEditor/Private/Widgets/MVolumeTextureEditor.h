#pragma once

#include "CoreMinimal.h"

#include "DObject/ObjectPtr.h"
#include "Texture/Texture.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Documents/AssetEditorDocumentModels.h"
#include "TexturePreview.h"
#include "VolumeTexturePreview.h"

namespace Durin { class DVolumeTexture; struct FVolumeTexturePlatformData; }

namespace Durin::Editor::Texture
{
	class MVolumeTextureEditor final : public IAssetEditorToolkit
	{
	public:
		explicit MVolumeTextureEditor(FEditorDocumentManager& InManager);
		auto GetToolkitId() const -> const FAssetEditorToolkitId& override;
		auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult override;
		auto ActivateDocument(const FEditorDocument& Document) -> void override;
		auto RequestDeactivate() -> bool override { return true; }
		auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult override;
		auto SaveDocument(const FEditorDocument& Document) -> bool override;
		auto DiscardDocument(const FEditorDocument& Document) -> bool override;
		auto OnPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void override;
		auto IsDocumentDirty(const FEditorDocument& Document) const -> bool override;
		auto CanSaveActiveDocument() const -> bool override;
		auto SaveActiveDocument() -> bool override;
		auto CanUndo() const -> bool override { return Documents.CanUndo(); }
		auto CanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string override { return Documents.GetRedoDescription(); }
		auto Undo() -> bool override { return Documents.Undo(); }
		auto Redo() -> bool override { return Documents.Redo(); }
		auto DrawToolkit(bool bActive) -> bool override;
		auto ResetLayout() -> void override;

	private:
		struct FPreviewState
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

		auto Find(std::string_view ResourceId) const -> DVolumeTexture*;
		auto Active() const -> DVolumeTexture*;
		auto Save(DVolumeTexture* Texture) -> bool;
		auto DrawDocument(const FEditorDocument& Document,
			DVolumeTexture* Texture) -> void;
		auto DrawPreview(const std::string& ResourceId, DVolumeTexture* Texture) -> void;
		auto DrawDetails(DVolumeTexture* Texture) -> void;
		auto SetError(std::string Message) -> void { ErrorMessage = std::move(Message); }

		FEditorDocumentManager& Manager;
		std::unordered_map<std::string, TObjectPtr<DVolumeTexture>> OpenTextures;
		std::unordered_map<std::string, FPreviewState> PreviewStates;
		FEditableAssetDocumentModel Documents;
		std::string ErrorMessage;
	};
}
