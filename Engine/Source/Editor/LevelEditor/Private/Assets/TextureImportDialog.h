#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"

namespace Durin::Editor::Level
{
	// Creates Texture2D, TextureCube, or VolumeTexture assets through one modal.
	class FTextureImportDialog
	{
	public:
		explicit FTextureImportDialog(FImportDialogCallbacks InCallbacks);
		FTextureImportDialog(const FTextureImportDialog&) = delete;
		auto operator=(const FTextureImportDialog&) -> FTextureImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto DrawSourceMode() -> void;
		auto DrawSingleSource(float BrowseButtonWidth) -> void;
		auto DrawSingleSettings() -> void;
		auto DrawSingleSourceDestination(float BrowseButtonWidth) -> void;
		auto ValidateAndDrawSingleDestination() -> std::string;

		auto DrawTextureCubeSource() -> void;
		auto DrawTextureCubeSourceDestinations() -> void;
		auto ValidateAndDrawTextureCubeDestination() -> std::string;
		auto BrowseFace(ETextureCubeFace Face) -> void;
		auto BrowsePanorama() -> void;
		auto RevalidateTextureCubeSources() -> bool;
		auto ImportTextureCube() -> bool;
		auto SuggestTextureCubeAssetPath(std::string_view SourceFile) -> void;
		auto SuggestTextureCubeSourceDestinations() -> void;

		auto GetSelectedSingleSource() -> FMountedSourceImportFormModel&;
		auto BrowseSingleSource() -> void;
		auto BrowseDestination() -> void;
		auto BrowseSingleSourceDestination() -> void;
		auto SuggestSelectedSourceDestinations() -> void;
		auto SuggestSingleSourceDestination() -> void;
		auto ImportSelectedTexture() -> bool;
		auto ImportSingleTexture() -> bool;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		FTextureImportDialogState State;
	};
} // namespace Durin::Editor::Level
