#pragma once

#include "AssetImportAPI.h"
#include "CoreFwd.h"
#include "Math/MathFwd.h"

#include <glm/mat4x4.hpp>

namespace Durin::Asset
{
	inline constexpr uint32 MaxImportedUVChannels = 4;

	struct FMeshImportOptions
	{
		glm::mat4 SourceToEngine{1.0f};
	};

	struct FImportedMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
		std::string SourceName;
	};

	struct FImportedMeshData
	{
		std::string Name;
		std::vector<glm::vec3> Positions;
		std::vector<glm::vec3> Normals;
		std::vector<glm::vec4> Tangents;
		std::array<std::vector<glm::vec2>, MaxImportedUVChannels> UVChannels;
		std::vector<glm::vec4> Colors;
		std::vector<uint32> Indices;
		uint32 MaterialIndex = 0;
	};

	struct FImportedSceneData
	{
		std::vector<FImportedMaterialSlot> MaterialSlots;
		std::vector<FImportedMeshData> Meshes;
	};

	struct FAsyncMeshImportResult
	{
		bool bSucceeded = false;
		FImportedSceneData Scene;
		std::string ErrorMessage;
	};

	struct FAsyncMeshImportSharedState;

	class FAsyncMeshImportHandle
	{
	public:
		ASSETIMPORT_API FAsyncMeshImportHandle();

		ASSETIMPORT_API auto IsValid() const -> bool;
		ASSETIMPORT_API auto IsComplete() const -> bool;
		ASSETIMPORT_API auto Wait() const -> void;
		ASSETIMPORT_API auto GetDebugName() const -> const char*;
		ASSETIMPORT_API auto TryGetResult(FAsyncMeshImportResult& OutResult) const -> bool;

	private:
		explicit FAsyncMeshImportHandle(std::shared_ptr<FAsyncMeshImportSharedState> InState);

		std::shared_ptr<FAsyncMeshImportSharedState> State;

		friend ASSETIMPORT_API auto ImportFromFileAsync(
			std::string_view FilePath,
			const FMeshImportOptions& Options) -> FAsyncMeshImportHandle;
	};

	ASSETIMPORT_API auto ImportFromFile(
		std::string_view FilePath,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options = {}) -> bool;
	ASSETIMPORT_API auto ImportFromFileAsync(
		std::string_view FilePath,
		const FMeshImportOptions& Options = {}) -> FAsyncMeshImportHandle;
}
