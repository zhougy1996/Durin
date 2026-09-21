#include "SceneImportInternal.h"
#include "AssetForge/Builtins/PBRSurfaceMaterial.h"
#include "Asset/Asset.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Materials/MaterialInstance.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		auto CheckParent(DMaterial& Parent, FMaterialExpressionRecipe& Graph) -> std::string
		{
			if (Parent.GetDomain() != EMaterialDomain::Surface)
				return "The selected parent must be a Surface material.";
			// Compare actual wiring, including UVs, shared samples and channel selection.
			// Defaults and presentation metadata may differ; instance overrides replace values.
			for (auto& Expression : Graph.Expressions)
				if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
				{
					const auto Expected = Parameter->GetParameterDefinition();
					const auto* Definition = Parent.FindParameterDefinition(Expected.Id);
					if (!Definition || Definition->Type != Expected.Type ||
						(Expected.Type == EMaterialParameterType::Texture && Definition->TextureUsage != Expected.TextureUsage))
						return "Parent is missing a compatible PBR parameter: " + Expected.DisplayName;
					if (!Parameter->SetParameterDefinition(*Definition))
						return "Parent has an incompatible PBR parameter: " + Expected.DisplayName;
				}
			if (!Graph.MatchesGraph(Parent))
				return "Parent sampling, channels or UV graph does not match this source. Select a material created from a compatible source, or choose Create Materials.";
			return {};
		}
	}

	auto ConfigureSceneMaterials(FSceneImportPlan& Plan,
		std::vector<FImportOutputSummary>& Outputs, std::string_view Source,
		const FPackagePath& Destination, const FSceneMaterialImportOptions& Options,
		std::vector<FSceneMaterialPreview>& Preview, std::string& Error) -> bool
	{
		std::unordered_map<std::string, DMaterialInterface*> Existing;
		std::vector<FPackagePath> Paths;
		const auto Prefix = Destination.ToString() + "/";
		for (const auto& [Path, Entry] : CaptureAssetCatalogSnapshot().Assets)
			if (Path.GetView().starts_with(Prefix)) Paths.push_back(Path);
		for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
			if (auto* Package = Cast<DPackage>(Object); Package && Package->GetPackagePath().starts_with(Prefix)
				&& std::ranges::find(Paths, Package->GetPackagePathIdentity()) == Paths.end())
				Paths.push_back(Package->GetPackagePathIdentity());
		for (const auto& Path : Paths)
		{
			FObjectPath ObjectPath;
			if (!FObjectPath::TryCreate(Path.ToString() + "." + std::string(Path.GetPackageName()), ObjectPath)) continue;
			auto* Material = LoadObject<DMaterialInterface>(ObjectPath).value_or(nullptr);
			if (!Material) continue;
			const auto& Receipt = Material->GetImportProvenance();
			if (Receipt.RecipeId != "Durin.ImportedSurface" || Receipt.RecipeVersion != 1 || Receipt.SourceIdentity != Source) continue;
			if (!Existing.emplace(Receipt.OutputIdentity, Material).second)
			{
				Error = "Multiple materials claim the same scene source/output identity.";
				return false;
			}
		}
		std::unordered_set<std::string> OverrideIds;
		for (const auto& Override : Options.Overrides)
			if (!OverrideIds.insert(Override.StableIdentity).second ||
				std::ranges::none_of(Plan.Outputs, [&](const auto& Output) {
					return IsSceneMaterial(Output.Kind) && Output.StableIdentity == Override.StableIdentity;
				}))
			{
				Error = "Material override has a duplicate or unknown source identity.";
				return false;
			}
		bool bValid = true;
		for (auto& Output : Plan.Outputs)
		{
			if (!IsSceneMaterial(Output.Kind)) continue;
			auto& Row = Preview.emplace_back();
			Row.StableIdentity = Output.StableIdentity;
			Row.SourceName = std::ranges::find(Plan.Scene.Materials, Output.SourceIndex,
				&FImportedMaterial::SourceMaterialIndex)->SourceName;
			Row.Selection = Options.Default;
			if (const auto Override = std::ranges::find(Options.Overrides, Output.StableIdentity,
				&FSceneMaterialOverride::StableIdentity); Override != Options.Overrides.end()) Row.Selection = Override->Selection;
			if (Row.Selection.Mode != ESceneMaterialImportMode::CreateMaterials &&
				Row.Selection.Mode != ESceneMaterialImportMode::CreateInstances)
			{
				Error = "Unknown scene material import mode.";
				return false;
			}
			auto Summary = std::ranges::find(Outputs, Output.StableIdentity, &FImportOutputSummary::StableIdentity);
			const auto Previous = Existing.find(Output.StableIdentity);
			if (Previous != Existing.end() && !Options.bRebuildExistingMaterials)
			{
				Output.PreservedMaterial = TStrongObjectPtr<DMaterialInterface>(Previous->second);
				Row.bPreserved = true;
				Row.Selection.Mode = Cast<DMaterial>(Previous->second)
					? ESceneMaterialImportMode::CreateMaterials : ESceneMaterialImportMode::CreateInstances;
				Row.Selection.ParentMaterialPath = Previous->second->GetParent()
					? Previous->second->GetParent()->GetObjectPath() : std::string{};
				Row.Message = "Preserve existing material, parent and edits";
			}
			const bool bInstance = Row.Selection.Mode == ESceneMaterialImportMode::CreateInstances;
			Output.Kind = bInstance ? ESceneOutputKind::MaterialInstance : ESceneOutputKind::Material;
			Summary->Role = bInstance ? "MaterialInstance" : "Material";
			Summary->AssetClassName = bInstance ? "Durin::DMaterialInstance" : "Durin::DMaterial";
			if (Previous != Existing.end())
			{
				Summary->AssetPath = Previous->second->GetPackage()->GetPackagePathIdentity();
				if (bInstance != (Cast<DMaterialInstance>(Previous->second) != nullptr))
					Row.Message = "Changing an existing material's asset type requires a separate destination.";
			}
			else
			{
				if (!FPackagePath::TryCreate(Destination.ToString() + "/Materials/" + (bInstance ? "MI_" : "M_")
					+ std::string(Summary->AssetPath.GetPackageName()), Summary->AssetPath))
					Row.Message = "The generated material path is invalid.";
				else if (FindAssetExact(Summary->AssetPath) || FindResidentPackage(Summary->AssetPath))
					Row.Message = "The material output path is occupied by an unrelated asset.";
			}
			Row.AssetPath = Summary->AssetPath;
			Row.bCompatible = Row.bPreserved || Row.Message.empty();
			if (bInstance && !Row.bPreserved && Row.bCompatible)
			{
				FObjectPath ParentPath;
				if (FObjectPath::TryCreate(Row.Selection.ParentMaterialPath, ParentPath))
					Output.Parent = TStrongObjectPtr<DMaterial>(LoadObject<DMaterial>(ParentPath).value_or(nullptr));
				if (!Output.Parent.Get()) Row.Message = "Select an existing parent Material to create instances.";
				else if (std::ranges::any_of(Existing, [&](const auto& Entry) {
					return Entry.second == Output.Parent.Get();
				}) || std::ranges::any_of(Outputs, [&](const auto& Other) {
					return Other.AssetPath == Output.Parent->GetPackage()->GetPackagePathIdentity();
				})) Row.Message = "The selected parent is an output of this import. Select a parent outside the output set.";
				else
				{
					auto Standard = MakePBRSurfaceMaterialMRExpressions();
					Output.bStandardPBRParent = CheckParent(*Output.Parent, Standard).empty();
					if (!Output.bStandardPBRParent)
					{
						auto Recipe = MakeImportedSurfaceRecipe(MakeSceneSurfaceRoles(Plan, Output));
						Row.Message = CheckParent(*Output.Parent, Recipe.Graph);
					}
				}
				Row.bCompatible = Row.Message.empty();
			}
			if (!Row.bCompatible)
			{
				bValid = false;
				if (Error.empty()) Error = Row.SourceName + ": " + Row.Message;
			}
			else if (Row.Message.empty()) Row.Message = bInstance
				? (Output.bStandardPBRParent ? "Compatible standard PBR mapping" : "Compatible imported PBR mapping")
				: "Create editable local material";
		}
		return bValid;
	}

	auto PreviewSceneMaterials(std::string_view SourceFile, const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings, const FSceneMaterialImportOptions& MaterialOptions)
		-> FSceneMaterialPreviewResult
	{
		FSceneMaterialPreviewResult Result;
		if (SourceFile.empty() || !DestinationDirectory.IsValid() || !Settings.Validate())
		{
			Result.Message = "Select a valid source, destination and coordinate settings.";
			return Result;
		}
		const auto Source = std::filesystem::absolute(SourceFile).lexically_normal().generic_string();
		std::vector<FImportDiagnostic> Diagnostics;
		FSourceSnapshotBuilder Builder;
		if (!Builder.CaptureRootFilename(Source, Diagnostics) ||
			!Builder.DiscoverSourceDependencies(DiscoverSceneImportDependencies, Diagnostics))
		{
			Result.Message = Diagnostics.empty() ? "Scene source could not be read." : Diagnostics.back().Message;
			return Result;
		}
		auto Snapshot = Builder.Freeze(Diagnostics);
		FSceneImportPlan Plan;
		std::vector<FImportOutputSummary> Outputs;
		if (!Snapshot || !BuildScenePlan(*Snapshot, DestinationDirectory, Settings, Plan, Outputs, Diagnostics, Result.Message))
		{
			if (Result.Message.empty()) Result.Message = "Scene preview could not be built.";
			return Result;
		}
		Result.bSucceeded = ConfigureSceneMaterials(Plan, Outputs, Source, DestinationDirectory,
			MaterialOptions, Result.Materials, Result.Message);
		return Result;
	}
}
