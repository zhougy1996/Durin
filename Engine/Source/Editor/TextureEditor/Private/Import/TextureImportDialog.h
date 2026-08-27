#pragma once

#include "Editor/Import/ImportDialogSupport.h"
#include "Import/TextureImportDialogState.h"
#include "AssetForge/Builtins/VolumeTextureImport.h"

namespace Durin::Editor::Texture
{
	// Creates Texture2D, TextureCube, or VolumeTexture assets through one modal.
	class FTextureImportDialog
	{
	public:
		explicit FTextureImportDialog(FImportDialogCallbacks InCallbacks);
		FTextureImportDialog(const FTextureImportDialog&) = delete;
		auto operator=(const FTextureImportDialog&) -> FTextureImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw(bool bAllowAssetMutation) -> void;

	private:
		auto DrawSingleSource(float BrowseButtonWidth) -> void;
		auto DrawSingleSettings() -> void;
		auto ValidateAndDrawSingleDestination() -> std::string;

		auto DrawTextureCubeSource() -> void;
		auto ValidateAndDrawTextureCubeDestination() -> std::string;
		auto BrowseFace(ETextureCubeFace Face) -> void;
		auto BrowsePanorama() -> void;
		auto RevalidateTextureCubeSources() -> bool;
		auto ImportTextureCube() -> bool;
		auto SuggestTextureCubeAssetPath(std::string_view SourceFile) -> void;

		auto GetSelectedSingleSourcePath() -> std::array<char, 512>&;
		auto BrowseSingleSource() -> void;
		auto BrowseDestination() -> void;
		auto InspectVolumeTextureSource() -> void;
		auto ApplyVolumeTextureLayoutSuggestion(size_t Index) -> void;
		auto ImportSelectedTexture() -> bool;
		auto ImportSingleTexture() -> bool;
		auto SetError(std::string Message) -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		FTextureImportDialogState State;
		AssetForge::Builtins::FVolumeTextureAtlasInspection VolumeInspection;
		std::string InspectedVolumeSourcePath;
		std::string SubmissionError;
		int SelectedVolumeLayout = -1;
	};
} // namespace Durin::Editor::Texture
