#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"

#include "Asset/PackageSerialization.h"
#include "Asset/SourceHint.h"
#include "Asset/Asset.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "EncodedSourceSnapshot.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/StringHelper.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMeshBuild.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		constexpr uint64 MaximumStaticMeshEncodedBytes = 512ull * 1024ull * 1024ull;

		auto ResolveOwningPackagePhysicalPath(const DStaticMesh& Mesh,
			std::filesystem::path& OutPath, std::string& OutError) -> bool
		{
			if (!Mesh.GetPackage()) return false;
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(Mesh.GetPackage()->GetPackagePath(),
					EMountPathExistence::AllowMissing);
			if (!Resolved) { OutError = Resolved.Message; return false; }
			OutPath = Resolved.PhysicalPath;
			OutPath += ".dasset";
			return true;
		}

		auto IsSupportedExtension(std::string_view Extension) -> bool
		{
			const std::string Lower = StringUtils::FoldAscii(Extension);
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
			State.ImportSettings = Settings;
			OutData = dynamic_cast<DStaticMeshImportData*>(Mesh.GetAssetImportData());
			if (!OutData) OutData = NewObject<DStaticMeshImportData>(
				&Mesh, "AssetImportData");
			return OutData && OutData->SetState(std::move(State), OutError);
		}

		auto RebuildFromFilename(DStaticMesh& Mesh, std::string Filename,
			ESourceHintBase HintBase,
			const FStaticMeshImportSettings& Settings, std::string& OutError,
			const FAssetBundleSaveOptions* SaveOptions,
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
			FStaticMeshBuildResult Product;
			if (!BuildStaticMeshDerivedData({
				.Reconciliation = CaptureStaticMeshReconciliation(Mesh),
				.ImportedData = MakeStaticMeshImportedData(Scene)}, Product, OutError))
				return false;
			DStaticMeshImportData* ImportData = nullptr;
			if (!PrepareImportData(Mesh, Filename, HintBase, PhysicalPath, Snapshot,
				Settings, ImportData, OutError)
				|| !ApplyStaticMeshBuildResult(
					Mesh, std::move(Product), OutError)
				|| !Mesh.PublishAssetImportData(*ImportData, OutError)) return false;
			if (!SaveOptions) return true;
			DPackage* Package = Mesh.GetPackage();
			const FAssetResult Saved = SavePackagesAtomically(
				std::span<DPackage* const>(&Package, 1), *SaveOptions);
			if (Saved) return true;
			OutError = Saved.Message;
			return false;
		}
	}

	DStaticMeshFactory::DStaticMeshFactory(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SupportedClass = DStaticMesh::StaticClass();
		Formats = {"obj", "fbx", "gltf", "glb", "dae", "3ds", "ply", "stl"};
	}

	auto DStaticMeshFactory::FactoryCreateFromFile(
		DClass* InClass,
		DObject* InParent,
		FName InName,
		EObjectFlags Flags,
		std::string_view Filename,
		DObject*,
		FFactoryDiagnostics* Diagnostics) const -> DObject*
	{
		auto Failed = [&](std::string Message) -> DObject* {
			if (Diagnostics) Diagnostics->Report(Message);
			return nullptr;
		};
		if (InClass != DStaticMesh::StaticClass())
			return Failed("Static mesh factory requires the exact StaticMesh class.");
		auto* Package = Cast<DPackage>(InParent);
		if (!Package || !Package->IsAssetPackage())
			return Failed("Static mesh factory requires an asset package parent.");
		const std::filesystem::path Input =
			std::filesystem::absolute(Filename).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
			return Failed("Static mesh source file does not exist.");
		if (!IsSupportedExtension(Input.extension().generic_string()))
			return Failed("Static mesh source uses an unsupported format.");
		std::string Error;
		if (!Settings.IsValid(&Error)) return Failed(std::move(Error));
		auto* Mesh = NewObject<DStaticMesh>(InClass, Package, InName, Flags);
		if (!Mesh) return Failed("Static mesh object could not be created.");
		if (!RebuildFromFilename(
			*Mesh, Input.generic_string(), ESourceHintBase::AssetRelative,
			Settings, Error, nullptr, Input)) return Failed(std::move(Error));
		return Mesh;
	}

	auto DStaticMeshFactory::GetReimportCapabilities(
		const DObject& Object) const -> FReimportCapabilities
	{
		const auto* Mesh = Cast<DStaticMesh>(&Object);
		const auto* Data = Mesh ? dynamic_cast<const DStaticMeshImportData*>(
			Mesh->GetAssetImportData()) : nullptr;
		if (!Mesh || !Mesh->GetPackage() || !Data)
			return {.Diagnostic = "StaticMesh has no current family import data."};
		const FStaticMeshImportDataState State = Data->GetStaticMeshState();
		const FSourceFile* Source = State.SourceData.FindByRole("source");
		const bool bHasSource = Source && !Source->Hint.empty();
		return {.bCanReimport = bHasSource, .bCanReimportFromFile = true,
			.Diagnostic = bHasSource ? std::string{}
				: "StaticMesh has no source hint to reimport."};
	}

	auto DStaticMeshFactory::Reimport(
		DObject& Object, FReimportCompletion Completion) const -> void
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		const auto* Data = Mesh ? dynamic_cast<const DStaticMeshImportData*>(
			Mesh->GetAssetImportData()) : nullptr;
		const FStaticMeshImportDataState State = Data
			? Data->GetStaticMeshState() : FStaticMeshImportDataState{};
		const FSourceFile* Source = Data ? State.SourceData.FindByRole("source") : nullptr;
		if (!Mesh || !Data || !Source || Source->Hint.empty())
		{
			if (Completion) Completion({EReimportStatus::MissingSource,
				"StaticMesh has no source hint to reimport."});
			return;
		}
		std::string Error;
		const bool bSucceeded = RebuildFromFilename(*Mesh, Source->Hint,
			Source->HintBase, State.ImportSettings, Error, nullptr);
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto DStaticMeshFactory::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, FReimportCompletion Completion) const
		-> void
	{
		auto* Mesh = Cast<DStaticMesh>(&Object);
		const auto* Data = Mesh ? dynamic_cast<const DStaticMeshImportData*>(
			Mesh->GetAssetImportData()) : nullptr;
		if (!Mesh || !Data || Filenames.size() != 1 || Filenames.front().empty())
		{
			if (Completion) Completion({EReimportStatus::SourceOrBuildFailure,
				"StaticMesh reimport requires one source file and current import data."});
			return;
		}
		const std::filesystem::path Requested =
			std::filesystem::absolute(Filenames.front()).lexically_normal();
		std::string Error;
		const bool bSucceeded = RebuildFromFilename(*Mesh, {}, ESourceHintBase::Absolute,
			Data->GetStaticMeshState().ImportSettings, Error, nullptr, Requested);
		if (Completion) Completion(bSucceeded
			? FReimportResult{EReimportStatus::Succeeded, {}}
			: FReimportResult{EReimportStatus::SourceOrBuildFailure, std::move(Error)});
	}

	auto ReimportStaticMesh(DStaticMesh& Mesh, std::string& OutError,
		const FAssetBundleSaveOptions& SaveOptions) -> bool
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
		const FAssetBundleSaveOptions& SaveOptions) -> bool
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

}
