#pragma once

#include "AssetCore/API.h"

namespace Doge
{
	namespace Asset
	{
		struct FTestAssetData
		{
			std::vector<glm::vec3> Positions;
			std::vector<glm::vec3> Normals;
			std::vector<glm::vec3> Colors;
			std::vector<glm::vec2> UVs;
			std::vector<uint32> Indices;
		};

		ASSETCORE_API auto ImportFromFile(std::string_view FilePath, std::vector<FTestAssetData>& OutData) -> bool;
	} // namespace AssetImport
} // namespace Doge