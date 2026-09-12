#include "AssetForge/Builtins/SceneImport.h"
#include "ImportedSurfaceLegacyUpgrade.h"
#include "AssetForge/Builtins/StandardMaterialFunctions.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Asset.h"
#include "AssetTools/IAssetTools.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	namespace
	{
		auto EnsureTemplateProgram(DMaterial& Material, const FStandardMaterialFunctions& Functions,
			std::string& OutError) -> bool
		{
			std::vector<FMaterialFunctionCall> Calls;
			FMaterialGraphPresentation Presentation;
			const auto Expected = MakeImportedSurfaceFunctionProgram(Functions, Calls, Presentation);
			if (*Material.GetMaterialProgram() == Expected
				&& std::ranges::equal(Material.GetMaterialFunctionCalls(), Calls)) return true;
			// Only the exact shipped expanded template is eligible for automatic replacement.
			if (!Material.GetMaterialFunctionCalls().empty()
				|| (*Material.GetMaterialProgram() != LegacyUpgrade::MakeImportedSurfaceProgram()
					&& *Material.GetMaterialProgram() != LegacyUpgrade::MakeImportedSurfaceAggregateProgram()))
			{
				OutError = "ImportedSurface has a modified material graph; preserve it and resolve the template conflict before importing.";
				const auto& Actual = *Material.GetMaterialProgram();
				const auto Legacy = LegacyUpgrade::MakeImportedSurfaceProgram();
				OutError += std::format(" Schema {}, nodes {} (expected {}), calls {}.", Actual.SchemaVersion,
					Actual.Nodes.size(), Legacy.Nodes.size(), Material.GetMaterialFunctionCalls().size());
				for (size_t I = 0; I < std::min(Actual.Nodes.size(), Legacy.Nodes.size()); ++I)
					if (Actual.Nodes[I] != Legacy.Nodes[I])
					{
						OutError += std::format(" First differing node {}: opcode {} (expected {}), name '{}', identity matches {}, inputs match {}.",
							I, static_cast<uint32>(Actual.Nodes[I].Opcode), static_cast<uint32>(Legacy.Nodes[I].Opcode),
							Actual.Nodes[I].DisplayName, Actual.Nodes[I].Id == Legacy.Nodes[I].Id, Actual.Nodes[I].Inputs == Legacy.Nodes[I].Inputs);
						break;
					}
				return false;
			}
			const auto PreviousProgram = *Material.GetMaterialProgram();
			const auto PreviousPresentation = Material.GetMaterialGraphPresentation();
			const bool bWasDirty = Material.GetPackage()->IsDirty();
			const auto Applied = Material.SetMaterialProgramAndFunctionCalls(Expected, std::move(Calls));
			if (!Applied)
			{
				OutError = Applied.Diagnostics.empty() ? "ImportedSurface upgrade failed." : Applied.Diagnostics.front().Message;
				return false;
			}
			Material.SetMaterialGraphPresentation(std::move(Presentation));
			const auto Saved = SavePackage(Material.GetPackage());
			if (!Saved)
			{
				(void)Material.SetMaterialProgramAndFunctionCalls(PreviousProgram, {});
				Material.SetMaterialGraphPresentation(PreviousPresentation);
				if (!bWasDirty) Material.GetPackage()->ClearDirty();
				OutError = Saved.Message;
				return false;
			}
			return true;
		}
	}
	auto EnsureImportedSurfaceMaterial(std::string& OutError) -> DMaterial*
	{
		FStandardMaterialFunctions Functions;
		if (!EnsureStandardMaterialFunctions(Functions, OutError)) return nullptr;
		FPackagePath MaterialPath;
		if (!FPackagePath::TryCreate(
			ImportedSurfaceMaterialPackagePath, MaterialPath, &OutError)) return nullptr;

		DPackage* LoadedPackage = FindResidentPackage(MaterialPath);
		if (LoadedPackage)
		{
			DObject* TopLevel = LoadedPackage->FindTopLevelAsset(
				FName(MaterialPath.GetPackageName()));
			DMaterial* Loaded = Cast<DMaterial>(TopLevel);
			if (!Loaded)
			{
				OutError = std::format(
					"Standard imported-surface path {} is occupied by {}.",
					MaterialPath.ToString(),
					TopLevel
						? TopLevel->GetClass()->GetQualifiedName().ToString()
						: std::string("an invalid package"));
				return nullptr;
			}
			const auto DeclarationValidation = ValidateMaterialParameterDefinitions(
				Loaded->GetParameterDefinitions());
			if (!DeclarationValidation)
			{
				OutError = std::string(GetMaterialParameterErrorText(
					DeclarationValidation.Error));
				return nullptr;
			}
			if (!EnsureTemplateProgram(*Loaded, Functions, OutError)) return nullptr;
			OutError.clear();
			return Loaded;
		}

		if (FindAssetExact(MaterialPath))
		{
			FObjectPath MaterialObjectPath;
			if (!FObjectPath::TryCreate(
				ImportedSurfaceMaterialObjectPath, MaterialObjectPath, &OutError))
				return nullptr;
			DMaterial* Loaded = nullptr;
			const FAssetResult LoadResult = LoadObject(
				MaterialObjectPath, Loaded);
			if (!LoadResult)
			{
				OutError = std::format(
					"Failed to load standard imported-surface material: {}",
					LoadResult.Message);
				return nullptr;
			}
			const auto DeclarationValidation = ValidateMaterialParameterDefinitions(
				Loaded->GetParameterDefinitions());
			if (!DeclarationValidation)
			{
				OutError = std::string(GetMaterialParameterErrorText(
					DeclarationValidation.Error));
				UnloadPackage(MaterialPath);
				return nullptr;
			}
			if (!EnsureTemplateProgram(*Loaded, Functions, OutError))
			{
				UnloadPackage(MaterialPath);
				return nullptr;
			}
			OutError.clear();
			return Loaded;
		}

		FTopLevelAssetPath MaterialAssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			MaterialPath, MaterialPath.GetPackageName(), MaterialAssetPath))
		{
			OutError = "The imported-surface material asset path is invalid.";
			return nullptr;
		}
		const FAssetToolsResult CreateResult = IAssetTools::Get().CreateAsset(
			MaterialAssetPath, DMaterial::StaticClass());
		DMaterial* Created = Cast<DMaterial>(CreateResult.Asset);
		if (!CreateResult || !Created)
		{
			OutError = std::format(
				"Failed to create standard imported-surface material: {}",
				CreateResult.Message.empty()
					? "the asset tool returned no material" : CreateResult.Message);
			return nullptr;
		}
		std::vector<FMaterialFunctionCall> Calls;
		FMaterialGraphPresentation Presentation;
		const auto TemplateProgram = MakeImportedSurfaceFunctionProgram(Functions, Calls, Presentation);
		const auto TemplateResult = Created->SetMaterialDefinitionsAndProgram(
			MakePBRMaterialParameterDefinitions(), TemplateProgram, std::move(Calls));
		if (!TemplateResult)
		{
			OutError = TemplateResult.Diagnostics.empty()
				? "Failed to initialize the standard imported-surface material program."
				: TemplateResult.Diagnostics.front().Message;
			UnloadPackage(Created->GetPackage(),
				Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return nullptr;
		}
		if (!Created->SetMaterialGraphPresentation(
			std::move(Presentation)))
		{
			OutError = "Failed to initialize the standard imported-surface material graph presentation.";
			UnloadPackage(Created->GetPackage(),
				Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return nullptr;
		}
		const FAssetResult SaveResult = SavePackage(Created->GetPackage());
		if (!SaveResult)
		{
			OutError = std::format(
				"Failed to save standard imported-surface material: {}",
				SaveResult.Message);
			UnloadPackage(Created->GetPackage(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return nullptr;
		}
		OutError.clear();
		return Created;
	}
}
