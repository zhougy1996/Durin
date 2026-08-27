#pragma once

#include "Editor/Import/ImportDialogSupport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::StaticMesh
{
	using namespace ::Durin::Editor::Import;
	// Creates one geometry-only StaticMesh without Scene materials or textures.
	class FStaticMeshImportDialog
	{
	public:
		explicit FStaticMeshImportDialog(FImportDialogCallbacks InCallbacks);
		FStaticMeshImportDialog(const FStaticMeshImportDialog&) = delete;
		auto operator=(const FStaticMeshImportDialog&) -> FStaticMeshImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw(bool bAllowAssetMutation) -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		std::array<char, 512> SourcePathBuffer{};
		FMeshCoordinateImportModel Coordinates;
	};
} // namespace Durin::Editor::StaticMesh
