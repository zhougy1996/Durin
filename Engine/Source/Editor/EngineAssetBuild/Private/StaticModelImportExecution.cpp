#include "StaticModelImportBuild.h"
#include "StaticModelImportBuildInternal.h"
#include "StaticModelImportWorkflowInternal.h"

#include "AssetSystem.h"
#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialTypes.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin
{
	auto FStaticModelImportExecutionTestAccess::SetFailurePoint(
		FStaticModelImportPlan& Plan,
		EImportTransactionFailurePoint Point,
		size_t Occurrence) -> void
	{
		check(Plan.Data);
		auto* Data = const_cast<FStaticModelImportPlanData*>(Plan.Data.get());
		Data->FailurePoint = static_cast<uint8>(Point);
		Data->FailureOccurrence = Occurrence;
	}

	namespace
	{
		auto ResolvePhysicalDependency(
			const FStaticModelImportPlan& Plan,
			const FStaticModelImportPlanData& Data,
			const Asset::FImportedDependency& Dependency,
			std::filesystem::path& OutPath,
			std::string& OutError) -> bool
		{
			const PathUtilities::FSourcePathResult Existing =
				PathUtilities::ResolveSourcePath(
					Dependency.Source.Path,
					PathUtilities::EPathExistence::RequireFile);
			if (Existing)
			{
				OutPath = Existing.PhysicalPath;
				return true;
			}

			const std::filesystem::path VirtualRoot(Plan.RootSource.Path);
			const std::filesystem::path VirtualDependency(Dependency.Source.Path);
			const std::filesystem::path Relative =
				VirtualDependency.lexically_relative(VirtualRoot.parent_path());
			if (Relative.empty() || Relative.is_absolute()
				|| std::ranges::any_of(Relative, [](const auto& Component) {
					return Component == "..";
				}))
			{
				OutError = std::format(
					"Dependency {} is not relative to root source {}.",
					Dependency.Source.Path,
					Plan.RootSource.Path);
				return false;
			}
			OutPath = (Data.PhysicalRootSource.parent_path() / Relative).lexically_normal();
			if (!std::filesystem::is_regular_file(OutPath))
			{
				OutError = std::format(
					"Physical dependency is unavailable: {}.", OutPath.generic_string());
				return false;
			}
			return true;
		}

		auto FindPlannedSource(
			const FStaticModelImportPlan& Plan,
			std::string_view StableIdentity) -> const FStaticModelPlannedSource*
		{
			const auto It = std::ranges::find(
				Plan.Sources,
				StableIdentity,
				&FStaticModelPlannedSource::StableIdentity);
			return It == Plan.Sources.end() ? nullptr : &*It;
		}

		auto DiscardCreatedPackages(std::span<DPackage* const> Packages) -> void
		{
			for (auto It = Packages.rbegin(); It != Packages.rend(); ++It)
			{
				const Asset::FAssetResult Result = Asset::DiscardUnpublishedPackage(*It);
				if (!Result)
				{
					DURIN_ERROR(
						"Failed to discard unpublished static-model package: {}",
						Result.Message);
				}
			}
		}

		auto BuildDependencyFingerprint(
			std::span<const Asset::FImportedDependency> Dependencies) -> std::string
		{
			FXxHash128Builder Builder;
			Builder.Update("StaticModelImportManifestV1");
			for (const Asset::FImportedDependency& Dependency : Dependencies)
			{
				Builder.UpdateValue(Dependency.Role);
				Builder.UpdateValue(Dependency.StableIdentity.size());
				Builder.Update(Dependency.StableIdentity);
				Builder.UpdateValue(Dependency.ContentHash.HashLow);
				Builder.UpdateValue(Dependency.ContentHash.HashHigh);
				Builder.UpdateValue(Dependency.ByteCount);
			}
			return Builder.Finalize().ToString();
		}
	}

	auto ExecuteStaticModelImport(
		const FStaticModelImportPlan& Plan) -> FStaticModelImportExecutionResult
	{
		FStaticModelImportExecutionResult Result;
		if (!Plan.Data || Plan.Assets.empty()
			|| Plan.Assets.front().Kind != EStaticModelPlannedAssetKind::StaticMesh
			|| Plan.Assets.front().AssetPath != Plan.RootAssetPath)
		{
			Result.Message = "Static-model execution requires a valid immutable output plan.";
			return Result;
		}
		const FStaticModelImportPlanData& Data = *Plan.Data;

		std::string Error;
		DMaterial* StandardMaterial = EnsureStandardImportedSurfaceMaterial(Error);
		if (!StandardMaterial)
		{
			Result.Message = std::move(Error);
			return Result;
		}

		std::vector<DPackage*> CreatedPackages;
		DStaticMesh* Mesh = Data.ExistingMesh;
		if (!Mesh)
		{
			const Asset::FAssetResult MeshCreate =
				Asset::CreateAsset(Plan.RootAssetPath, Mesh);
			if (!MeshCreate)
			{
				Result.Message = MeshCreate.Message;
				return Result;
			}
			CreatedPackages.push_back(Mesh->GetPackage());
		}

		std::unordered_map<uint32, DMaterialInstance*> MaterialsBySourceIndex;
		std::unordered_map<size_t, DMaterialInstance*> MaterialsByPlannedIndex;
		for (size_t AssetIndex = 0; AssetIndex < Plan.Assets.size(); ++AssetIndex)
		{
			const FStaticModelPlannedAsset& Planned = Plan.Assets[AssetIndex];
			if (Planned.Kind != EStaticModelPlannedAssetKind::MaterialInstance) continue;
			DMaterialInstance* Instance = nullptr;
			if (const auto Existing = Data.ExistingMaterials.find(AssetIndex);
				Existing != Data.ExistingMaterials.end())
			{
				Instance = Existing->second;
			}
			else
			{
				const Asset::FAssetResult CreateResult =
					Asset::CreateAsset(Planned.AssetPath, Instance);
				if (!CreateResult)
				{
					Result.Message = CreateResult.Message;
					DiscardCreatedPackages(CreatedPackages);
					return Result;
				}
				CreatedPackages.push_back(Instance->GetPackage());
			}
			MaterialsBySourceIndex.emplace(Planned.SourceIndex, Instance);
			MaterialsByPlannedIndex.emplace(AssetIndex, Instance);
			Result.Materials.push_back(Instance);
		}

		FMultiAssetImportTransaction Transaction;
		for (DMaterialInstance* Material : Result.Materials)
			Transaction.AddPackage(Material->GetPackage());
		for (const auto& [AssetIndex, Texture] : Data.ExistingTextures)
			Transaction.AddPackage(Texture->GetPackage());
		Transaction.AddPackage(Mesh->GetPackage(), true);
		if (Data.FailurePoint != 0)
		{
			FMultiAssetImportTransactionTestAccess::SetFailurePoint(
				Transaction,
				static_cast<EImportTransactionFailurePoint>(Data.FailurePoint),
				Data.FailureOccurrence);
		}

		const auto RootDependency = std::ranges::find(
			Data.Scene.Dependencies,
			Asset::EImportedDependencyRole::RootModel,
			&Asset::FImportedDependency::Role);
		if (RootDependency == Data.Scene.Dependencies.end())
		{
			Result.Message = "Normalized static-model data has no root dependency.";
			DiscardCreatedPackages(CreatedPackages);
			return Result;
		}
		Transaction.AddSource({
			.AuthoringAssetPath = Plan.RootAssetPath,
			.ExternalSource = Data.PhysicalRootSource,
			.SourceDestination = Plan.RootSource});
		for (const Asset::FImportedDependency& Dependency : Data.Scene.Dependencies)
		{
			if (Dependency.Role != Asset::EImportedDependencyRole::GeometryBuffer) continue;
			std::filesystem::path Physical;
			if (!ResolvePhysicalDependency(Plan, Data, Dependency, Physical, Error))
			{
				Result.Message = std::move(Error);
				DiscardCreatedPackages(CreatedPackages);
				return Result;
			}
			Transaction.AddSource({
				.AuthoringAssetPath = Plan.RootAssetPath,
				.ExternalSource = std::move(Physical),
				.SourceDestination = Dependency.Source});
		}

		std::unordered_map<size_t, size_t> TextureRequestByPlannedIndex;
		for (size_t AssetIndex = 0; AssetIndex < Plan.Assets.size(); ++AssetIndex)
		{
			const FStaticModelPlannedAsset& Planned = Plan.Assets[AssetIndex];
			if (Planned.Kind != EStaticModelPlannedAssetKind::Texture2D) continue;
			if (Planned.SourceIndex >= Data.Scene.Images.size())
			{
				Result.Message = "A planned texture references an invalid imported image.";
				DiscardCreatedPackages(CreatedPackages);
				return Result;
			}
			const Asset::FImportedImage& Image = Data.Scene.Images[Planned.SourceIndex];
			const FStaticModelPlannedSource* PlannedSource =
				FindPlannedSource(Plan, Image.StableIdentity);
			if (!PlannedSource)
			{
				Result.Message = std::format(
					"Planned image source is missing for {}.", Image.StableIdentity);
				DiscardCreatedPackages(CreatedPackages);
				return Result;
			}
			FPortableTextureBuildRequest TextureRequest{
				.AssetPath = Planned.AssetPath,
				.SourceDestination = PlannedSource->SourcePath,
				.Settings = {
					.Usage = ETextureUsage::Color,
					.bSRGB = true},
				.ImportOwner = Plan.RootAssetPath,
				.bAllowSourceReplacement = Data.ExistingMesh != nullptr};
			if (const auto Existing = Data.ExistingTextures.find(AssetIndex);
				Existing != Data.ExistingTextures.end())
				TextureRequest.ExistingTexture = Existing->second;
			if (Image.ExternalDependencyIndex)
			{
				if (*Image.ExternalDependencyIndex >= Data.Scene.Dependencies.size())
				{
					Result.Message = "A planned external image has an invalid dependency.";
					DiscardCreatedPackages(CreatedPackages);
					return Result;
				}
				if (!ResolvePhysicalDependency(
					Plan,
					Data,
					Data.Scene.Dependencies[*Image.ExternalDependencyIndex],
					TextureRequest.ExternalSource,
					Error))
				{
					Result.Message = std::move(Error);
					DiscardCreatedPackages(CreatedPackages);
					return Result;
				}
			}
			else
			{
				TextureRequest.EncodedBytes = Image.EmbeddedEncodedBytes;
			}
			TextureRequestByPlannedIndex.emplace(
				AssetIndex, TextureRequestByPlannedIndex.size());
			Transaction.AddTexture(std::move(TextureRequest));
		}

		const FStaticMeshSourceImportData SourceImportData{
			.SourcePath = Plan.RootSource,
			.SourceContentHash = RootDependency->ContentHash.ToString(),
			.ImporterId = "AssetImport.StaticModel",
			.ImporterVersion = Asset::StaticModelImporterVersion,
			.ImportSettings = Data.ImportSettings};
		const std::string MeshDerivedDataKey = BuildStaticMeshDerivedDataKey({
			.SourceContentHash = RootDependency->ContentHash,
			.ImporterId = SourceImportData.ImporterId,
			.ImporterVersion = SourceImportData.ImporterVersion,
			.ImportSettings = SourceImportData.ImportSettings,
			.TargetPlatform = EStaticMeshTargetPlatform::Win64});
		Asset::FDerivedDataObjectStore MeshStore(
			"StaticMesh/Objects", MaximumStaticMeshPayloadBytes);
		std::filesystem::path MeshDerivedDataPath;
		if (!MeshStore.GetObjectPath(
			MeshDerivedDataKey, MeshDerivedDataPath, &Error))
		{
			Result.Message = std::move(Error);
			DiscardCreatedPackages(CreatedPackages);
			return Result;
		}
		std::error_code DerivedDataError;
		const bool bMeshDerivedDataExisted =
			std::filesystem::is_regular_file(
				MeshDerivedDataPath, DerivedDataError);
		if (DerivedDataError == std::errc::no_such_file_or_directory)
			DerivedDataError.clear();
		if (DerivedDataError)
		{
			Result.Message = std::format(
				"Failed to inspect static-mesh derived data {}: {}",
				MeshDerivedDataPath.generic_string(),
				DerivedDataError.message());
			DiscardCreatedPackages(CreatedPackages);
			return Result;
		}
		DStaticMesh* MeshCandidate =
			NewObject<DStaticMesh>(nullptr, "StaticModelImportCandidate");
		if (Data.ExistingMesh)
			MeshCandidate->SeedMaterialReconciliationFrom(*Data.ExistingMesh);
		std::unordered_map<size_t, DMaterialInstance*> MaterialCandidatesByPlannedIndex;
		for (const auto& [AssetIndex, Instance] : MaterialsByPlannedIndex)
		{
			(void)Instance;
			MaterialCandidatesByPlannedIndex.emplace(
				AssetIndex,
				NewObject<DMaterialInstance>(
					nullptr, std::format("StaticModelMaterialCandidate_{}", AssetIndex)));
		}
		bool bCandidateStatePublished = false;
		Transaction.AddLoadedObjectMutation(
			[&](std::string& MutationError) {
				if (!MeshCandidate)
				{
					MutationError = "Static-model candidate graph is unavailable.";
					return false;
				}
				for (const auto& [AssetIndex, Instance] : MaterialsByPlannedIndex)
					Instance->ExchangeImportedState(
						*MaterialCandidatesByPlannedIndex.at(AssetIndex));
				if (!Mesh->ExchangeImportedState(
					*MeshCandidate, MutationError))
				{
					for (const auto& [AssetIndex, Instance]
						: MaterialsByPlannedIndex)
					{
						Instance->ExchangeImportedState(
							*MaterialCandidatesByPlannedIndex.at(
								AssetIndex));
					}
					return false;
				}
				bCandidateStatePublished = true;
				return true;
			},
			[&] {
				if (!bCandidateStatePublished) return;
				std::string RollbackError;
				const bool bMeshRolledBack =
					Mesh->ExchangeImportedState(
						*MeshCandidate, RollbackError);
				check(bMeshRolledBack);
				for (const auto& [AssetIndex, Instance] : MaterialsByPlannedIndex)
					Instance->ExchangeImportedState(
						*MaterialCandidatesByPlannedIndex.at(AssetIndex));
				bCandidateStatePublished = false;
			});

		auto DiscardCandidates = [&] {
			if (MeshCandidate) MarkAsGarbage(MeshCandidate);
			for (const auto& [AssetIndex, Candidate] : MaterialCandidatesByPlannedIndex)
			{
				(void)AssetIndex;
				MarkAsGarbage(Candidate);
			}
		};
		auto FailPrepared = [&](std::string Message) -> FStaticModelImportExecutionResult {
			Transaction.Rollback();
			if (!bMeshDerivedDataExisted)
			{
				std::error_code ErrorCode;
				std::filesystem::remove(MeshDerivedDataPath, ErrorCode);
			}
			DiscardCandidates();
			DiscardCreatedPackages(CreatedPackages);
			Result.Message = std::move(Message);
			Result.Materials.clear();
			return Result;
		};

		if (!Transaction.Prepare(Error))
			return FailPrepared(std::move(Error));

		if (!MeshCandidate->InitializeFromImportedScene(
			Data.Scene,
			SourceImportData,
			Data.PhysicalRootSource.generic_string(),
			Error)) return FailPrepared(std::move(Error));

		const std::span<DTexture2D* const> Textures = Transaction.GetTextures();
		for (size_t AssetIndex = 0; AssetIndex < Plan.Assets.size(); ++AssetIndex)
		{
			const FStaticModelPlannedAsset& Planned = Plan.Assets[AssetIndex];
			if (Planned.Kind != EStaticModelPlannedAssetKind::MaterialInstance) continue;
			DMaterialInstance* Candidate =
				MaterialCandidatesByPlannedIndex.at(AssetIndex);
			Candidate->SetImportOwner(Plan.RootAssetPath);
			const auto MaterialIt = std::ranges::find(
				Data.Scene.Materials,
				Planned.SourceIndex,
				&Asset::FImportedMaterial::SourceMaterialIndex);
			if (MaterialIt == Data.Scene.Materials.end()
				|| !Candidate->SetParent(StandardMaterial)
				|| !Candidate->SetVectorParameterValue(
					MaterialParameters::BaseColorName(),
					FVector3(MaterialIt->BaseColorFactor))
				|| !Candidate->SetScalarParameterValue(
					MaterialParameters::OpacityName(),
					MaterialIt->BaseColorFactor.a))
			{
				return FailPrepared(std::format(
					"Failed to map imported material {}.", Planned.SourceIndex));
			}
			if (Planned.TextureAssetIndex)
			{
				const auto RequestIt =
					TextureRequestByPlannedIndex.find(*Planned.TextureAssetIndex);
				if (RequestIt == TextureRequestByPlannedIndex.end()
					|| RequestIt->second >= Textures.size()
					|| !Candidate->SetTextureParameterValue(
						MaterialParameters::BaseColorTextureName(),
						Textures[RequestIt->second]))
				{
					return FailPrepared(std::format(
						"Failed to map base-color texture for material {}.",
						Planned.SourceIndex));
				}
			}
			if (!MeshCandidate->SetImportedDefaultMaterial(
				Planned.SourceIndex,
				MaterialsByPlannedIndex.at(AssetIndex),
				Error)) return FailPrepared(std::move(Error));
		}

		FStaticModelImportManifest Manifest{
			.Version = StaticModelImportManifestVersion,
			.DependencyFingerprint =
				BuildDependencyFingerprint(Data.Scene.Dependencies),
			.ImporterVersion = Asset::StaticModelImporterVersion,
			.MaterialMapperVersion = StaticModelMaterialMapperVersion,
			.Warnings = Plan.Warnings};
		Manifest.Dependencies.reserve(Data.Scene.Dependencies.size());
		for (const Asset::FImportedDependency& Dependency : Data.Scene.Dependencies)
		{
			Manifest.Dependencies.push_back({
				.Role = static_cast<uint8>(Dependency.Role),
				.StableIdentity = Dependency.StableIdentity,
				.SourcePath = Dependency.Source,
				.ContentHash = Dependency.ContentHash.ToString(),
				.ByteCount = Dependency.ByteCount});
		}
		Manifest.Materials.reserve(MaterialsByPlannedIndex.size());
		for (size_t AssetIndex = 0; AssetIndex < Plan.Assets.size(); ++AssetIndex)
		{
			const FStaticModelPlannedAsset& Planned = Plan.Assets[AssetIndex];
			if (Planned.Kind != EStaticModelPlannedAssetKind::MaterialInstance) continue;
			DMaterialInstance* Instance = MaterialsByPlannedIndex.at(AssetIndex);
			const auto Imported = std::ranges::find(
				Data.Scene.Materials,
				Planned.SourceIndex,
				&Asset::FImportedMaterial::SourceMaterialIndex);
			const std::span ImportedSlots = MeshCandidate->GetMaterialSlots();
			const auto ImportedSlot = std::ranges::find(
				ImportedSlots,
				Planned.SourceIndex,
				&FStaticMeshMaterialSlotDefinition::SourceMaterialIndex);
			if (Imported == Data.Scene.Materials.end()
				|| ImportedSlot == ImportedSlots.end())
				return FailPrepared(
					"Failed to correlate imported material manifest records.");
			Manifest.Materials.push_back({
				.SlotId = ImportedSlot->SlotId,
				.SourceMaterialIndex = Planned.SourceIndex,
				.SourceName = Imported->SourceName,
				.BaseColorFactor = FVector4(Imported->BaseColorFactor),
				.GeneratedMaterialPath = Planned.AssetPath,
				.bImporterManaged = true,
				.GeneratedMaterial = Instance});
		}
		for (size_t AssetIndex = 0; AssetIndex < Plan.Assets.size(); ++AssetIndex)
		{
			const FStaticModelPlannedAsset& Planned = Plan.Assets[AssetIndex];
			if (Planned.Kind != EStaticModelPlannedAssetKind::Texture2D) continue;
			const size_t RequestIndex =
				TextureRequestByPlannedIndex.at(AssetIndex);
			if (RequestIndex >= Textures.size()
				|| Planned.SourceIndex >= Data.Scene.Images.size())
				return FailPrepared(
					"Failed to correlate imported texture manifest records.");
			Manifest.Textures.push_back({
				.StableIdentity =
					Data.Scene.Images[Planned.SourceIndex].StableIdentity,
				.Semantic = static_cast<uint8>(
					Asset::EImportedTextureSemantic::BaseColor),
				.GeneratedTexturePath = Planned.AssetPath,
				.GeneratedTexture = Textures[RequestIndex]});
		}
		if (!MeshCandidate->SetImportManifest(std::move(Manifest), Error))
			return FailPrepared(std::move(Error));

		if (!Transaction.Stage(Error) || !Transaction.Publish(Error))
			return FailPrepared(std::move(Error));
		DiscardCandidates();
		Result.StaticMesh = Mesh;
		Result.Textures.assign(Textures.begin(), Textures.end());
		Result.OrphanedAssets = Data.OrphanedAssets;
		Result.bSucceeded = true;
		return Result;
	}
}
