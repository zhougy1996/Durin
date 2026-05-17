#include "AssetCore.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace Durin
{

	namespace Asset
	{
		auto ImportFromFile(std::string_view FilePath,  std::vector<FTestAssetData>& OutDatas) -> bool
		{
			Assimp::Importer importer;

			// Read the file with post-processing flags
			// aiProcess_Triangulate: Force polygons to triangles
			// aiProcess_FlipUVs: Flip texture coordinates on Y axis (common for Vulkan/DirectX)
			// aiProcess_GenNormals: Generate smooth normals if they are missing
			const aiScene* scene = importer.ReadFile(FilePath.data(),
				aiProcess_Triangulate |
				aiProcess_FlipUVs |
				aiProcess_GenNormals |
				aiProcess_JoinIdenticalVertices);

			// Check if the scene was loaded successfully
			if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
				std::cerr << "ASSIMP ERROR: " << importer.GetErrorString() << std::endl;
				return false;
			}

			OutDatas.clear();
			OutDatas.resize(scene->mNumMeshes);
			// Process all meshes in the scene
			for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
				aiMesh* mesh = scene->mMeshes[i];
				auto& OutMesh = OutDatas[i];

				// Extract vertex data
				OutMesh.Positions.reserve(OutMesh.Positions.size() + mesh->mNumVertices);
				OutMesh.Normals.reserve(OutMesh.Normals.size() + mesh->mNumVertices);
				OutMesh.UVs.reserve(OutMesh.UVs.size() + mesh->mNumVertices);
				for (unsigned int j = 0; j < mesh->mNumVertices; j++) {
					// Position
					float px = mesh->mVertices[j].x;
					float py = mesh->mVertices[j].y;
					float pz = mesh->mVertices[j].z;
					OutMesh.Positions.emplace_back(px, py, pz);

					// Normal (if available)
					if (mesh->HasNormals()) {
						float nx = mesh->mNormals[j].x;
						float ny = mesh->mNormals[j].y;
						float nz = mesh->mNormals[j].z;
						OutMesh.Normals.emplace_back(nx, ny, nz);
					}

					// UVs (if available)
					if (mesh->mTextureCoords[0]) {
						float u = mesh->mTextureCoords[0][j].x;
						float v = mesh->mTextureCoords[0][j].y;
						OutMesh.UVs.emplace_back(u, v);
					}
				}

				// Extract index data
				OutMesh.Indices.reserve(OutMesh.Indices.size() + mesh->mNumFaces * 3);
				for (unsigned int j = 0; j < mesh->mNumFaces; j++) {
					aiFace face = mesh->mFaces[j];
					for (unsigned int k = 0; k < face.mNumIndices; k++) {
						uint32_t index = face.mIndices[k];
						// ... store to your index buffer
						OutMesh.Indices.emplace_back(index);
					}
				}
			}

			return true;
		}
	}
} // namespace Doge