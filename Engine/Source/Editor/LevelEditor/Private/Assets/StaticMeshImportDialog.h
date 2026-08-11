#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Level
{
	// Creates one geometry-only StaticMesh without Scene materials or textures.
	class FStaticMeshImportDialog
	{
	public:
		explicit FStaticMeshImportDialog(FImportDialogCallbacks InCallbacks);

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		enum class EImportPreset : uint8
		{
			Durin,
			YUpNegativeZForward,
			Custom
		};

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
		FStaticMeshImportSettings ImportSettings =
			FStaticMeshImportSettings::MakeDurin();
		EImportPreset ImportPreset = EImportPreset::Durin;
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
	};
} // namespace Durin::Editor::Level
