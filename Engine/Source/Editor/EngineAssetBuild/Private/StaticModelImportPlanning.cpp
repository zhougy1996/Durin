#include "StaticModelImportBuild.h"

#include "AssetImport.h"
#include "AssetSystem.h"
#include "Misc/Paths.h"

namespace Durin
{
	struct FStaticModelImportPlanData
	{
		Asset::FImportedSceneData Scene;
		std::filesystem::path PhysicalRootSource;
	};

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

	auto PlanStaticModelImport(
		const FStaticModelImportPlanRequest& Request) -> FStaticModelImportPlanResult
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
		if (!CheckPlannedAssetCollision(
			Request.RootAssetPath, PlannedIdentities, Result.Message)) return Result;
		Result.Plan.Assets.push_back({
			.Kind = EStaticModelPlannedAssetKind::StaticMesh,
			.AssetPath = Request.RootAssetPath});

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
			if (!MakeChildAssetPath(
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
					if (!MakeChildAssetPath(
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
				}
				else
				{
					PlannedMaterial.TextureAssetIndex = TextureIt->second;
				}
			}
			Result.Plan.Assets.push_back(std::move(PlannedMaterial));
		}

		Result.Plan.Data = std::move(Data);
		Result.bSucceeded = true;
		return Result;
	}
}
