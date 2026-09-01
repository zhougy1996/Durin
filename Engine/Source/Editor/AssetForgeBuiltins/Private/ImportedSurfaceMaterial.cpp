#include "AssetForge/Builtins/SceneImport.h"

#include "Asset/AssetOperations.h"
#include "Asset/Asset.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	namespace
	{
		auto MakeTemplatePresentation(const FMaterialProgram& Program)
			-> FMaterialGraphPresentation
		{
			check(Program.Nodes.size() == 1);
			return {
				.Nodes = {{Program.Nodes.front().Id, 0, 0}},
				.bHasMaterialOutputPosition = true,
				.MaterialOutputX = 320,
				.MaterialOutputY = 0,
			};
		}

		auto EnsureTemplateProgram(DMaterial& Material, bool bAllowMigration,
			std::string& OutError) -> bool
		{
			const FMaterialProgram Expected = MakeStandardSurfaceMaterialProgram();
			if (*Material.GetMaterialProgram() == Expected) return true;
			if (bAllowMigration
				&& *Material.GetMaterialProgram() == MakeCanonicalMaterialProgram())
			{
				FMaterialProgramValidationResult Validation;
				if (Material.SetMaterialProgram(Expected, Validation)
					&& Material.SetMaterialGraphPresentation(
						MakeTemplatePresentation(Expected))) return true;
			}
			OutError = "ImportedSurface has a modified or stale material program; run the exact built-in template migration before importing.";
			return false;
		}
	}
	auto EnsureImportedSurfaceMaterial(std::string& OutError) -> DMaterial*
	{
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
			if (!ValidateCanonicalMaterialParameterDefinitions(
				Loaded->GetParameterDefinitions(), OutError)) return nullptr;
			if (!EnsureTemplateProgram(*Loaded, false, OutError)) return nullptr;
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
			if (!ValidateCanonicalMaterialParameterDefinitions(
				Loaded->GetParameterDefinitions(), OutError))
			{
				UnloadPackage(MaterialPath);
				return nullptr;
			}
			if (!EnsureTemplateProgram(*Loaded, true, OutError))
			{
				UnloadPackage(MaterialPath);
				return nullptr;
			}
			if (Loaded->GetPackage()->IsDirty())
			{
				const FAssetResult SaveResult = SavePackage(Loaded->GetPackage());
				if (!SaveResult)
				{
					OutError = SaveResult.Message;
					UnloadPackage(MaterialPath);
					return nullptr;
				}
			}
			OutError.clear();
			return Loaded;
		}

		DMaterial* Created = nullptr;
		FTopLevelAssetPath MaterialAssetPath;
		if (!FTopLevelAssetPath::TryCreate(
			MaterialPath, MaterialPath.GetPackageName(), MaterialAssetPath))
		{
			OutError = "The imported-surface material asset path is invalid.";
			return nullptr;
		}
		const FAssetResult CreateResult =
			CreateAsset(MaterialAssetPath, Created);
		if (!CreateResult)
		{
			OutError = std::format(
				"Failed to create standard imported-surface material: {}",
				CreateResult.Message);
			return nullptr;
		}
		if (!ValidateCanonicalMaterialParameterDefinitions(
			Created->GetParameterDefinitions(), OutError))
		{
			UnloadPackage(Created->GetPackage(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return nullptr;
		}
		FMaterialProgramValidationResult ProgramValidation;
		if (!Created->SetMaterialProgram(
			MakeStandardSurfaceMaterialProgram(), ProgramValidation))
		{
			OutError = ProgramValidation.Diagnostics.empty()
				? "Failed to initialize the standard imported-surface material program."
				: ProgramValidation.Diagnostics.front().Message;
			UnloadPackage(Created->GetPackage(),
				Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
			return nullptr;
		}
		if (!Created->SetMaterialGraphPresentation(
			MakeTemplatePresentation(*Created->GetMaterialProgram())))
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
