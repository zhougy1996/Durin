#pragma once

#include "AssetImport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	struct FStaticModelImportPlanData
	{
		Asset::FImportedSceneData Scene;
		std::filesystem::path PhysicalRootSource;
		FStaticMeshImportSettings ImportSettings;
		DStaticMesh* ExistingMesh = nullptr;
		std::unordered_map<size_t, DMaterialInstance*> ExistingMaterials;
		std::unordered_map<size_t, DTexture2D*> ExistingTextures;
		std::vector<FAssetPath> OrphanedAssets;
		uint8 FailurePoint = 0;
		size_t FailureOccurrence = 0;
	};
}
