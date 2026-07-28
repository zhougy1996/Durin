#include "StaticModelImportBuild.h"
#include "StaticModelImportWorkflowInternal.h"

#include "AssetSystem.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		auto FoldAscii(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
			return Value;
		}

		auto SanitizeAssetName(std::string_view Value, std::string_view Fallback) -> std::string
		{
			std::string Result;
			bool bLastWasSeparator = false;
			for (const char Character : Value)
			{
				const bool bValid =
					(Character >= 'a' && Character <= 'z')
					|| (Character >= 'A' && Character <= 'Z')
					|| (Character >= '0' && Character <= '9');
				if (bValid)
				{
					Result.push_back(Character);
					bLastWasSeparator = false;
				}
				else if (!Result.empty() && !bLastWasSeparator)
				{
					Result.push_back('_');
					bLastWasSeparator = true;
				}
			}
			while (!Result.empty() && Result.back() == '_') Result.pop_back();
			return Result.empty() ? std::string(Fallback) : Result;
		}

		auto MakeUniqueAssetName(
			std::string_view Requested,
			std::string_view Fallback,
			std::unordered_set<std::string>& UsedNames) -> std::string
		{
			const std::string Base = SanitizeAssetName(Requested, Fallback);
			std::string Candidate = Base;
			for (uint32 Suffix = 2; !UsedNames.insert(FoldAscii(Candidate)).second; ++Suffix)
				Candidate = std::format("{}_{}", Base, Suffix);
			return Candidate;
		}

		auto MakeChildAssetPath(
			const FAssetPath& Root,
			std::string_view DirectorySuffix,
			std::string_view Leaf,
			FAssetPath& OutPath,
			std::string& OutError) -> bool
		{
			const std::filesystem::path RootPath(Root.ToString());
			const std::string Value = (
				RootPath.parent_path()
				/ (RootPath.filename().generic_string() + std::string(DirectorySuffix))
				/ std::string(Leaf)).generic_string();
			return FAssetPath::TryCreate(Value, OutPath, &OutError);
		}

		auto ImportAxisVector(EStaticMeshImportAxis Axis) -> FVector3f
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

		auto MakeImportOptions(
			const FStaticMeshImportSettings& Settings,
			const FSourcePath& RootSource) -> Asset::FMeshImportOptions
		{
			const FVector3f Forward = ImportAxisVector(Settings.ForwardAxis);
			const FVector3f Right = ImportAxisVector(Settings.RightAxis);
			const FVector3f Up = ImportAxisVector(Settings.UpAxis);
			Asset::FMeshImportOptions Options;
			for (uint32 SourceComponent = 0; SourceComponent < 3; ++SourceComponent)
			{
				Options.SourceToEngine[SourceComponent][0] = Forward[SourceComponent];
				Options.SourceToEngine[SourceComponent][1] = Right[SourceComponent];
				Options.SourceToEngine[SourceComponent][2] = Up[SourceComponent];
			}
			Options.RootSource = RootSource;
			return Options;
		}

		auto ResolveRootSource(
			const FStaticModelImportPlanRequest& Request,
			FSourcePath& OutRootSource,
			EStaticModelPlannedSourceAction& OutAction,
			std::string& OutError) -> bool
		{
			const PathUtilities::FSourcePathResult Classified =
				PathUtilities::ClassifySourcePath(Request.SourceFile);
			if (Classified)
			{
				OutRootSource.Path = Classified.NormalizedVirtualPath;
				OutAction = EStaticModelPlannedSourceAction::Reference;
				return true;
			}
			if (Request.RootSourceDestination.IsEmpty())
			{
				OutError =
					"An external static-model source requires an explicit mounted SourceAssets destination.";
				return false;
			}
			const PathUtilities::FSourcePathResult Destination =
				PathUtilities::ResolveSourcePath(
					Request.RootSourceDestination.Path,
					PathUtilities::EPathExistence::AllowMissing);
			if (!Destination)
			{
				OutError = Destination.Message;
				return false;
			}
			const PathUtilities::FMountPolicyResult Policy =
				PathUtilities::CheckSourceMutation(
					Request.RootAssetPath.ToString(), Destination.NormalizedVirtualPath);
			if (!Policy)
			{
				OutError = Policy.Message;
				return false;
			}
			OutRootSource.Path = Destination.NormalizedVirtualPath;
			OutAction = EStaticModelPlannedSourceAction::Ingest;
			return true;
		}

		auto CheckPlannedAssetCollision(
			const FAssetPath& Path,
			std::unordered_set<std::string>& Identities,
			std::string& OutError) -> bool
		{
			if (!Identities.insert(FoldAscii(Path.ToString())).second)
			{
				OutError = std::format(
					"Generated output {} collides case-insensitively with another planned asset.",
					Path.ToString());
				return false;
			}
			if (Asset::FindLoadedPackage(Path) || Asset::GetAssetRegistry().FindAsset(Path))
			{
				OutError = std::format(
					"Generated output {} collides with an existing asset.", Path.ToString());
				return false;
			}
			const PathUtilities::FContentPathResult Destination =
				PathUtilities::ResolveContentPath(
					Path.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!Destination)
			{
				OutError = Destination.Message;
				return false;
			}
			std::filesystem::path PackagePath = Destination.PhysicalPath;
			PackagePath += ".dasset";
			std::error_code ErrorCode;
			if (std::filesystem::exists(PackagePath, ErrorCode))
			{
				OutError = std::format(
					"Generated output {} collides with an existing package file.",
					Path.ToString());
				return false;
			}
			return true;
		}

		auto ImageExtension(Asset::EImportedImageEncoding Encoding) -> std::string_view
		{
			switch (Encoding)
			{
			case Asset::EImportedImageEncoding::Png: return ".png";
			case Asset::EImportedImageEncoding::Jpeg: return ".jpg";
			case Asset::EImportedImageEncoding::Bmp: return ".bmp";
			case Asset::EImportedImageEncoding::Tga: return ".tga";
			}
			return {};
		}
	}

	auto PlanStaticModelImportInternal(
		const FStaticModelImportPlanRequest& Request,
		DStaticMesh* ExistingMesh,
		bool bRecreateMissingAssets) -> FStaticModelImportPlanResult
	{
		FStaticModelImportPlanResult Result;
		if (!Request.RootAssetPath.IsValid())
		{
			Result.Message = "Static-model output planning requires a valid root asset path.";
			return Result;
		}
		const std::filesystem::path Input =
			std::filesystem::absolute(Request.SourceFile).lexically_normal();
		if (!std::filesystem::is_regular_file(Input))
		{
			Result.Message = std::format(
				"Static-model source file does not exist: {}", Input.generic_string());
			return Result;
		}
		if (!Request.ImportSettings.IsValid(&Result.Message)) return Result;

		EStaticModelPlannedSourceAction RootAction =
			EStaticModelPlannedSourceAction::Reference;
		if (!ResolveRootSource(
			Request, Result.Plan.RootSource, RootAction, Result.Message)) return Result;

		auto Data = std::make_shared<FStaticModelImportPlanData>();
		Data->PhysicalRootSource = Input;
		Data->ImportSettings = Request.ImportSettings;
		Data->ExistingMesh = ExistingMesh;
		if (!Asset::ImportFromFile(
			Input.generic_string(),
			Data->Scene,
			MakeImportOptions(Request.ImportSettings, Result.Plan.RootSource)))
		{
			Result.Message = std::format(
				"Failed to parse static-model source {}.", Input.generic_string());
			return Result;
		}

		Result.Plan.RootAssetPath = Request.RootAssetPath;
		Result.Plan.StandardMaterialPath = StandardImportedSurfaceMaterialPath;

		const auto RootDependency = std::ranges::find(
			Data->Scene.Dependencies,
			Asset::EImportedDependencyRole::RootModel,
			&Asset::FImportedDependency::Role);
		Result.Plan.Sources.push_back({
			.Action = RootAction,
			.SourcePath = Result.Plan.RootSource,
			.StableIdentity = RootDependency == Data->Scene.Dependencies.end()
				? std::string("root") : RootDependency->StableIdentity,
			.ByteCount = RootDependency == Data->Scene.Dependencies.end()
				? static_cast<uint64>(std::filesystem::file_size(Input))
				: RootDependency->ByteCount});
		for (const Asset::FImportedDependency& Dependency : Data->Scene.Dependencies)
		{
			if (Dependency.Role != Asset::EImportedDependencyRole::GeometryBuffer) continue;
			const PathUtilities::FSourcePathResult Existing =
				PathUtilities::ResolveSourcePath(
					Dependency.Source.Path,
					PathUtilities::EPathExistence::RequireFile);
			Result.Plan.Sources.push_back({
				.Action = Existing
					? EStaticModelPlannedSourceAction::Reference
					: EStaticModelPlannedSourceAction::Ingest,
				.SourcePath = Dependency.Source,
				.StableIdentity = Dependency.StableIdentity,
				.ByteCount = Dependency.ByteCount});
		}

		for (const Asset::FImportDiagnostic& Diagnostic : Data->Scene.Diagnostics)
		{
			if (Diagnostic.Severity == Asset::EImportDiagnosticSeverity::Warning)
				Result.Plan.Warnings.push_back(Diagnostic.Message);
		}

		std::unordered_set<std::string> PlannedIdentities;
		if (ExistingMesh)
		{
			PlannedIdentities.insert(FoldAscii(Request.RootAssetPath.ToString()));
		}
		else if (!CheckPlannedAssetCollision(
			Request.RootAssetPath, PlannedIdentities, Result.Message)) return Result;
		Result.Plan.Assets.push_back({
			.Kind = EStaticModelPlannedAssetKind::StaticMesh,
			.AssetPath = Request.RootAssetPath});

		const FStaticModelImportManifest* PreviousManifest =
			ExistingMesh ? &ExistingMesh->GetImportManifest() : nullptr;
		std::vector<bool> UsedPreviousMaterials(
			PreviousManifest ? PreviousManifest->Materials.size() : 0, false);
		std::vector<bool> UsedPreviousTextures(
			PreviousManifest ? PreviousManifest->Textures.size() : 0, false);

		auto ReserveExistingPath = [&](DObject* Object, FAssetPath& OutPath) -> bool {
			if (!Object || !Object->GetPackage()
				|| !FAssetPath::TryCreate(
					Object->GetPackage()->GetPackagePath(), OutPath, &Result.Message))
			{
				if (Result.Message.empty())
					Result.Message = "A manifest-managed generated asset has no valid package path.";
				return false;
			}
			if (!PlannedIdentities.insert(FoldAscii(OutPath.ToString())).second)
			{
				Result.Message = std::format(
					"Manifest-managed output {} is referenced more than once.", OutPath.ToString());
				return false;
			}
			return true;
		};
		auto HasActiveDifferentOwner = [&](const FAssetPath& Owner) -> bool {
			return Owner.IsValid() && Owner != Request.RootAssetPath
				&& (Asset::FindLoadedPackage(Owner)
					|| Asset::GetAssetRegistry().FindAsset(Owner));
		};

		auto FindPreviousMaterial = [&](const Asset::FImportedMaterial& Imported)
			-> std::optional<size_t> {
			if (!PreviousManifest) return std::nullopt;
			size_t NameMatch = std::numeric_limits<size_t>::max();
			uint32 NameMatches = 0;
			for (size_t Index = 0; Index < PreviousManifest->Materials.size(); ++Index)
			{
				if (!UsedPreviousMaterials[Index]
					&& PreviousManifest->Materials[Index].SourceName == Imported.SourceName)
				{
					NameMatch = Index;
					++NameMatches;
				}
			}
			if (NameMatches == 1) return NameMatch;
			for (size_t Index = 0; Index < PreviousManifest->Materials.size(); ++Index)
			{
				if (!UsedPreviousMaterials[Index]
					&& PreviousManifest->Materials[Index].SourceMaterialIndex
						== Imported.SourceMaterialIndex) return Index;
			}
			return std::nullopt;
		};

		std::unordered_set<uint32> UsedMaterialIndices;
		std::vector<uint32> OrderedMaterialIndices;
		for (const Asset::FImportedMaterialSlot& Slot : Data->Scene.MaterialSlots)
		{
			if (UsedMaterialIndices.insert(Slot.SourceMaterialIndex).second)
				OrderedMaterialIndices.push_back(Slot.SourceMaterialIndex);
		}

		std::unordered_set<std::string> UsedMaterialNames;
		std::unordered_set<std::string> UsedTextureNames;
		std::unordered_map<std::string, uint32> PlannedTextureByIdentity;
		for (const uint32 SourceMaterialIndex : OrderedMaterialIndices)
		{
			const auto MaterialIt = std::ranges::find(
				Data->Scene.Materials,
				SourceMaterialIndex,
				&Asset::FImportedMaterial::SourceMaterialIndex);
			if (MaterialIt == Data->Scene.Materials.end())
			{
				Result.Message = std::format(
					"Used source material {} has no normalized material record.",
					SourceMaterialIndex);
				return Result;
			}
			const std::string MaterialName = MakeUniqueAssetName(
				MaterialIt->SourceName, "Material", UsedMaterialNames);
			FAssetPath MaterialPath;
			DMaterialInstance* ExistingMaterial = nullptr;
			const std::optional<size_t> PreviousMaterialIndex =
				FindPreviousMaterial(*MaterialIt);
			if (PreviousMaterialIndex)
			{
				UsedPreviousMaterials[*PreviousMaterialIndex] = true;
				const FStaticModelImportMaterialRecord& Previous =
					PreviousManifest->Materials[*PreviousMaterialIndex];
				ExistingMaterial = Previous.GeneratedMaterial.Get();
				if (ExistingMaterial)
				{
					if (HasActiveDifferentOwner(ExistingMaterial->GetImportOwner()))
					{
						Result.Message = std::format(
							"Importer-managed material {} is now owned by {}; repair ownership before reimport.",
							Previous.SourceName,
							ExistingMaterial->GetImportOwner().ToString());
						return Result;
					}
					if (!ReserveExistingPath(ExistingMaterial, MaterialPath)) return Result;
				}
				else
				{
					if (!bRecreateMissingAssets)
					{
						Result.Message = std::format(
							"Importer-managed material {} is missing; explicitly enable recreation.",
							Previous.GeneratedMaterialPath.IsValid()
								? Previous.GeneratedMaterialPath.ToString()
								: Previous.SourceName);
						return Result;
					}
					MaterialPath = Previous.GeneratedMaterialPath;
					if (!MaterialPath.IsValid()
						&& !MakeChildAssetPath(
							Request.RootAssetPath,
							"_Materials",
							MaterialName,
							MaterialPath,
							Result.Message)) return Result;
					if (!CheckPlannedAssetCollision(
						MaterialPath, PlannedIdentities, Result.Message)) return Result;
				}
			}
			else if (!MakeChildAssetPath(
				Request.RootAssetPath,
				"_Materials",
				MaterialName,
				MaterialPath,
				Result.Message)
				|| !CheckPlannedAssetCollision(
					MaterialPath, PlannedIdentities, Result.Message)) return Result;

			FStaticModelPlannedAsset PlannedMaterial{
				.Kind = EStaticModelPlannedAssetKind::MaterialInstance,
				.AssetPath = MaterialPath,
				.SourceIndex = SourceMaterialIndex};

			const auto BaseColorBinding = std::ranges::find(
				MaterialIt->TextureBindings,
				Asset::EImportedTextureSemantic::BaseColor,
				&Asset::FImportedTextureBinding::Semantic);
			if (BaseColorBinding != MaterialIt->TextureBindings.end())
			{
				if (BaseColorBinding->ImageIndex >= Data->Scene.Images.size())
				{
					Result.Message = std::format(
						"Material {} references invalid image index {}.",
						SourceMaterialIndex, BaseColorBinding->ImageIndex);
					return Result;
				}
				const Asset::FImportedImage& Image =
					Data->Scene.Images[BaseColorBinding->ImageIndex];
				const std::string TextureIdentity =
					Image.StableIdentity + "|BaseColor|Color|sRGB";
				auto TextureIt = PlannedTextureByIdentity.find(TextureIdentity);
				if (TextureIt == PlannedTextureByIdentity.end())
				{
					const std::string TextureName = MakeUniqueAssetName(
						Image.SuggestedName + "_BaseColor",
						"Image_BaseColor",
						UsedTextureNames);
					FAssetPath TexturePath;
					DTexture2D* ExistingTexture = nullptr;
					std::optional<size_t> PreviousTextureIndex;
					if (PreviousManifest)
					{
						for (size_t Index = 0; Index < PreviousManifest->Textures.size(); ++Index)
						{
							const FStaticModelImportTextureRecord& Previous =
								PreviousManifest->Textures[Index];
							if (!UsedPreviousTextures[Index]
								&& Previous.StableIdentity == Image.StableIdentity
								&& Previous.Semantic == static_cast<uint8>(
									Asset::EImportedTextureSemantic::BaseColor))
							{
								PreviousTextureIndex = Index;
								break;
							}
						}
					}
					if (PreviousTextureIndex)
					{
						UsedPreviousTextures[*PreviousTextureIndex] = true;
						const FStaticModelImportTextureRecord& Previous =
							PreviousManifest->Textures[*PreviousTextureIndex];
						ExistingTexture = Previous.GeneratedTexture.Get();
						if (ExistingTexture)
						{
							if (HasActiveDifferentOwner(ExistingTexture->GetImportOwner()))
							{
								Result.Message = std::format(
									"Importer-managed texture {} is now owned by {}; repair ownership before reimport.",
									Previous.StableIdentity,
									ExistingTexture->GetImportOwner().ToString());
								return Result;
							}
							if (!ReserveExistingPath(ExistingTexture, TexturePath)) return Result;
						}
						else
						{
							if (!bRecreateMissingAssets)
							{
								Result.Message = std::format(
									"Importer-managed texture {} is missing; explicitly enable recreation.",
									Previous.GeneratedTexturePath.IsValid()
										? Previous.GeneratedTexturePath.ToString()
										: Previous.StableIdentity);
								return Result;
							}
							TexturePath = Previous.GeneratedTexturePath;
							if (!TexturePath.IsValid()
								&& !MakeChildAssetPath(
									Request.RootAssetPath,
									"_Textures",
									TextureName,
									TexturePath,
									Result.Message)) return Result;
							if (!CheckPlannedAssetCollision(
								TexturePath, PlannedIdentities, Result.Message)) return Result;
						}
					}
					else if (!MakeChildAssetPath(
						Request.RootAssetPath,
						"_Textures",
						TextureName,
						TexturePath,
						Result.Message)
						|| !CheckPlannedAssetCollision(
							TexturePath, PlannedIdentities, Result.Message)) return Result;
					const uint32 PlannedAssetIndex =
						static_cast<uint32>(Result.Plan.Assets.size());
					PlannedTextureByIdentity.emplace(TextureIdentity, PlannedAssetIndex);
					PlannedMaterial.TextureAssetIndex = PlannedAssetIndex;

					FSourcePath ImageSource;
					EStaticModelPlannedSourceAction SourceAction =
						EStaticModelPlannedSourceAction::Reference;
					if (Image.ExternalDependencyIndex)
					{
						if (*Image.ExternalDependencyIndex >= Data->Scene.Dependencies.size())
						{
							Result.Message = "An imported image has an invalid dependency index.";
							return Result;
						}
						const Asset::FImportedDependency& Dependency =
							Data->Scene.Dependencies[*Image.ExternalDependencyIndex];
						ImageSource = Dependency.Source;
						const PathUtilities::FSourcePathResult Existing =
							PathUtilities::ResolveSourcePath(
								ImageSource.Path,
								PathUtilities::EPathExistence::RequireFile);
						SourceAction = Existing
							? EStaticModelPlannedSourceAction::Reference
							: EStaticModelPlannedSourceAction::Ingest;
					}
					else
					{
						if (!BuildEmbeddedImageSourcePath(
							Result.Plan.RootSource,
							Image.StableIdentity,
							Image.SuggestedName,
							ImageExtension(Image.Encoding),
							ImageSource,
							Result.Message)) return Result;
						SourceAction = EStaticModelPlannedSourceAction::Extract;
					}
					Result.Plan.Sources.push_back({
						.Action = SourceAction,
						.SourcePath = ImageSource,
						.StableIdentity = Image.StableIdentity,
						.ByteCount = Image.EncodedByteCount});
					Result.Plan.Assets.push_back({
						.Kind = EStaticModelPlannedAssetKind::Texture2D,
						.AssetPath = TexturePath,
						.SourceIndex = BaseColorBinding->ImageIndex});
					if (ExistingTexture)
						Data->ExistingTextures.emplace(PlannedAssetIndex, ExistingTexture);
				}
				else
				{
					PlannedMaterial.TextureAssetIndex = TextureIt->second;
				}
			}
			const size_t PlannedMaterialIndex = Result.Plan.Assets.size();
			Result.Plan.Assets.push_back(std::move(PlannedMaterial));
			if (ExistingMaterial)
				Data->ExistingMaterials.emplace(PlannedMaterialIndex, ExistingMaterial);
		}

		if (PreviousManifest)
		{
			for (size_t Index = 0; Index < PreviousManifest->Materials.size(); ++Index)
			{
				if (UsedPreviousMaterials[Index]) continue;
				const FStaticModelImportMaterialRecord& Previous =
					PreviousManifest->Materials[Index];
				if (Previous.GeneratedMaterial && Previous.GeneratedMaterial->GetPackage())
				{
					FAssetPath Path;
					if (FAssetPath::TryCreate(
						Previous.GeneratedMaterial->GetPackage()->GetPackagePath(), Path))
						Data->OrphanedAssets.push_back(std::move(Path));
				}
				else if (Previous.GeneratedMaterialPath.IsValid())
					Data->OrphanedAssets.push_back(Previous.GeneratedMaterialPath);
			}
			for (size_t Index = 0; Index < PreviousManifest->Textures.size(); ++Index)
			{
				if (UsedPreviousTextures[Index]) continue;
				const FStaticModelImportTextureRecord& Previous =
					PreviousManifest->Textures[Index];
				if (Previous.GeneratedTexture && Previous.GeneratedTexture->GetPackage())
				{
					FAssetPath Path;
					if (FAssetPath::TryCreate(
						Previous.GeneratedTexture->GetPackage()->GetPackagePath(), Path))
						Data->OrphanedAssets.push_back(std::move(Path));
				}
				else if (Previous.GeneratedTexturePath.IsValid())
					Data->OrphanedAssets.push_back(Previous.GeneratedTexturePath);
			}
		}

		Result.Plan.Data = std::move(Data);
		Result.bSucceeded = true;
		return Result;
	}

	auto PlanStaticModelImport(
		const FStaticModelImportPlanRequest& Request) -> FStaticModelImportPlanResult
	{
		return PlanStaticModelImportInternal(Request, nullptr, false);
	}

	auto PlanStaticModelReimport(
		const FStaticModelReimportPlanRequest& Request) -> FStaticModelImportPlanResult
	{
		FStaticModelImportPlanResult Result;
		if (!Request.StaticMesh || !Request.StaticMesh->GetPackage())
		{
			Result.Message = "Static-model reimport requires a packaged root mesh.";
			return Result;
		}
		const FStaticModelImportManifest& Manifest =
			Request.StaticMesh->GetImportManifest();
		if (!Manifest.IsValid())
		{
			Result.Message = "Static-model reimport requires a supported import manifest.";
			return Result;
		}
		FAssetPath RootPath;
		if (!FAssetPath::TryCreate(
			Request.StaticMesh->GetPackage()->GetPackagePath(), RootPath, &Result.Message))
			return Result;
		const PathUtilities::FSourcePathResult Source =
			PathUtilities::ResolveSourcePath(
				Request.StaticMesh->GetSourceImportData().SourcePath.Path,
				PathUtilities::EPathExistence::RequireFile);
		if (!Source)
		{
			Result.Message = std::format(
				"Static-model root source requires repair before reimport: {}",
				Source.Message);
			return Result;
		}
		return PlanStaticModelImportInternal(
			{
				.SourceFile = Source.PhysicalPath,
				.RootAssetPath = std::move(RootPath),
				.RootSourceDestination =
					Request.StaticMesh->GetSourceImportData().SourcePath,
				.ImportSettings = Request.StaticMesh->GetImportSettings()},
			Request.StaticMesh,
			Request.bRecreateMissingAssets);
	}
}
