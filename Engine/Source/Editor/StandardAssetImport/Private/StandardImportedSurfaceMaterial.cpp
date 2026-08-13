#include "SceneImport.h"

#include "AssetSystem.h"
#include "Materials/Material.h"

namespace Durin::Asset::Import
{
	auto EnsureStandardImportedSurfaceMaterial(std::string& OutError) -> DMaterial*
	{
		FAssetPath MaterialPath;
		if (!FAssetPath::TryCreate(
			StandardImportedSurfaceMaterialPath, MaterialPath, &OutError)) return nullptr;

		if (DPackage* LoadedPackage = Asset::FindLoadedPackage(MaterialPath))
		{
			DMaterial* Loaded = Cast<DMaterial>(LoadedPackage->GetAsset());
			if (!Loaded)
			{
				OutError = std::format(
					"Standard imported-surface path {} is occupied by {}.",
					MaterialPath.ToString(),
					LoadedPackage->GetAsset()
						? LoadedPackage->GetAsset()->GetClass()->GetQualifiedName().ToString()
						: std::string("an invalid package"));
				return nullptr;
			}
			if (!ValidateCanonicalMaterialParameterDefinitions(
				Loaded->GetParameterDefinitions(), OutError)) return nullptr;
			OutError.clear();
			return Loaded;
		}

		if (Asset::GetAssetRegistry().FindAssetExact(MaterialPath))
		{
			DMaterial* Loaded = nullptr;
			const Asset::FAssetResult LoadResult = Asset::LoadAsset(MaterialPath, Loaded);
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
				Asset::UnloadPackage(MaterialPath);
				return nullptr;
			}
			OutError.clear();
			return Loaded;
		}

		DMaterial* Created = nullptr;
		const Asset::FAssetResult CreateResult = Asset::CreateAsset(MaterialPath, Created);
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
			Asset::DiscardUnpublishedPackage(Created->GetPackage());
			return nullptr;
		}
		const Asset::FAssetResult SaveResult = Asset::SavePackage(Created->GetPackage());
		if (!SaveResult)
		{
			OutError = std::format(
				"Failed to save standard imported-surface material: {}",
				SaveResult.Message);
			Asset::DiscardUnpublishedPackage(Created->GetPackage());
			return nullptr;
		}
		OutError.clear();
		return Created;
	}
}
