#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"

namespace Durin
{
	enum class ETextureUsage : uint8;
}

namespace Durin::Editor::Level
{
	// Collects texture import options and submits the selected source file.
	class FTextureImportDialog
	{
	public:
		explicit FTextureImportDialog(FImportDialogCallbacks InCallbacks);

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
		std::array<char, 512> SourcePathBuffer{};
		std::array<char, 512> SourceDestinationBuffer{};
		std::string LastSuggestedSourceDestination;
		ETextureUsage Usage = static_cast<ETextureUsage>(0);
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
	};
} // namespace Durin::Editor::Level
