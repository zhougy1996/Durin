#include "Import/TextureFileImport.h"
#include "AssetTools/PackageUnloading.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"

#include "AssetTools/IAssetTools.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Import/AssetDestinationValidation.h"
#include "Misc/StringConvert.h"
#include "Asset/AssetCompilingManager.h"

namespace Durin::TextureEditor
{
	auto FTextureFileImport::ImportFile(std::string_view Filename, std::string_view TargetDirectory) -> DObject*
	{
		if (bRunning) { Callbacks.Report("A texture import is already running."); return nullptr; }
		FFeedbackContext Feedback({}, [this](ELogLevel, std::string_view Message) { Callbacks.Report(std::string(Message)); });
		return AdmitFile(Filename, TargetDirectory, {}, &Feedback);
	}

	auto FTextureFileImport::AdmitFile(std::string_view Filename, std::string_view Directory,
		std::shared_ptr<const AssetForge::Builtins::FPreparedTexture2DImport> Prepared, FFeedbackContext* Warn) -> DObject*
	{
		Callbacks.Clear();
		const auto Reject = [Warn](std::string_view Message) -> DObject* {
			(Warn ? *Warn : FFeedbackContext::Get()).Error(Message);
			return nullptr;
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
		auto* Imported = IAssetTools::Get().ImportAsset(Destination, DTexture2D::StaticClass(), Filename, Factory,
			nullptr, Durin::DefaultAssetCreationFlags, Warn);
		if (!Imported || bBatchImport) return Imported;
		FAssetCompilingManager::Get().FinishCompilationForObject(*Imported);
		if (!Save(Destination.GetPackagePath())) PendingSaves.push_back(Destination.GetPackagePath());
		return Imported;
	}

	auto FTextureFileImport::Save(const FPackagePath& Path) -> bool
	{
		std::string Error;
		FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
			if (Level >= ELogLevel::Error)
			{
				if (!Error.empty()) Error += "\n";
				Error += Message;
			}
		});
		const bool bSaved = IAssetTools::Get().SaveAssets({.AssetPaths = {Path}}, &Feedback);
		if (bSaved)
		{
			if (bRunning) Published.push_back(Path.ToString());
			else Callbacks.NotifyAssetCreated(Path.ToString());
		}
		else if (!bRunning) Callbacks.Report(std::format(
			"Texture {} was imported but could not be saved. Use Import > Retry Texture Saves. {}", Path.ToString(), Error));
		return bSaved;
	}

	auto FTextureFileImport::RetryPendingSaves() -> void
	{
		if (!CanRetrySaves()) return;
		Callbacks.Clear();
		std::erase_if(PendingSaves, [this](const FPackagePath& Path) {
			if (!Save(Path)) return false;
			PackageTools::UnloadPackages(std::array{FindResidentPackage(Path)});
			return true;
		});
	}

	FTextureFileImport::~FTextureFileImport()
	{
		// Join feature-owned decoding. Engine owns admitted saves independently of this observer.
		if (Preparation.valid()) Preparation.wait();
		DiskSave = {};
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
		return std::format("{} {}/{}: {}", DiskSave.IsValid() ? "Saving" : Active ? "Building" : "Reading",
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
			const auto CompilationEnd = DiskSave.IsValid() ? SaveStarted : std::chrono::steady_clock::now();
			auto* Package = Active->GetPackage();

			FPackagePath Path;
			require(FPackagePath::TryCreate(Package->GetPackagePath(), Path));
			FAssetWriteResult Saved;
			if (!DiskSave.IsValid())
			{
				SaveStarted = CompilationEnd;
				DiskSave = Package->SaveAsync(Saved, FAssetPackageSaveContext{});
			}
			if (DiskSave.IsValid())
			{
				if (!DiskSave.IsCompleted()) return;
				Saved = DiskSave.GetState() == ETaskState::Succeeded ? DiskSave.GetResult()
					: FAssetWriteResult{EAssetWriteError::IoError, "Save task failed or was canceled."};
				DiskSave = {};
			}
			const auto Filename = Files[Next];
			double SaveMilliseconds = 0;
			const auto& Result = Saved;
			SaveMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - SaveStarted).count();
			if (Result) { Published.push_back(Path.ToString()); ++SavedCount; ++Next; }
			else { PendingSaves.push_back(Path); Fail("Imported, but save failed: " + Result.Message); }
			Active.Reset();
			if (Result && !Package->IsDirty()) PackageTools::UnloadPackages(std::array{Package});
			TimingDetails += std::format(
				"{}: prepare {:.1f} ms, compilation elapsed {:.1f} ms, save {:.1f} ms\n",
				Filename, PreparationMilliseconds,
				std::chrono::duration<double, std::milli>(CompilationEnd - CompilationStart).count(),
				SaveMilliseconds);
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
			std::string Error;
			FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
				if (Level >= ELogLevel::Error) Error += Message;
				else FFeedbackContext::Get().Serialize(Level, Message);
			});
			auto* Result = AdmitFile(Files[Next], Directory, std::move(Prepared.Data), &Feedback);
			if (!Result) { Fail(Error); return; }
			Active = Cast<DTexture2D>(Result);
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
