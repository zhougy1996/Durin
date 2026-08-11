#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"
#include "SceneImport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Level
{
	// Selects the mesh-coordinate policy applied while importing a Scene source.
	enum class ESceneMeshImportPreset : uint8
	{
		Durin,
		YUpNegativeZForward,
		Custom
	};

	// Imports one supported Scene source into a typed multi-asset directory.
	class FSceneImportDialog
	{
	public:
		explicit FSceneImportDialog(FImportDialogCallbacks InCallbacks);
		~FSceneImportDialog();

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw() -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestinationDirectory() -> void;
		auto BrowseSourceDestination() -> void;
		auto SuggestSourceDestination() -> void;
		auto RefreshPreview(const FAssetPath& DestinationDirectory) -> void;
		auto Import() -> bool;
		auto PollImport() -> bool;
		auto CancelRequests() -> void;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDirectoryModel DestinationDirectory;
		FImportDialogModalState ModalState;
		std::array<char, 512> SourcePathBuffer{};
		std::array<char, 512> SourceDestinationBuffer{};
		std::string LastSuggestedSourceDestination;
		FStaticMeshImportSettings ImportSettings;
		ESceneMeshImportPreset ImportPreset = ESceneMeshImportPreset::Durin;
		EMountedSourceImportMode SourceMode =
			EMountedSourceImportMode::IngestExternal;
		std::string PreviewKey;
		std::optional<FSceneImportPlanResult> Preview;
		std::optional<FSceneImportAsyncPlanHandle> PreviewRequest;
		std::optional<FSceneImportAsyncPlanHandle> ImportRequest;
	};
} // namespace Durin::Editor::Level
