#include "Import/TextureFileImport.h"

#include "AssetTools/IAssetTools.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Editor/Import/AssetDestinationValidation.h"
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
		std::shared_ptr<const AssetForge::Builtins::FPreparedTexture2DImport> Prepared,
		FTexture2DCompilationCompletion Done) -> FAssetOperationResult
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
			if (!Candidate) return Reject(Candidate.Message);
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
		const bool bDeferred = bool(Prepared);
		Factory->SetPreparedImport(std::move(Prepared), std::move(Done));
		auto Imported = IAssetTools::Get().ImportAsset(
			Destination, DTexture2D::StaticClass(), Filename, Factory);
		if (!Imported)
		{
			if (!bRunning) Callbacks.Report(Imported.Message);
			return Imported;
		}
		if (bDeferred) return Imported;
		const auto Saved = Save(Destination.GetPackagePath());
		if (!Saved)
		{
			PendingSaves.push_back(Destination.GetPackagePath());
			Imported.State = Saved.State;
			Imported.Message = Saved.Message;
			return Imported;
		}
		Imported.Persistence = Saved.Persistence;
		Imported.bPublished = Saved.bPublished;
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
		// Join detached decoding before unloading feature code. Compiler completion
		// owns only a result cell, never this importer or the host callbacks.
		if (Preparation.valid()) Preparation.wait();
		DiskSave.reset(); // Drain detached writes before discarding an unfinished asset.
		if (Active)
		{
			auto* Package = Active->GetPackage();
			FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Active);
			FAssetCompilingManager::Get().FinishCompilationForObject(*Active);
			Active.Reset();
			UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved);
		}
	}

	auto FTextureFileImport::Begin(std::vector<std::string> InFiles, std::string InDirectory) -> bool
	{
		if (!bMutationAllowed || bRunning || InFiles.empty() || InDirectory.empty()) return false;
		Files.clear();
		for (auto& File : InFiles)
			if (std::ranges::find(Files, File) == Files.end()) Files.push_back(std::move(File));
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
			if (!Completion || !Completion->has_value()) return;
			const auto CompilationEnd = bSaveStarted ? SaveStarted : std::chrono::steady_clock::now();
			const auto Diagnostic = GetTexture2DCompilationDiagnostic(*Active);
			auto* Package = Active->GetPackage();
			FPackagePath Path;
			require(FPackagePath::TryCreate(Package->GetPackagePath(), Path));
			std::optional<FAssetOperationResult> Saved;
			if (Completion->value().Succeeded() && !SaveOperation)
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
			if (Completion->value().Succeeded())
			{
				const auto SaveStart = bSaveStarted ? SaveStarted : std::chrono::steady_clock::now();
				const auto Result = Saved ? std::move(*Saved) : Save(Path);
				SaveMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - SaveStart).count();
				if (Result) { ++SavedCount; ++Next; }
				else { PendingSaves.push_back(Path); Fail(Result.Message); }
				Active.Reset();
				if (Result && !Package->IsDirty()) UnloadPackage(Package);
			}
			else
			{
				Fail(Completion->value().Diagnostic);
				Active.Reset();
				UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved);
			}
			TimingDetails += std::format(
				"{}: prepare {:.1f} ms, compilation elapsed {:.1f} ms (mips {:.1f}, compression {:.1f}, cache write {:.1f} ms; {}), save {:.1f} ms\n",
				Filename, PreparationMilliseconds,
				std::chrono::duration<double, std::milli>(CompilationEnd - CompilationStart).count(),
				Diagnostic.Metrics.MipGenerationNanoseconds / 1e6,
				Diagnostic.Metrics.CompressionNanoseconds / 1e6,
				Diagnostic.Metrics.PersistenceNanoseconds / 1e6,
				Diagnostic.Origin == ETexture2DCompilationOrigin::CacheHit ? "cache hit" : "build",
				SaveMilliseconds);
			Completion.reset();
			bSaveStarted = false;
			return;
		}
		if (Preparation.valid())
		{
			if (Preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
			auto Prepared = Preparation.get();
			PreparationMilliseconds = Prepared.PreparationMilliseconds;
			if (!Prepared.Data) { Fail(std::move(Prepared.Error)); return; }
			Completion = std::make_shared<std::optional<FTexture2DCompilationResult>>();
			CompilationStart = std::chrono::steady_clock::now();
			const auto Result = AdmitFile(Files[Next], Directory, std::move(Prepared.Data),
				[Cell = Completion](FTexture2DCompilationResult Value) { *Cell = std::move(Value); });
			if (!Result) { Completion.reset(); Fail(Result.Message); return; }
			Active = Cast<DTexture2D>(Result.Asset);
			return;
		}
		if (bCancelRequested || Next == Files.size()) { FinishBatch(); return; }
		try
		{
			Preparation = std::async(std::launch::async, [Filename = Files[Next]] {
				FPreparation Result;
				const auto Start = std::chrono::steady_clock::now();
				try
				{
					Result.Data = std::make_shared<AssetForge::Builtins::FPreparedTexture2DImport>();
					if (!AssetForge::Builtins::PrepareTexture2DImport(Filename, *Result.Data, Result.Error))
						Result.Data.reset();
				}
				catch (const std::exception& Error) { Result.Data.reset(); Result.Error = Error.what(); }
				Result.PreparationMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - Start).count();
				return Result;
			});
		}
		catch (const std::exception& Error) { Fail(Error.what()); }
	}

}
