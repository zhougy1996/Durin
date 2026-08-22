#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"

namespace Durin
{
	enum class ETextureUsage : uint8;
	enum class EVolumeTextureSourceChannels : uint8;
}

namespace Durin::Editor::Level
{
	// Collects texture import options and submits the selected source file.
	class FTextureImportDialog
	{
	public:
		explicit FTextureImportDialog(FImportDialogCallbacks InCallbacks);
		FTextureImportDialog(const FTextureImportDialog&) = delete;
		auto operator=(const FTextureImportDialog&) -> FTextureImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto BrowseSourceDestination() -> void;
		auto SuggestSourceDestination() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		FMountedSourceImportFormModel SourceForm;
		std::array<char, 512>& SourcePathBuffer = SourceForm.GetSourcePathBuffer();
		std::array<char, 512>& SourceDestinationBuffer = SourceForm.GetDestinationBuffer();
		ETextureUsage Usage = static_cast<ETextureUsage>(0);
		bool bImportVolume = false;
		EVolumeTextureSourceChannels VolumeChannels = static_cast<EVolumeTextureSourceChannels>(0);
		uint32 VolumeSliceWidth = 128;
		uint32 VolumeSliceHeight = 128;
		uint32 VolumeDepth = 128;
		uint32 VolumeTilesX = 12;
		uint32 VolumeTilesY = 12;
		EMountedSourceImportMode& SourceMode = SourceForm.GetMode();
	};
} // namespace Durin::Editor::Level
