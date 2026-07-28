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
	};
}
