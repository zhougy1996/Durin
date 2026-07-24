#pragma once

#include "AssetCoreAPI.h"
#include "CoreFwd.h"
#include "Math/MathFwd.h"
#include "AssetSystem.h"
#include "ImageDecoder.h"

#include <glm/mat4x4.hpp>

namespace Durin
{
	namespace Asset
	{
		inline constexpr uint32 MaxImportedUVChannels = 4;

		// Defines the source-space transform applied while importing mesh data.
		struct FMeshImportOptions
		{
			// Converts source positions and directions into engine space.
			glm::mat4 SourceToEngine{1.0f};
		};

		// Identifies a material slot retained from the source scene.
		struct FImportedMaterialSlot
		{
			std::string Name;

			// Index used by imported meshes to reference this slot.
			uint32 SourceMaterialIndex = 0;

			std::string SourceName;
		};

		// Carries one imported mesh in engine-ready vertex and index streams.
		struct FImportedMeshData
		{
			std::string Name;
			std::vector<glm::vec3> Positions;
			std::vector<glm::vec3> Normals;
			std::vector<glm::vec4> Tangents;

			// UV streams preserve their source channel indices up to the supported limit.
			std::array<std::vector<glm::vec2>, MaxImportedUVChannels> UVChannels;

			std::vector<glm::vec4> Colors;
			std::vector<uint32> Indices;

			// Index into FImportedSceneData::MaterialSlots.
			uint32 MaterialIndex = 0;
		};

		// Aggregates the meshes and shared material slots produced by one scene import.
		struct FImportedSceneData
		{
			std::vector<FImportedMaterialSlot> MaterialSlots;
			std::vector<FImportedMeshData> Meshes;
		};

		// Captures the terminal value of an asynchronous mesh import.
		struct FAsyncMeshImportResult
		{
			bool bSucceeded = false;
			FImportedSceneData Scene;
			std::string ErrorMessage;
		};

		struct FAsyncMeshImportSharedState;

		// Provides shared access to the progress and result of an import task.
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

			// Keeps the task state alive until every copied handle is released.
			std::shared_ptr<FAsyncMeshImportSharedState> State;

			friend ASSETCORE_API auto ImportFromFileAsync(std::string_view FilePath, const FMeshImportOptions& Options) -> FAsyncMeshImportHandle;
		};

		ASSETCORE_API auto ImportFromFile(std::string_view FilePath, FImportedSceneData& OutData, const FMeshImportOptions& Options = {}) -> bool;
		ASSETCORE_API auto ImportFromFileAsync(std::string_view FilePath, const FMeshImportOptions& Options = {}) -> FAsyncMeshImportHandle;
	} // namespace AssetImport
} // namespace Durin
