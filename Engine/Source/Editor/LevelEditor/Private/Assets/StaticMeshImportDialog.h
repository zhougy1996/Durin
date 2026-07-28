#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"
#include "StaticModelImportBuild.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	// Selects a predefined static-mesh import policy.
	enum class EStaticMeshImportPreset : uint8
	{
		Durin,
		YUpNegativeZForward,
		Custom
	};

	// Collects static-mesh import options and reports import progress.
	class FStaticMeshImportDialog
	{
	public:
		explicit FStaticMeshImportDialog(FImportDialogCallbacks InCallbacks);

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestination() -> void;
		auto BrowseSourceDestination() -> void;
		auto RefreshPreview(const FAssetPath& RootAssetPath) -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDestinationModel Destination;
		FImportDialogModalState ModalState;
		std::array<char, 512> SourcePathBuffer{};
		std::array<char, 512> SourceDestinationBuffer{};
		std::string LastSuggestedSourceDestination;
		FStaticMeshImportSettings ImportSettings;
		EStaticMeshImportPreset ImportPreset = EStaticMeshImportPreset::Durin;
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
		std::string PreviewKey;
		std::optional<FStaticModelImportPlanResult> Preview;
	};
} // namespace Durin
