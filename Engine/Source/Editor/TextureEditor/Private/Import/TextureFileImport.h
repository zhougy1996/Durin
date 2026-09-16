#pragma once

#include "AssetTools/AssetSave.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "DObject/StrongObjectPtr.h"
#include <future>
#include "Editor/Import/ImportDialogSupport.h"

namespace Durin::Editor::Texture
{
	// UI-independent first-import/save policy. Failed saves retain resident assets
	// and can be retried without reopening or decoding the source.
	class FTextureFileImport
	{
	public:
		using FSaveOperation = std::function<FAssetOperationResult(const FAssetSaveRequest&)>;
		explicit FTextureFileImport(FImportDialogCallbacks InCallbacks = {}, FSaveOperation InSave = {})
			: Callbacks(std::move(InCallbacks)), SaveOperation(std::move(InSave)) {}
		~FTextureFileImport();
		// One bounded batch at a time. Tick is called by the editor host, including
		// when the browser is hidden. Only detached capture/decode runs on our worker.
		auto Begin(std::vector<std::string> Files, std::string Directory) -> bool;
		auto Tick(bool bAllowMutation = true) -> void;
		auto Cancel() -> void { bCancelRequested = true; }
		auto IsRunning() const -> bool { return bRunning; }
		auto GetCompletedCount() const -> size_t { return Next; }
		auto GetTotalCount() const -> size_t { return Files.size(); }
		auto GetSavedCount() const -> size_t { return SavedCount; }
		auto GetFailedCount() const -> size_t { return Errors.size(); }
		auto GetCanceledCount() const -> size_t { return bRunning ? 0 : Files.size() - Next; }
		auto GetErrors() const -> const std::vector<std::string>& { return Errors; }
		auto GetActivity() const -> std::string;
		auto GetTimingDetails() const -> const std::string& { return TimingDetails; }
		auto ImportFile(std::string_view Filename, std::string_view Directory)
			-> FAssetOperationResult;
		auto RetryPendingSaves() -> void;
		auto HasPendingSaves() const -> bool { return !PendingSaves.empty(); }
		auto CanRetrySaves() const -> bool { return bMutationAllowed && !bRunning && HasPendingSaves(); }

	private:
		auto AdmitFile(std::string_view Filename, std::string_view Directory,
			std::shared_ptr<const AssetForge::Builtins::FPreparedTexture2DImport> Prepared = {},
			FTexture2DCompilationCompletion Completion = {}) -> FAssetOperationResult;
		auto FinishBatch() -> void;
		auto Save(const FPackagePath& Path) -> FAssetOperationResult;
		FImportDialogCallbacks Callbacks;
		FSaveOperation SaveOperation;
		std::vector<FPackagePath> PendingSaves;
		struct FPreparation
		{
			std::shared_ptr<AssetForge::Builtins::FPreparedTexture2DImport> Data;
			std::string Error;
			double PreparationMilliseconds = 0;
		};
		std::future<FPreparation> Preparation;
		std::shared_ptr<std::optional<FTexture2DCompilationResult>> Completion;
		TStrongObjectPtr<DTexture2D> Active;
		std::unique_ptr<FAssetSaveOperation> DiskSave;
		bool bSaveStarted = false;
		std::chrono::steady_clock::time_point SaveStarted;
		std::vector<std::string> Files;
		std::string Directory;
		std::vector<std::string> Published;
		std::vector<std::string> Errors;
		std::string TimingDetails;
		double PreparationMilliseconds = 0;
		std::chrono::steady_clock::time_point CompilationStart;
		size_t Next = 0, SavedCount = 0;
		bool bRunning = false, bCancelRequested = false, bMutationAllowed = true;
	};
}
