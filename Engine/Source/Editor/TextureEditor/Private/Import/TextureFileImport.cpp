#include "Import/TextureFileImport.h"

#include "AssetTools/IAssetTools.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Import/AssetDestinationValidation.h"
#include "Misc/StringConvert.h"
#include "Asset/AssetCompilingManager.h"

namespace Durin::Editor::Texture
{
	auto FTextureFileImport::ImportFile(std::string_view Filename, std::string_view TargetDirectory)
		-> FAssetOperationResult
	{
		if (bRunning) return {.Kind = EAssetOperationKind::Import,
			.State = EAssetOperationTerminalState::Rejected, .Message = "A texture import is already running."};
		return AdmitFile(Filename, TargetDirectory);
	}

	auto FTextureFileImport::AdmitFile(std::string_view Filename, std::string_view Directory,
		std::shared_ptr<const AssetForge::Builtins::FPreparedTexture2DImport> Prepared) -> FAssetOperationResult
	{
		Callbacks.Clear();
		auto Reject = [this](std::string Message) -> FAssetOperationResult {
			if (!bRunning) Callbacks.Report(Message);
			return {.Kind = EAssetOperationKind::Import,
				.State = EAssetOperationTerminalState::Rejected, .Message = std::move(Message)};
		};
		if (Filename.empty() || !AssetForge::Builtins::IsTexture2DSourceExtension(
			std::filesystem::path(Filename).extension().generic_string()))
			return Reject("Choose a PNG, JPEG, BMP, or TGA texture file.");
		if (Directory.empty()) return Reject("Choose a destination folder in the Content Browser.");
		std::string Prefix(Directory);
		if (!Prefix.ends_with('/')) Prefix.push_back('/');
		const std::string Name = StringUtils::SanitizeFileName(
			std::filesystem::path(Filename).stem().generic_string(), "Texture");
		FTopLevelAssetPath Destination;
		for (uint32 Suffix = 0; Suffix <= 10000; ++Suffix)
		{
			const std::string CandidateName = Suffix == 0 ? Name : std::format("{}_{}", Name, Suffix);
			const auto Candidate = InspectAssetDestination(Prefix + CandidateName);
			if (Candidate.AssetExists()) continue;
			if (!Candidate) return Reject(FormatAssetDestinationValidation(Candidate));
			std::error_code Error;
			const bool bExists = std::filesystem::exists(Candidate.PhysicalPath, Error);
			if (Error) return Reject("Could not inspect the destination: " + Error.message());
			if (bExists) continue;
			if (!FTopLevelAssetPath::TryCreate(Candidate.AssetPath, CandidateName, Destination))
				return Reject("The texture asset name is invalid.");
			break;
		}
		if (!Destination.IsValid()) return Reject("Could not find an available texture name in this folder.");
		auto* Factory = NewObject<AssetForge::Builtins::DTexture2DFactory>(
			nullptr, "TextureFileImportFactory", EObjectFlags::Transient);
		Factory->SetAutoDetectSettings(true);
		const bool bBatchImport = bool(Prepared);
		Factory->SetPreparedImport(std::move(Prepared));
		auto Imported = IAssetTools::Get().ImportAsset(
			Destination, DTexture2D::StaticClass(), Filename, Factory);
		if (!Imported)
		{
			if (!bRunning) Callbacks.Report(Imported.Message);
			return Imported;
		}
		if (bBatchImport) return Imported;
		// Saving/unloading waits for retirement, independently of the compile outcome.
		FAssetCompilingManager::Get().FinishCompilationForObject(*Imported.Asset);
		const auto Saved = Save(Destination.GetPackagePath());
		if (!Saved)
		{
			PendingSaves.push_back(Destination.GetPackagePath());
			Imported.Kind = EAssetOperationKind::Save;
			Imported.State = Saved.State;
			Imported.Message = Saved.Message;
			return Imported;
		}
		Imported.Persistence = Saved.Persistence;
		return Imported;
	}

	auto FTextureFileImport::Save(const FPackagePath& Path) -> FAssetOperationResult
	{
		const FAssetSaveRequest Request{
			.AssetPaths = {Path},
			.Publish = [this, Path](const FAssetOperationNotification&) {
				if (bRunning) Published.push_back(Path.ToString());
				else Callbacks.NotifyAssetCreated(Path.ToString());
			}};
		auto Result = SaveOperation ? SaveOperation(Request) : IAssetTools::Get().SaveAssets(Request);
		if (!Result && !bRunning)
			Callbacks.Report(std::format(
				"Texture {} was imported but could not be saved. Use Import > Retry Texture Saves. {}",
				Path.ToString(), Result.Message));
		return Result;
	}

	auto FTextureFileImport::RetryPendingSaves() -> void
	{
		if (!CanRetrySaves()) return;
		Callbacks.Clear();
		std::erase_if(PendingSaves, [this](const FPackagePath& Path) {
			if (!Save(Path)) return false;
			UnloadPackage(Path);
			return true;
		});
	}

	FTextureFileImport::~FTextureFileImport()
	{
		// Join feature-owned decoding and disk writes before releasing their state.
		if (Preparation.valid()) Preparation.wait();
		DiskSave.reset();
		if (Active)
		{
			// The authored import is already committed. Preserve it, including any
			// external edits, and drain current compilation before releasing ownership.
			FAssetCompilingManager::Get().FinishCompilationForObject(*Active);
			Active.Reset();
		}
	}

