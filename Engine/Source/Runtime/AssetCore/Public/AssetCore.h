#pragma once

#include "AssetCoreAPI.h"
#include "CoreFwd.h"
#include "Math/MathFwd.h"

namespace Durin
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

		struct FAsyncMeshImportResult
		{
			bool bSucceeded = false;
			std::vector<FTestAssetData> Meshes;
			std::string ErrorMessage;
		};

		struct FAsyncMeshImportSharedState;

		class FAsyncMeshImportHandle
		{
		public:
			ASSETCORE_API FAsyncMeshImportHandle();

			ASSETCORE_API auto IsValid() const -> bool;
			ASSETCORE_API auto IsComplete() const -> bool;
			ASSETCORE_API auto Wait() const -> void;
			ASSETCORE_API auto GetDebugName() const -> const char*;
			ASSETCORE_API auto TryGetResult(FAsyncMeshImportResult& OutResult) const -> bool;

		private:
			explicit FAsyncMeshImportHandle(std::shared_ptr<FAsyncMeshImportSharedState> InState);

			std::shared_ptr<FAsyncMeshImportSharedState> State;

			friend ASSETCORE_API auto ImportFromFileAsync(std::string_view FilePath) -> FAsyncMeshImportHandle;
		};

		ASSETCORE_API auto ImportFromFile(std::string_view FilePath, std::vector<FTestAssetData>& OutData) -> bool;
		ASSETCORE_API auto ImportFromFileAsync(std::string_view FilePath) -> FAsyncMeshImportHandle;
	} // namespace AssetImport
} // namespace Durin
