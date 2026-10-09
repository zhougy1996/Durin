#include "SceneImportInternal.h"
#include "StaticMesh/StaticMeshImportSettings.h"
#include "AssetForge/Builtins/PBRSurfaceMaterial.h"
#include "Asset/Asset.h"
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

	auto ConfigureSceneMaterials(FAssetImportPlan& Plan,
		std::vector<FImportOutputSummary>& Outputs,
		const FPackagePath& Destination, const FSceneMaterialImportOptions& Options,
		std::vector<FSceneMaterialPreview>& Preview, std::string& Error) -> bool
	{
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
				Row.Selection.Mode != ESceneMaterialImportMode::CreateInstances &&
				Row.Selection.Mode != ESceneMaterialImportMode::UseExisting)
			{
				Error = "Unknown scene material import mode.";
				return false;
			}
			auto Summary = std::ranges::find(Outputs, Output.StableIdentity, &FImportOutputSummary::StableIdentity);
			Output.bExistingMaterialMapping = false;
			if (Row.Selection.Mode == ESceneMaterialImportMode::UseExisting)
			{
				FObjectPath Path;
				auto* Material = FObjectPath::TryCreate(Row.Selection.ExistingMaterialPath, Path)
					? LoadObject<DMaterialInterface>(Path).value_or(nullptr) : nullptr;
				if (!Material || !Material->GetPackage())
				{
					Row.Message = "Select a packaged existing material or material instance.";
					bValid = false;
				}
				else
				{
					Output.bExistingMaterialMapping = true;
					Output.PreservedMaterial = TStrongObjectPtr<DMaterialInterface>(Material);
					Output.Kind = Cast<DMaterialInstance>(Material) ? ESceneOutputKind::MaterialInstance : ESceneOutputKind::Material;
					Output.TextureBindings.clear();
					Output.Dependencies.clear();
					Summary->AssetPath = Material->GetPackage()->GetPackagePathIdentity();
					Summary->Role = Output.Kind == ESceneOutputKind::Material ? "Material" : "MaterialInstance";
					Summary->AssetClassName = Material->GetClass()->GetQualifiedName().ToString();
					Row.AssetPath = Summary->AssetPath;
					Row.bPreserved = Row.bCompatible = true;
					Row.Message = "Use existing material without modifying it";
				}
				continue;
			}

			const bool bInstance = Row.Selection.Mode == ESceneMaterialImportMode::CreateInstances;
			Output.Kind = bInstance ? ESceneOutputKind::MaterialInstance : ESceneOutputKind::Material;
			Summary->Role = bInstance ? "MaterialInstance" : "Material";
			Summary->AssetClassName = bInstance ? "Durin::DMaterialInstance" : "Durin::DMaterial";
			if (!FPackagePath::TryCreate(Destination.ToString() + "/Materials/" + (bInstance ? "MI_" : "M_")
				+ std::string(Summary->AssetPath.GetPackageName()), Summary->AssetPath))
				Row.Message = "The generated material path is invalid.";
			else if (FindAssetExact(Summary->AssetPath) || FindResidentPackage(Summary->AssetPath))
				Row.Message = "The material output path is occupied by an unrelated asset.";
			Row.AssetPath = Summary->AssetPath;
			Row.bCompatible = Row.bPreserved || Row.Message.empty();
			if (bInstance && !Row.bPreserved && Row.bCompatible)
			{
				FObjectPath ParentPath;
				if (FObjectPath::TryCreate(Row.Selection.ParentMaterialPath, ParentPath))
					Output.Parent = TStrongObjectPtr<DMaterial>(LoadObject<DMaterial>(ParentPath).value_or(nullptr));
				if (!Output.Parent.Get()) Row.Message = "Select an existing parent Material to create instances.";
				else if (std::ranges::any_of(Outputs, [&](const auto& Other) {
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
		if (!bValid)
			if (const auto Row = std::ranges::find(Preview, false, &FSceneMaterialPreview::bCompatible); Row != Preview.end()) Error = Row->Message;

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
		FAssetImportPlan Plan;
		std::vector<FImportOutputSummary> Outputs;
		if (!Snapshot || !BuildScenePlan(*Snapshot, DestinationDirectory, Settings, Plan, Outputs, Diagnostics, Result.Message))
		{
			if (Result.Message.empty()) Result.Message = "Scene preview could not be built.";
			return Result;
		}
		Result.bSucceeded = ConfigureSceneMaterials(Plan, Outputs, DestinationDirectory,
			MaterialOptions, Result.Materials, Result.Message);
		return Result;
	}
}
