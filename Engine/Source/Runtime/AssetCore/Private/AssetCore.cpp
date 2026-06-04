#include "AssetCore.h"

#include "Logging/LogMacros.h"
#include "Threading/Task.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace Durin
{
	namespace Asset
	{
		struct FAsyncMeshImportSharedState
		{
			FTaskHandle Task;
			mutable std::mutex Mutex;
			std::optional<FAsyncMeshImportResult> Result;
		};

		namespace
		{
			auto ImportMeshesFromFile(std::string_view FilePath) -> FAsyncMeshImportResult
			{
				FAsyncMeshImportResult Result;
				std::string OwnedFilePath(FilePath);
				Assimp::Importer Importer;

				const aiScene* Scene = Importer.ReadFile(OwnedFilePath.c_str(),
					aiProcess_Triangulate |
					aiProcess_FlipUVs |
					aiProcess_GenNormals |
					aiProcess_JoinIdenticalVertices);

				if (!Scene || (Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 || !Scene->mRootNode)
				{
					Result.ErrorMessage = Importer.GetErrorString();
					if (Result.ErrorMessage.empty())
					{
						Result.ErrorMessage = "Unknown mesh import failure.";
					}

					DURIN_ERROR("Asset import failed. (file: {}, error: {})", FilePath, Result.ErrorMessage);
					return Result;
				}

				Result.Meshes.resize(Scene->mNumMeshes);
				for (unsigned int MeshIndex = 0; MeshIndex < Scene->mNumMeshes; ++MeshIndex)
				{
					aiMesh* Mesh = Scene->mMeshes[MeshIndex];
					FTestAssetData& OutMesh = Result.Meshes[MeshIndex];

					OutMesh.Positions.reserve(Mesh->mNumVertices);
					OutMesh.Normals.reserve(Mesh->mNumVertices);
					OutMesh.Colors.reserve(Mesh->mNumVertices);
					OutMesh.UVs.reserve(Mesh->mNumVertices);
					for (unsigned int VertexIndex = 0; VertexIndex < Mesh->mNumVertices; ++VertexIndex)
					{
						OutMesh.Positions.emplace_back(
							Mesh->mVertices[VertexIndex].x,
							Mesh->mVertices[VertexIndex].y,
							Mesh->mVertices[VertexIndex].z);

						if (Mesh->HasNormals())
						{
							OutMesh.Normals.emplace_back(
								Mesh->mNormals[VertexIndex].x,
								Mesh->mNormals[VertexIndex].y,
								Mesh->mNormals[VertexIndex].z);
						}

						if (Mesh->HasVertexColors(0))
						{
							OutMesh.Colors.emplace_back(
								Mesh->mColors[0][VertexIndex].r,
								Mesh->mColors[0][VertexIndex].g,
								Mesh->mColors[0][VertexIndex].b);
						}

						if (Mesh->mTextureCoords[0])
						{
							OutMesh.UVs.emplace_back(
								Mesh->mTextureCoords[0][VertexIndex].x,
								Mesh->mTextureCoords[0][VertexIndex].y);
						}
					}

					OutMesh.Indices.reserve(Mesh->mNumFaces * 3);
					for (unsigned int FaceIndex = 0; FaceIndex < Mesh->mNumFaces; ++FaceIndex)
					{
						const aiFace& Face = Mesh->mFaces[FaceIndex];
						for (unsigned int IndexIndex = 0; IndexIndex < Face.mNumIndices; ++IndexIndex)
						{
							OutMesh.Indices.emplace_back(Face.mIndices[IndexIndex]);
						}
					}
				}

				Result.bSucceeded = true;
				return Result;
			}
		}

		FAsyncMeshImportHandle::FAsyncMeshImportHandle() = default;

		FAsyncMeshImportHandle::FAsyncMeshImportHandle(std::shared_ptr<FAsyncMeshImportSharedState> InState)
			: State(std::move(InState))
		{
		}

		auto FAsyncMeshImportHandle::IsValid() const -> bool
		{
			return State && State->Task.IsValid();
		}

		auto FAsyncMeshImportHandle::IsComplete() const -> bool
		{
			return State && State->Task.IsComplete();
		}

		auto FAsyncMeshImportHandle::Wait() const -> void
		{
			if (!State)
			{
				return;
			}

			WaitTask(State->Task);
		}

		auto FAsyncMeshImportHandle::GetDebugName() const -> const char*
		{
			return State ? State->Task.GetDebugName() : "";
		}

		auto FAsyncMeshImportHandle::TryGetResult(FAsyncMeshImportResult& OutResult) const -> bool
		{
			if (!State || !State->Task.IsComplete())
			{
				return false;
			}

			std::lock_guard Lock(State->Mutex);
			if (!State->Result.has_value())
			{
				return false;
			}

			OutResult = *State->Result;
			return true;
		}

		auto ImportFromFile(std::string_view FilePath, std::vector<FTestAssetData>& OutDatas) -> bool
		{
			FAsyncMeshImportResult Result = ImportMeshesFromFile(FilePath);
			if (!Result.bSucceeded)
			{
				OutDatas.clear();
				return false;
			}

			OutDatas = std::move(Result.Meshes);
			return true;
		}

		auto ImportFromFileAsync(std::string_view FilePath) -> FAsyncMeshImportHandle
		{
			auto SharedState = std::make_shared<FAsyncMeshImportSharedState>();
			std::string OwnedFilePath(FilePath);
			SharedState->Task = LaunchTask("AssetImport.Mesh", [SharedState, FilePath = std::move(OwnedFilePath)]() mutable {
				FAsyncMeshImportResult Result = ImportMeshesFromFile(FilePath);

				std::lock_guard Lock(SharedState->Mutex);
				SharedState->Result = std::move(Result);
			});

			if (!SharedState->Task.IsValid())
			{
				return {};
			}

			return FAsyncMeshImportHandle(std::move(SharedState));
		}
	}
} // namespace Durin
