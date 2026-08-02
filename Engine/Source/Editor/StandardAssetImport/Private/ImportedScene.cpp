#include "ImportedScene.h"

#include "ImportedSceneInternal.h"

#include "Logging/LogMacros.h"
#include "Threading/Task.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace Durin::Asset
{
	struct FAsyncMeshImportSharedState
	{
		FTaskHandle Task;
		mutable std::mutex Mutex;
		std::optional<FAsyncMeshImportResult> Result;
	};

	namespace
	{
		auto ValidateGltfMaterialProjection(
			const aiScene& Scene,
			std::span<const uint32> SourcePrimitiveMaterialIndices,
			FAsyncMeshImportResult& Result) -> bool
		{
			if (SourcePrimitiveMaterialIndices.size() != Scene.mNumMeshes)
			{
				return Private::FailImport(
					Result,
					EImportDiagnosticCategory::InvalidReference,
					"meshes",
					"Assimp geometry does not match the glTF primitive count.");
			}

			for (uint32 SourceMaterialIndex : SourcePrimitiveMaterialIndices)
			{
				if (SourceMaterialIndex >= Result.Scene.Materials.size())
				{
					return Private::FailImport(
						Result,
						EImportDiagnosticCategory::InvalidReference,
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
						EImportDiagnosticCategory::InvalidReference,
						std::format("mesh:{}", MeshIndex),
						"Assimp geometry contains a null mesh.");
				}
			}
			return true;
		}

		auto ImportMeshesFromFile(
			std::string_view FilePath,
			const FMeshImportOptions& Options) -> FAsyncMeshImportResult
		{
			FAsyncMeshImportResult Result;
			const std::filesystem::path RootPath = std::filesystem::path(std::string(FilePath));
			if (!Private::IsValidSourcePath(Options.RootSource.Path))
			{
				Private::FailImport(
					Result,
					EImportDiagnosticCategory::UnsafeDependencyPath,
					Options.RootSource.Path,
					"Root source path is not a normalized mounted virtual path.");
				return Result;
			}

			std::vector<uint8> RootBytes;
			std::string ReadError;
			if (!Private::ReadFileBytes(RootPath, MaxImportedSceneSourceBytes, RootBytes, ReadError))
			{
				const bool bExists = std::filesystem::exists(RootPath);
				Private::FailImport(
					Result,
					bExists
						? EImportDiagnosticCategory::ResourceLimitExceeded
						: EImportDiagnosticCategory::MissingDependency,
					"root",
					ReadError);
				return Result;
			}
			if (!Private::AppendDependency(
				Result.Scene,
				EImportedDependencyRole::RootScene,
				"root",
				Options.RootSource.Path,
				RootBytes))
			{
				Private::FailImport(
					Result,
					EImportDiagnosticCategory::ResourceLimitExceeded,
					"dependencies",
					"Imported dependency count exceeds the limit.");
				return Result;
			}

			std::string Extension = RootPath.extension().string();
			std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			const bool bGltf = Extension == ".gltf";
			const bool bGlb = Extension == ".glb";
			const Private::FImportedSceneContext Context{
				RootPath, Options.RootSource.Path, RootBytes, Result};
			std::vector<uint32> SourcePrimitiveMaterialIndices;
			if ((bGltf || bGlb)
				&& !Private::ImportGltfFormat(Context, bGlb, SourcePrimitiveMaterialIndices))
			{
				DURIN_ERROR(
					"Asset import failed. (file: {}, error: {})",
					FilePath,
					Result.ErrorMessage);
				return Result;
			}

			Assimp::Importer Importer;
			const std::string OwnedFilePath(FilePath);
			const aiScene* Scene = Importer.ReadFile(
				OwnedFilePath.c_str(),
				aiProcess_Triangulate
					| aiProcess_FlipUVs
					| aiProcess_GenSmoothNormals
					| aiProcess_CalcTangentSpace
					| aiProcess_JoinIdenticalVertices);
			if (Scene == nullptr
				|| (Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0
				|| Scene->mRootNode == nullptr)
			{
				std::string ErrorMessage = Importer.GetErrorString();
				if (ErrorMessage.empty()) ErrorMessage = "Unknown mesh import failure.";
				Private::FailImport(
					Result,
					EImportDiagnosticCategory::InvalidValue,
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
					EImportDiagnosticCategory::InvalidReference,
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
						EImportDiagnosticCategory::InvalidReference,
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

	FAsyncMeshImportHandle::FAsyncMeshImportHandle() = default;

	FAsyncMeshImportHandle::FAsyncMeshImportHandle(
		std::shared_ptr<FAsyncMeshImportSharedState> InState)
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
		if (State) WaitTask(State->Task);
	}

	auto FAsyncMeshImportHandle::GetDebugName() const -> const char*
	{
		return State ? State->Task.GetDebugName() : "";
	}

	auto FAsyncMeshImportHandle::TryGetResult(FAsyncMeshImportResult& OutResult) const -> bool
	{
		if (!State || !State->Task.IsComplete()) return false;
		std::lock_guard Lock(State->Mutex);
		if (!State->Result.has_value()) return false;
		OutResult = *State->Result;
		return true;
	}

	auto ImportFromFile(
		std::string_view FilePath,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options) -> bool
	{
		FAsyncMeshImportResult Result = ImportMeshesFromFile(FilePath, Options);
		OutData = std::move(Result.Scene);
		return Result.bSucceeded;
	}

	auto ImportGeometryFromMemory(
		std::span<const uint8> EncodedBytes,
		std::string_view ExtensionHint,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options) -> bool
	{
		FAsyncMeshImportResult Result;
		if (!Private::IsValidSourcePath(Options.RootSource.Path)
			|| EncodedBytes.empty() || EncodedBytes.size() > MaxImportedSceneSourceBytes)
		{
			Private::FailImport(Result,
				EncodedBytes.size() > MaxImportedSceneSourceBytes
					? EImportDiagnosticCategory::ResourceLimitExceeded
					: EImportDiagnosticCategory::UnsafeDependencyPath,
				"root", "Captured static-mesh source or mounted identity is invalid.");
			OutData = std::move(Result.Scene);
			return false;
		}
		if (!Private::AppendDependency(Result.Scene, EImportedDependencyRole::RootScene,
			"root", Options.RootSource.Path, EncodedBytes))
		{
			Private::FailImport(Result, EImportDiagnosticCategory::ResourceLimitExceeded,
				"dependencies", "Imported dependency count exceeds the limit.");
			OutData = std::move(Result.Scene);
			return false;
		}

		std::string Hint(ExtensionHint);
		if (!Hint.empty() && Hint.front() == '.') Hint.erase(Hint.begin());
		std::ranges::transform(Hint, Hint.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
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
			Private::FailImport(Result, EImportDiagnosticCategory::InvalidValue, "root", Error);
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
			Private::FailImport(Result, EImportDiagnosticCategory::InvalidReference,
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

	auto ImportFromFileAsync(
		std::string_view FilePath,
		const FMeshImportOptions& Options) -> FAsyncMeshImportHandle
	{
		auto SharedState = std::make_shared<FAsyncMeshImportSharedState>();
		std::string OwnedFilePath(FilePath);
		SharedState->Task = LaunchTask(
			"StandardAssetImport.Scene",
			[SharedState, FilePath = std::move(OwnedFilePath), Options]() mutable {
				FAsyncMeshImportResult Result = ImportMeshesFromFile(FilePath, Options);
				std::lock_guard Lock(SharedState->Mutex);
				SharedState->Result = std::move(Result);
			});
		if (!SharedState->Task.IsValid()) return {};
		return FAsyncMeshImportHandle(std::move(SharedState));
	}
}
