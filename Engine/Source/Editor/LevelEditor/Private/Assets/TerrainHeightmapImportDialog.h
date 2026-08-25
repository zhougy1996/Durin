#pragma once

#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::Editor::Level
{
	using namespace ::Durin::Editor::Import;
	// Collects the source and destination for an explicit lossless heightmap import.
	class FTerrainHeightmapImportDialog
	{
	public:
		explicit FTerrainHeightmapImportDialog(FImportDialogCallbacks InCallbacks);

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		std::array<char, 512> SourcePathBuffer{};
	};
}
