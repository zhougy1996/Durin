#include "Import/TextureFileImport.h"

#include "AssetTools/IAssetTools.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "DObject/DObjectGlobals.h"
#include "Editor/Import/AssetDestinationValidation.h"
#include "Misc/StringHelper.h"

namespace Durin::Editor::Texture
{
	auto FTextureFileImport::ImportFile(std::string_view Filename, std::string_view Directory)
		-> FAssetOperationResult
	{
		Callbacks.Clear();
		auto Reject = [this](std::string Message) -> FAssetOperationResult {
			Callbacks.Report(Message);
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
		auto Imported = IAssetTools::Get().ImportAsset(
			Destination, DTexture2D::StaticClass(), Filename, Factory);
		if (!Imported)
		{
			Callbacks.Report(Imported.Message);
			return Imported;
		}
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
				Callbacks.NotifyAssetCreated(Path.ToString());
			}};
		auto Result = SaveOperation ? SaveOperation(Request) : IAssetTools::Get().SaveAssets(Request);
		if (!Result)
			Callbacks.Report(std::format(
				"Texture {} was imported but could not be saved. Use Import > Retry Texture Saves. {}",
				Path.ToString(), Result.Message));
		return Result;
	}

	auto FTextureFileImport::RetryPendingSaves() -> void
	{
		Callbacks.Clear();
		std::erase_if(PendingSaves, [this](const FPackagePath& Path) {
			if (!Save(Path)) return false;
			UnloadPackage(Path);
			return true;
		});
	}
}
