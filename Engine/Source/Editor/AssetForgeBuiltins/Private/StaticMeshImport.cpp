#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"

#include "Asset/AssetOperations.h"
#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Misc/Paths.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		constexpr uint64 MaximumStaticMeshEncodedBytes = 512ull * 1024ull * 1024ull;
		constexpr std::string_view StaticMeshImporterId = "Assimp";
		constexpr uint32 StaticMeshAssimpImporterVersion = 3;

		auto ResolveOwningPackagePhysicalPath(const DStaticMesh& Mesh,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			if (!Mesh.GetPackage()) return false;
			const PathUtilities::FAssetPathResult Resolved =
				PathUtilities::ResolveAssetPath(Mesh.GetPackage()->GetPackagePath(),
					PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved) { OutError = Resolved.Message; return false; }
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return true;
		}

		auto IsSupportedExtension(std::string_view Extension) -> bool
		{
			std::string Lower(Extension);
			std::ranges::transform(Lower, Lower.begin(), [](unsigned char Value) {
				return static_cast<char>(std::tolower(Value));
			});
			return Lower == ".obj" || Lower == ".fbx" || Lower == ".gltf"
				|| Lower == ".glb" || Lower == ".dae" || Lower == ".3ds"
				|| Lower == ".ply" || Lower == ".stl";
		}

		auto AxisVector(EStaticMeshImportAxis Axis) -> FVector3f
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: return {1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveY: return {0.0f, 1.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeY: return {0.0f, -1.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveZ: return {0.0f, 0.0f, 1.0f};
			case EStaticMeshImportAxis::NegativeZ: return {0.0f, 0.0f, -1.0f};
			}
			return {};
		}

		auto MakeOptions(const FStaticMeshImportSettings& Settings,
			std::string SourceFilename) -> FMeshImportOptions
		{
			const FVector3f Forward = AxisVector(Settings.ForwardAxis);
			const FVector3f Right = AxisVector(Settings.RightAxis);
			const FVector3f Up = AxisVector(Settings.UpAxis);
			FMeshImportOptions Options;
			for (uint32 Component = 0; Component < 3; ++Component)
			{
				Options.SourceToEngine[Component][0] = Forward[Component];
				Options.SourceToEngine[Component][1] = Right[Component];
				Options.SourceToEngine[Component][2] = Up[Component];
			}
			Options.RootSourcePath = std::move(SourceFilename);
			return Options;
		}

		auto PrepareImportData(DStaticMesh& Mesh, std::string Filename,
			ESourceHintBase HintBase,
			const std::filesystem::path& PhysicalPath,
			const FEncodedSourceSnapshot& Snapshot,
			const FStaticMeshImportSettings& Settings,
			DStaticMeshImportData*& OutData, std::string& OutError) -> bool
		{
			FStaticMeshImportDataState State;
			State.SourceData.Sources.push_back({
				.Role = "source",
				.DisplayLabel = PhysicalPath.filename().generic_string(),
				.Hint = std::move(Filename),
				.HintBase = HintBase,
				.ContentHashLow = Snapshot.ContentHash.HashLow,
				.ContentHashHigh = Snapshot.ContentHash.HashHigh,
				.ByteCount = Snapshot.FileSize});
			State.ImporterId = std::string(StaticMeshImporterId);
			State.ImporterVersion = StaticMeshAssimpImporterVersion;
			State.ImportSettings = Settings;
			OutData = dynamic_cast<DStaticMeshImportData*>(Mesh.GetAssetImportData());
			if (!OutData) OutData = NewObject<DStaticMeshImportData>(
				&Mesh, "AssetImportData");
			return OutData && OutData->SetState(std::move(State), OutError);
		}

		auto RebuildFromFilename(DStaticMesh& Mesh, std::string Filename,
			ESourceHintBase HintBase,
			const FStaticMeshImportSettings& Settings, std::string& OutError,
			const Asset::FAssetBundleSaveOptions* SaveOptions,
			std::optional<std::filesystem::path> SelectedPhysicalPath = {}) -> bool
		{
			if (!Settings.IsValid(&OutError)) return false;
			std::filesystem::path OwningPackagePath;
			const bool bPackaged = ResolveOwningPackagePhysicalPath(
				Mesh, OwningPackagePath, OutError);
			if (!SelectedPhysicalPath && !bPackaged) return false;
			std::filesystem::path PhysicalPath;
			if (SelectedPhysicalPath) PhysicalPath = std::move(*SelectedPhysicalPath);
			else
			{
				std::string PhysicalPathText;
				if (!ResolveSourceHint(HintBase, Filename,
					OwningPackagePath.generic_string(), PhysicalPathText, OutError)) return false;
				PhysicalPath = PhysicalPathText;
			}
			if (!std::filesystem::is_regular_file(PhysicalPath)
				|| !IsSupportedExtension(PhysicalPath.extension().generic_string()))
			{
				OutError = "StaticMesh source is missing or uses an unsupported format.";
				return false;
			}
			if (SelectedPhysicalPath)
			{
				if (bPackaged)
				{
					if (!MakeSourceHint(PhysicalPath.generic_string(),
						OwningPackagePath.generic_string(), HintBase, Filename, OutError))
						return false;
				}
				else
				{
					HintBase = ESourceHintBase::Absolute;
					Filename = PhysicalPath.generic_string();
				}
			}
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(Filename, PhysicalPath, Snapshot,
				OutError, MaximumStaticMeshEncodedBytes)) return false;
			FImportedSceneData Scene;
			if (!ImportGeometryFromMemory(Snapshot.GetBytes(),
				PhysicalPath.extension().generic_string(), Scene,
				MakeOptions(Settings, {})))
			{
				OutError = std::format("Failed to decode StaticMesh source {}.", Filename);
				return false;
			}
			FStaticMeshSourceImportData Legacy{
				.SourceFilename = Snapshot.Filename,
				.SourceContentHash = Snapshot.ContentHash.ToString(),
				.ImporterId = std::string(StaticMeshImporterId),
				.ImporterVersion = StaticMeshAssimpImporterVersion,
				.ImportSettings = Settings};
			FStaticMeshBuildProduct Product;
			if (!Asset::FStaticMeshBuildOperations::BuildImportedProduct(
				Asset::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(Mesh),
				MakeStaticMeshImportedData(Scene), Legacy, Filename, Product, OutError))
				return false;
			Product.bSourceImporterInvoked = true;
			DStaticMeshImportData* ImportData = nullptr;
			if (!PrepareImportData(Mesh, Filename, HintBase, PhysicalPath, Snapshot,
				Settings, ImportData, OutError)
				|| !Asset::FStaticMeshBuildOperations::PublishImportedProduct(
					Mesh, std::move(Product), OutError)
				|| !Mesh.PublishAssetImportData(*ImportData, OutError)) return false;
			if (!SaveOptions) return true;
			DPackage* Package = Mesh.GetPackage();
			const Asset::FAssetResult Saved = Asset::SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}
	}

	auto ReimportStaticMesh(DStaticMesh& Mesh, std::string& OutError,
		const Asset::FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(
			Mesh.GetAssetImportData());
		if (!Data)
		{
			OutError = "StaticMesh has no current family import data.";
			return false;
		}
		const FStaticMeshImportDataState State = Data->GetStaticMeshState();
		const FSourceFile* Source = State.SourceData.FindByRole("source");
		if (!Source)
		{
			OutError = "StaticMesh has no source filename to reimport.";
			return false;
		}
		return RebuildFromFilename(
			Mesh, Source->Hint, Source->HintBase,
			State.ImportSettings, OutError, &SaveOptions);
	}

	auto ReimportStaticMeshFromFile(DStaticMesh& Mesh, std::string_view FilePath,
		std::string& OutError,
		const Asset::FAssetBundleSaveOptions& SaveOptions) -> bool
	{
		const auto* Data = dynamic_cast<const DStaticMeshImportData*>(
			Mesh.GetAssetImportData());
		if (!Data)
		{
			OutError = "StaticMesh has no current family import parameters.";
			return false;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Requested))
		{
			OutError = "The selected StaticMesh source file does not exist.";
			return false;
		}
		return RebuildFromFilename(Mesh, {}, ESourceHintBase::Absolute,
			Data->GetStaticMeshState().ImportSettings, OutError, &SaveOptions, Requested);
	}

	auto CreateTransientStaticMeshFromFile(std::string_view FilePath,
		DObject* Outer, std::string_view ObjectName, std::string& OutError,
		const FStaticMeshImportSettings& ImportSettings) -> DStaticMesh*
	{
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)) return nullptr;
		auto* Mesh = NewObject<DStaticMesh>(Outer, ObjectName);
		if (Mesh && RebuildFromFilename(
			*Mesh, {}, ESourceHintBase::Absolute,
			ImportSettings, OutError, nullptr, Input)) return Mesh;
		if (Mesh) MarkAsGarbage(Mesh);
		return nullptr;
	}

	auto ImportStaticMeshAsset(std::string_view FilePath,
		std::string_view AssetPath, const FStaticMeshImportSettings& ImportSettings,
		std::string_view, bool bAllowEngineContentWrite) -> FStaticMeshImportResult
	{
		(void)bAllowEngineContentWrite;
		const std::filesystem::path Input =
			std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return {false, "Source file does not exist.", nullptr};
		std::string Error;
		if (!ImportSettings.IsValid(&Error)) return {false, std::move(Error), nullptr};
		FAssetPath ParsedPath;
		if (!FAssetPath::TryCreate(AssetPath, ParsedPath, &Error))
			return {false, std::move(Error), nullptr};
		if (Asset::FindAssetExact(ParsedPath) || Asset::FindResidentPackage(ParsedPath))
			return {false, std::format("Asset {} already exists.", ParsedPath.ToString()), nullptr};
		DStaticMesh* Mesh = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(ParsedPath, Mesh);
		if (!Created || !Mesh)
			return {false, Created.Message.empty()
				? "StaticMesh destination could not be created." : Created.Message, nullptr};
		auto Abandon = [&] {
			(void)Asset::UnloadPackage(
				ParsedPath, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
		};
		if (!RebuildFromFilename(
			*Mesh, {}, ESourceHintBase::AssetRelative,
			ImportSettings, Error, nullptr, Input))
		{
			Abandon();
			return {false, std::move(Error), nullptr};
		}
		const Asset::FAssetResult Saved = Asset::SavePackage(Mesh->GetPackage());
		if (!Saved) return {false, Saved.Message, Mesh};
		return {true, {}, Mesh};
	}
}
