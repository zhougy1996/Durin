#pragma once

#include "DObject/ObjectPtr.h"
#include "Editor/Workspace.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Widgets/TexturePreview.h"
#include "VolumeTexturePreview.h"

namespace Durin { class DVolumeTexture; }

namespace Durin::Editor::Texture
{
	class MVolumeTextureEditor final : public ::Durin::Editor::IWorkspace
	{
	public:
		explicit MVolumeTextureEditor(::Durin::Editor::FWorkspaceManager& InManager);
		~MVolumeTextureEditor() override = default;
		auto GetWorkspaceType() const -> const ::Durin::Editor::FWorkspaceTypeId& override;
		auto OpenDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentOpenResult override;
		auto ActivateDocument(const ::Durin::Editor::FDocumentTab& Document) -> void override;
		auto RequestDeactivate() -> bool override { return true; }
		auto RequestCloseDocument(const ::Durin::Editor::FDocumentTab& Document) -> ::Durin::Editor::EDocumentCloseResult override;
		auto SaveDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool override;
		auto DiscardDocument(const ::Durin::Editor::FDocumentTab& Document) -> bool override;
		auto IsDocumentDirty(const ::Durin::Editor::FDocumentTab& Document) const -> bool override;
		auto CanSaveActiveDocument() const -> bool override;
		auto SaveActiveDocument() -> bool override;
		auto CanUndo() const -> bool override { return Documents.CanUndo(); }
		auto CanRedo() const -> bool override { return Documents.CanRedo(); }
		auto GetUndoDescription() const -> std::string_view override { return Documents.GetUndoDescription(); }
		auto GetRedoDescription() const -> std::string_view override { return Documents.GetRedoDescription(); }
		auto Undo() -> bool override { return Documents.Undo(); }
		auto Redo() -> bool override { return Documents.Redo(); }
		auto DrawWorkspace(bool bActive) -> bool override;
		auto ResetLayout() -> void override;

	private:
		struct FPreviewState
		{
			std::unique_ptr<FTexturePreview> Preview = std::make_unique<FTexturePreview>();
			EVolumeTexturePreviewAxis Axis = EVolumeTexturePreviewAxis::XY;
			ETexturePreviewChannel Channel = ETexturePreviewChannel::RGBA;
			uint32 Mip = 0;
			uint32 Slice = 0;
			uint64 Revision = 0;
			uint64 SelectionKey = std::numeric_limits<uint64>::max();
			float Zoom = 0.0f;
		};

		auto Find(std::string_view ResourceId) const -> DVolumeTexture*;
		auto Active() const -> DVolumeTexture*;
		auto Save(DVolumeTexture* Texture) -> bool;
		auto DrawDocument(const ::Durin::Editor::FDocumentTab& Document,
			DVolumeTexture* Texture) -> void;
		auto DrawPreview(const std::string& ResourceId, DVolumeTexture* Texture) -> void;
		auto DrawDetails(DVolumeTexture* Texture) -> void;
		auto SetError(std::string Message) -> void { ErrorMessage = std::move(Message); }

		::Durin::Editor::FWorkspaceManager& Manager;
		std::unordered_map<std::string, TObjectPtr<DVolumeTexture>> OpenTextures;
		std::unordered_map<std::string, FPreviewState> PreviewStates;
		::Durin::Editor::FEditableAssetDocumentModel Documents;
		std::string ErrorMessage;
	};
}