	auto FTextureFileImport::Begin(std::vector<std::string> InFiles, std::string InDirectory) -> bool
	{
		if (!bMutationAllowed || bRunning || InFiles.empty() || InDirectory.empty()) return false;
		Files.clear();
		for (auto& File : InFiles)
			if (!std::ranges::contains(Files, File)) Files.push_back(std::move(File));
		Directory = std::move(InDirectory);
		Next = SavedCount = 0;
		Published.clear(); Errors.clear();
		TimingDetails.clear();
		bCancelRequested = false;
		bRunning = true;
		return true;
	}

	auto FTextureFileImport::GetActivity() const -> std::string
	{
		if (!bRunning || Next >= Files.size()) return {};
		return std::format("{} {}/{}: {}", DiskSave ? "Saving" : Active ? "Building" : "Reading",
			Next + 1, Files.size(), std::filesystem::path(Files[Next]).filename().generic_string());
	}

	auto FTextureFileImport::FinishBatch() -> void
	{
		bRunning = false;
		if (Published.size() == 1) Callbacks.NotifyAssetCreated(Published.front());
		else if (!Published.empty()) Callbacks.NotifyImportedDirectory(Directory);
	}

	auto FTextureFileImport::Tick(bool bAllowMutation) -> void
	{
		bMutationAllowed = bAllowMutation;
		if (!bRunning || !bAllowMutation) return;
		auto Fail = [this](std::string Message) {
			Errors.push_back(std::format("{}: {}", Files[Next], Message));
			++Next;
		};
		if (Active)
		{
			if (!Active->IsAsyncCacheComplete()) return;
			const auto CompilationEnd = bSaveStarted ? SaveStarted : std::chrono::steady_clock::now();
			auto* Package = Active->GetPackage();

			FPackagePath Path;
			require(FPackagePath::TryCreate(Package->GetPackagePath(), Path));
			std::optional<FAssetOperationResult> Saved;
			if (!SaveOperation)
			{
				if (!bSaveStarted)
				{
					bSaveStarted = true;
					SaveStarted = CompilationEnd;
					FAssetOperationResult Admission;
					DiskSave = FAssetSaveOperation::Begin({.AssetPaths = {Path},
						.Publish = [this, Path](const FAssetOperationNotification&) {
							Published.push_back(Path.ToString());
						}}, Admission);
					if (!DiskSave) Saved = std::move(Admission);
				}
				if (DiskSave)
				{
					if (!DiskSave->IsReady()) return;
					Saved = DiskSave->Complete();
					DiskSave.reset();
				}
			}
			const auto Filename = Files[Next];
			double SaveMilliseconds = 0;
			const auto SaveStart = bSaveStarted ? SaveStarted : std::chrono::steady_clock::now();
			const auto Result = Saved ? std::move(*Saved) : Save(Path);
			SaveMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - SaveStart).count();
			if (Result) { ++SavedCount; ++Next; }
			else { PendingSaves.push_back(Path); Fail("Imported, but save failed: " + Result.Message); }
			Active.Reset();
			if (Result && !Package->IsDirty()) UnloadPackage(Package);
			TimingDetails += std::format(
				"{}: prepare {:.1f} ms, compilation elapsed {:.1f} ms, save {:.1f} ms\n",
				Filename, PreparationMilliseconds,
				std::chrono::duration<double, std::milli>(CompilationEnd - CompilationStart).count(),
				SaveMilliseconds);
			bSaveStarted = false;
			return;
		}
		if (Preparation.valid())
		{
			if (Preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			auto Prepared = Preparation.get();
			PreparationMilliseconds = Prepared.PreparationMilliseconds;
			if (bCancelRequested) { FinishBatch(); return; }
			if (!Prepared.Data)
			{
				Fail(std::move(Prepared.Message));
				return;
			}
			CompilationStart = std::chrono::steady_clock::now();
			const auto Result = AdmitFile(Files[Next], Directory, std::move(Prepared.Data));
			if (!Result) { Fail(Result.Message); return; }
			Active = Cast<DTexture2D>(Result.Asset);
			return;
		}
		if (bCancelRequested || Next == Files.size()) { FinishBatch(); return; }
		try
		{
			Preparation = std::async(std::launch::async, PrepareFile, Files[Next]);
		}
		catch (const std::exception& Error) { Fail(Error.what()); }
	}

	auto FTextureFileImport::PrepareFile(const std::string& Filename) -> FPreparation
	{
		FPreparation Result;
		const auto Start = std::chrono::steady_clock::now();
		try
		{
			auto Prepared = AssetForge::Builtins::PrepareTexture2DImport(Filename);
			if (!Prepared)
				Result.Message = Prepared.error();
			else
				Result.Data = std::make_shared<AssetForge::Builtins::FPreparedTexture2DImport>(std::move(*Prepared));
		}
		catch (const std::exception& Error) { Result.Data.reset(); Result.Message = Error.what(); }
		Result.PreparationMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - Start).count();
		return Result;
	}

}
