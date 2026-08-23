#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"
#include "VolumeTextureSourceTranslation.h"

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
		auto InspectVolumeTextureSource() -> void;
		auto ApplyVolumeTextureLayoutSuggestion(size_t Index) -> void;
		auto ImportSelectedTexture() -> bool;
		auto ImportSingleTexture() -> bool;
		auto SetError(std::string Message) -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		FTextureImportDialogState State;
		Asset::Forge::FVolumeTextureAtlasInspection VolumeInspection;
		std::string InspectedVolumeSourcePath;
		std::string SubmissionError;
		std::optional<Asset::FInterchangeImportHandle> TextureCubePreview;
		std::string PendingTextureCubePreviewKey;
		std::string ValidatedTextureCubePreviewKey;
		int SelectedVolumeLayout = -1;
	};
} // namespace Durin::Editor::Level
