#include "AssetForge/Builtins/ImportedScene.h"

#include "ImportedSceneInternal.h"

#include "Logging/LogMacros.h"
#include "Misc/StringHelper.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace Durin::AssetForge::Builtins
{
	using namespace Durin;
	auto IsSceneSurfaceImageEncodingSupported(EImportedImageEncoding Encoding) -> bool
	{
		switch (Encoding)
		{
		case EImportedImageEncoding::Png:
		case EImportedImageEncoding::Jpeg:
		case EImportedImageEncoding::Bmp:
		case EImportedImageEncoding::Tga:
			return true;
		}
		return false;
	}

	namespace
	{
		auto ValidateGltfMaterialProjection(
			const aiScene& Scene,
			std::span<const uint32> SourcePrimitiveMaterialIndices,
			FSceneDecodeResult& Result) -> bool
		{
			if (SourcePrimitiveMaterialIndices.size() != Scene.mNumMeshes)
			{
				return Private::FailImport(
					Result,
					ESceneImportDiagnosticCategory::InvalidReference,
					"meshes",
					"Assimp geometry does not match the glTF primitive count.");
			}

			for (uint32 SourceMaterialIndex : SourcePrimitiveMaterialIndices)
			{
				if (SourceMaterialIndex >= Result.Scene.Materials.size())
				{
					return Private::FailImport(
						Result,
						ESceneImportDiagnosticCategory::InvalidReference,
						std::format("material:{}", SourceMaterialIndex),
						"glTF primitive references a material outside the normalized material table.");
				}
			}

			for (uint32 MeshIndex = 0; MeshIndex < Scene.mNumMeshes; ++MeshIndex)
			{
				const aiMesh* Mesh = Scene.mMeshes[MeshIndex];
				if (Mesh == nullptr)
				{
					return Private::FailImport(
						Result,
						ESceneImportDiagnosticCategory::InvalidReference,
						std::format("mesh:{}", MeshIndex),
						"Assimp geometry contains a null mesh.");
				}
			}
			return true;
		}

		auto ImportMeshesFromFile(
			std::string_view FilePath,
			const FMeshImportOptions& Options) -> FSceneDecodeResult
		{
			FSceneDecodeResult Result;
			const std::filesystem::path RootPath = std::filesystem::path(std::string(FilePath));
			if (!Private::IsValidSourcePath(Options.RootSourcePath))
			{
				Private::FailImport(
					Result,
					ESceneImportDiagnosticCategory::UnsafeDependencyPath,
					Options.RootSourcePath,
					"Root source path is not a normalized source filename.");
				return Result;
			}

			FByteBuffer RootBytes;
			std::string ReadError;
			if (!Private::ReadFileBytes(RootPath, MaxImportedSceneSourceBytes, RootBytes, ReadError))
			{
				const bool bExists = std::filesystem::exists(RootPath);
				Private::FailImport(
					Result,
					bExists
						? ESceneImportDiagnosticCategory::ResourceLimitExceeded
						: ESceneImportDiagnosticCategory::MissingDependency,
					"root",
					ReadError);
				return Result;
			}
			if (!Private::AppendDependency(
				Result.Scene,
				EImportedDependencyRole::RootScene,
				"root",
				Options.RootSourcePath,
				RootBytes))
			{
				Private::FailImport(
					Result,
					ESceneImportDiagnosticCategory::ResourceLimitExceeded,
					"dependencies",
					"Imported dependency count exceeds the limit.");
				return Result;
			}

			const std::string Extension = StringUtils::FoldAscii(RootPath.extension().string());
			const bool bGltf = Extension == ".gltf";
			const bool bGlb = Extension == ".glb";
			const Private::FImportedSceneContext Context{
				RootPath, Options.RootSourcePath, RootBytes, Options, Result};
			std::vector<uint32> SourcePrimitiveMaterialIndices;
			FByteBuffer AssimpProjection;
			if ((bGltf || bGlb)
				&& !Private::ImportGltfFormat(
					Context, bGlb, SourcePrimitiveMaterialIndices, AssimpProjection))
			{
				DURIN_ERROR(
					"Asset import failed. (file: {}, error: {})",
					FilePath,
					Result.ErrorMessage);
				return Result;
			}

			Assimp::Importer Importer;
			const std::string OwnedFilePath(FilePath);
			const unsigned int AssimpFlags =
				aiProcess_Triangulate
					| aiProcess_FlipUVs
					| aiProcess_GenSmoothNormals
					| aiProcess_CalcTangentSpace
					| aiProcess_JoinIdenticalVertices;
			const aiScene* Scene = (bGltf || bGlb)
				? Importer.ReadFileFromMemory(
					AssimpProjection.data(), AssimpProjection.size(), AssimpFlags, "gltf")
				: Importer.ReadFile(OwnedFilePath.c_str(), AssimpFlags);
			if (Scene == nullptr
				|| (Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0
				|| Scene->mRootNode == nullptr)
			{
				std::string ErrorMessage = Importer.GetErrorString();
				if (ErrorMessage.empty()) ErrorMessage = "Unknown mesh import failure.";
				Private::FailImport(
					Result,
					ESceneImportDiagnosticCategory::InvalidValue,
					"root",
					ErrorMessage);
				DURIN_ERROR("Asset import failed. (file: {}, error: {})", FilePath, ErrorMessage);
				return Result;
			}

			if (!bGltf && !bGlb && !Private::ImportAssimpFormat(*Scene, Context))
			{
				DURIN_ERROR(
					"Asset import failed. (file: {}, error: {})",
					FilePath,
					Result.ErrorMessage);
				return Result;
			}
			if (Result.Scene.Materials.empty())
			{
				Result.Scene.Materials.push_back({.SourceMaterialIndex = 0, .SourceName = {}});
			}
			if ((bGltf || bGlb)
				&& !ValidateGltfMaterialProjection(
					*Scene, SourcePrimitiveMaterialIndices, Result))
			{
				return Result;
			}

			if (!Private::ImportAssimpGeometry(
				*Scene,
				Options,
				(bGltf || bGlb)
					? std::span<const uint32>(SourcePrimitiveMaterialIndices)
					: std::span<const uint32>{},
				Result.Scene,
				Result.ErrorMessage))
			{
				const std::string ErrorMessage = Result.ErrorMessage;
				Private::FailImport(
					Result,
					ESceneImportDiagnosticCategory::InvalidReference,
					"meshes",
					ErrorMessage);
				DURIN_ERROR(
					"Asset import failed. (file: {}, error: {})",
					FilePath,
					Result.ErrorMessage);
				return Result;
			}

			for (const FImportedMeshData& Mesh : Result.Scene.Meshes)
			{
				const auto MaterialIt = std::ranges::find(
					Result.Scene.Materials,
					Mesh.SourceMaterialIndex,
					&FImportedMaterial::SourceMaterialIndex);
				if (MaterialIt == Result.Scene.Materials.end())
				{
					Private::FailImport(
						Result,
						ESceneImportDiagnosticCategory::InvalidReference,
						std::format("material:{}", Mesh.SourceMaterialIndex),
						"Imported mesh references a source material that was not normalized.");
					return Result;
				}
			}

			std::unordered_map<std::string, uint32> MaterialNameCounts;
			Result.Scene.MaterialSlots.reserve(Result.Scene.Materials.size());
			for (const FImportedMaterial& Material : Result.Scene.Materials)
			{
				if (std::ranges::none_of(
					Result.Scene.Meshes,
					[&Material](const FImportedMeshData& Mesh) {
						return Mesh.SourceMaterialIndex == Material.SourceMaterialIndex;
					}))
				{
					continue;
				}
				Result.Scene.MaterialSlots.push_back({
					Private::MakeUniqueName(
						Material.SourceName,
						Material.SourceMaterialIndex,
						MaterialNameCounts),
					Material.SourceMaterialIndex,
					Material.SourceName});
			}
			if (Result.Scene.MaterialSlots.empty())
			{
				Result.Scene.MaterialSlots.push_back({"Default", 0, {}});
			}

			Result.bSucceeded = true;
			return Result;
		}
	}

	auto ImportFromFile(
		std::string_view FilePath,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options) -> bool
	{
		FSceneDecodeResult Result = ImportMeshesFromFile(FilePath, Options);
		OutData = std::move(Result.Scene);
		return Result.bSucceeded;
	}

	auto ImportGeometryFromMemory(
		FByteView EncodedBytes,
		std::string_view ExtensionHint,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options) -> bool
	{
		FSceneDecodeResult Result;
		if (!Private::IsValidSourcePath(Options.RootSourcePath)
			|| EncodedBytes.empty() || EncodedBytes.size() > MaxImportedSceneSourceBytes)
		{
			Private::FailImport(Result,
				EncodedBytes.size() > MaxImportedSceneSourceBytes
					? ESceneImportDiagnosticCategory::ResourceLimitExceeded
					: ESceneImportDiagnosticCategory::UnsafeDependencyPath,
				"root", "Captured static-mesh source or filename is invalid.");
			OutData = std::move(Result.Scene);
			return false;
		}
		if (!Private::AppendDependency(Result.Scene, EImportedDependencyRole::RootScene,
			"root", Options.RootSourcePath, EncodedBytes))
		{
			Private::FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
				"dependencies", "Imported dependency count exceeds the limit.");
			OutData = std::move(Result.Scene);
			return false;
		}

		std::string Hint = StringUtils::FoldAscii(ExtensionHint);
		if (!Hint.empty() && Hint.front() == '.') Hint.erase(Hint.begin());
		Assimp::Importer Importer;
		const aiScene* Scene = Importer.ReadFileFromMemory(
			EncodedBytes.data(), EncodedBytes.size(),
			aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenSmoothNormals
				| aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices,
			Hint.empty() ? nullptr : Hint.c_str());
		if (!Scene || (Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0
			|| !Scene->mRootNode)
		{
			std::string Error = Importer.GetErrorString();
			if (Error.empty()) Error = "Unknown in-memory geometry import failure.";
			Private::FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue, "root", Error);
			OutData = std::move(Result.Scene);
			return false;
		}

		Result.Scene.Materials.reserve(std::max<uint32>(Scene->mNumMaterials, 1));
		for (uint32 MaterialIndex = 0; MaterialIndex < Scene->mNumMaterials; ++MaterialIndex)
		{
			aiString Name;
			if (Scene->mMaterials[MaterialIndex])
				Scene->mMaterials[MaterialIndex]->Get(AI_MATKEY_NAME, Name);
			Result.Scene.Materials.push_back({
				.SourceMaterialIndex = MaterialIndex,
				.SourceName = Name.C_Str()});
		}
		if (Result.Scene.Materials.empty())
			Result.Scene.Materials.push_back({.SourceMaterialIndex = 0});

		std::string GeometryError;
		if (!Private::ImportAssimpGeometry(
			*Scene, Options, {}, Result.Scene, GeometryError))
		{
			Private::FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
				"meshes", GeometryError);
			OutData = std::move(Result.Scene);
			return false;
		}
		std::unordered_map<std::string, uint32> MaterialNameCounts;
		for (const FImportedMaterial& Material : Result.Scene.Materials)
		{
			if (std::ranges::none_of(Result.Scene.Meshes,
				[&](const FImportedMeshData& Mesh) {
					return Mesh.SourceMaterialIndex == Material.SourceMaterialIndex;
				})) continue;
			Result.Scene.MaterialSlots.push_back({
				Private::MakeUniqueName(Material.SourceName, Material.SourceMaterialIndex,
					MaterialNameCounts),
				Material.SourceMaterialIndex,
				Material.SourceName});
		}
		if (Result.Scene.MaterialSlots.empty())
			Result.Scene.MaterialSlots.push_back({"Default", 0, {}});
		Result.bSucceeded = true;
		OutData = std::move(Result.Scene);
		return true;
	}

}
