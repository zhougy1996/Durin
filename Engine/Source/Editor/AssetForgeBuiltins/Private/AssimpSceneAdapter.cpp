#include "ImportedSceneInternal.h"

#include <assimp/material.h>
#include <assimp/scene.h>

namespace Durin::AssetForge::Builtins::Private
{
	using namespace Durin::Asset;
	auto ImportAssimpImage(
		const aiScene& Scene,
		const std::filesystem::path& RootPath,
		std::string_view RootSourcePath,
		std::string TexturePath,
		FSceneDecodeResult& Result,
		std::unordered_map<std::string, uint32>& ImageIndices,
		uint64& EmbeddedByteCount,
		uint32& OutImageIndex) -> bool
	{
		if (CheckSceneDecodeCancellation(Result, TexturePath)) return false;
		std::ranges::replace(TexturePath, '\\', '/');
		if (const auto Existing = ImageIndices.find(TexturePath); Existing != ImageIndices.end())
		{
			OutImageIndex = Existing->second;
			return true;
		}
		if (Result.Scene.Images.size() >= MaxImportedImages)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
				"images", "Imported image count exceeds the limit.");
		}

		FImportedImage Imported;
		std::span<const std::byte> EncodedBytes;
		std::vector<std::byte> OwnedBytes;
		std::string EncodingHint;
		if (const aiTexture* Embedded = Scene.GetEmbeddedTexture(TexturePath.c_str()))
		{
			if (Embedded->mHeight != 0)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedEncoding,
					TexturePath, "Uncompressed Assimp embedded textures are not accepted as encoded image sources.");
			}
			if (Embedded->mWidth > MaxImportedImageEncodedBytes)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
					TexturePath, "Embedded image exceeds the per-image byte limit.");
			}
			OwnedBytes.assign(
				reinterpret_cast<const std::byte*>(Embedded->pcData),
				reinterpret_cast<const std::byte*>(Embedded->pcData) + Embedded->mWidth);
			EncodedBytes = OwnedBytes;
			EmbeddedByteCount += OwnedBytes.size();
			if (EmbeddedByteCount > MaxImportedEmbeddedImageBytes)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
					TexturePath, "Embedded image bytes exceed the aggregate limit.");
			}
			EncodingHint = Embedded->achFormatHint;
			if (!EncodingHint.empty() && EncodingHint.front() != '.')
				EncodingHint.insert(EncodingHint.begin(), '.');
			Imported.StableIdentity = std::format("assimp-embedded:{}", TexturePath);
			Imported.SuggestedName = std::format("Embedded_{}", Result.Scene.Images.size());
		}
		else
		{
			std::filesystem::path DependencyPath;
			std::string NormalizedUri;
			if (!ResolveDependencyPath(RootPath, TexturePath, DependencyPath, NormalizedUri))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsafeDependencyPath,
					TexturePath, std::format("Material texture path '{}' escapes the source directory.", TexturePath));
			}
			std::string Error;
			if (!ReadFileBytes(DependencyPath, MaxImportedImageEncodedBytes, OwnedBytes, Error))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::MissingDependency,
					NormalizedUri, Error);
			}
			uint32 DependencyIndex = 0;
			if (!AppendDependency(Result.Scene, EImportedDependencyRole::Image,
				std::format("image:{}", NormalizedUri),
				MakeDependencySourcePath(RootSourcePath, NormalizedUri), OwnedBytes, &DependencyIndex))
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
					"dependencies", "Imported dependency count exceeds the limit.");
			}
			Imported.StableIdentity = std::format("external:{}", NormalizedUri);
			Imported.SuggestedName = DependencyPath.stem().string();
			Imported.ExternalDependencyIndex = DependencyIndex;
			EncodingHint = NormalizedUri;
			EncodedBytes = OwnedBytes;
		}
		if (!EncodingFromMimeOrPath({}, EncodingHint, Imported.Encoding))
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedEncoding,
				TexturePath, "Material image encoding is unsupported.");
		}
		std::string ImageError;
		if (!ValidateImageBytes(Imported.Encoding, EncodedBytes, ImageError))
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
				TexturePath, ImageError);
		}
		Imported.EncodedByteCount = EncodedBytes.size();
		if (!Imported.ExternalDependencyIndex.has_value())
			Imported.EmbeddedEncodedBytes = std::move(OwnedBytes);
		OutImageIndex = static_cast<uint32>(Result.Scene.Images.size());
		ImageIndices.emplace(TexturePath, OutImageIndex);
		Result.Scene.Images.push_back(std::move(Imported));
		return true;
	}

	auto AddAssimpShadingDiagnostic(
		const aiMaterial& SourceMaterial,
		uint32 MaterialIndex,
		FSceneDecodeResult& Result) -> bool
	{
		int ShadingModel = aiShadingMode_NoShading;
		const bool bHasShadingModel =
			SourceMaterial.Get(AI_MATKEY_SHADING_MODEL, ShadingModel) == aiReturn_SUCCESS;
		const bool bPhong =
			bHasShadingModel
			&& (ShadingModel == aiShadingMode_Phong || ShadingModel == aiShadingMode_Blinn);
		const bool bUnknownShading =
			bHasShadingModel && ShadingModel == aiShadingMode_NoShading;
		const bool bHasUnsupportedTextures =
			SourceMaterial.GetTextureCount(aiTextureType_SPECULAR) > 0
			|| SourceMaterial.GetTextureCount(aiTextureType_NORMALS) > 0
			|| SourceMaterial.GetTextureCount(aiTextureType_LIGHTMAP) > 0
			|| SourceMaterial.GetTextureCount(aiTextureType_EMISSIVE) > 0
			|| SourceMaterial.GetTextureCount(aiTextureType_OPACITY) > 0
			|| SourceMaterial.GetTextureCount(aiTextureType_METALNESS) > 0
			|| SourceMaterial.GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS) > 0;
		if (!bPhong && !bUnknownShading && !bHasUnsupportedTextures) return true;
		if (AddDiagnostic(
			Result.Scene,
			EImportDiagnosticSeverity::Warning,
			ESceneImportDiagnosticCategory::UnsupportedMaterialProperty,
			std::format("material:{}", MaterialIndex),
			bPhong ? "Phong" : "unmapped-material-properties",
			bPhong
				? "Phong/specular properties are not converted to metallic/roughness values."
				: "Structured source material properties outside the supported subset use the default fallback."))
		{
			return true;
		}
		return FailImport(
			Result,
			ESceneImportDiagnosticCategory::ResourceLimitExceeded,
			"diagnostics",
			"Import diagnostic limit exceeded.");
	}

	auto ImportAssimpMaterials(
		const aiScene& Scene,
		const std::filesystem::path& RootPath,
		std::string_view RootSourcePath,
		FSceneDecodeResult& Result) -> bool
	{
		if (Scene.mNumMaterials > MaxImportedSourceMaterials)
		{
			return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
				"materials", "Source material count exceeds the limit.");
		}
		for (uint32 MaterialIndex = 0; MaterialIndex < Scene.mNumMaterials; ++MaterialIndex)
		{
			if (CheckSceneDecodeCancellation(Result, "materials")) return false;
			const aiMaterial* SourceMaterial = Scene.mMaterials[MaterialIndex];
			if (SourceMaterial == nullptr)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
					std::format("material:{}", MaterialIndex), "Assimp returned a null material.");
			}
			if (!AddAssimpShadingDiagnostic(*SourceMaterial, MaterialIndex, Result)) return false;
		}
		std::unordered_map<std::string, uint32> ImageIndices;
		uint64 EmbeddedByteCount = 0;
		for (uint32 TextureIndex = 0; TextureIndex < Scene.mNumTextures; ++TextureIndex)
		{
			if (CheckSceneDecodeCancellation(Result, "images")) return false;
			uint32 ImportedImageIndex = 0;
			if (!ImportAssimpImage(Scene, RootPath, RootSourcePath,
				std::format("*{}", TextureIndex), Result, ImageIndices,
				EmbeddedByteCount, ImportedImageIndex)) return false;
		}
		Result.Scene.Materials.reserve(Scene.mNumMaterials);
		for (uint32 MaterialIndex = 0; MaterialIndex < Scene.mNumMaterials; ++MaterialIndex)
		{
			if (CheckSceneDecodeCancellation(Result, "materials")) return false;
			const aiMaterial* SourceMaterial = Scene.mMaterials[MaterialIndex];
			if (SourceMaterial == nullptr)
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
					std::format("material:{}", MaterialIndex), "Assimp returned a null material.");
			}
			FImportedMaterial Material;
			Material.SourceMaterialIndex = MaterialIndex;
			aiString Name;
			SourceMaterial->Get(AI_MATKEY_NAME, Name);
			Material.SourceName = Name.C_Str();

			aiColor4D Diffuse(1.0f, 1.0f, 1.0f, 1.0f);
			if (SourceMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, Diffuse) == aiReturn_SUCCESS)
			{
				if (!std::isfinite(Diffuse.r) || !std::isfinite(Diffuse.g) ||
					!std::isfinite(Diffuse.b) || !std::isfinite(Diffuse.a))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
						std::format("material:{}", MaterialIndex), "Material diffuse color is non-finite.");
				}
				Material.BaseColorFactor = {Diffuse.r, Diffuse.g, Diffuse.b, Diffuse.a};
			}
			float Opacity = 1.0f;
			if (SourceMaterial->Get(AI_MATKEY_OPACITY, Opacity) == aiReturn_SUCCESS)
			{
				if (!std::isfinite(Opacity))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidValue,
						std::format("material:{}", MaterialIndex), "Material opacity is non-finite.");
				}
				Material.BaseColorFactor.a *= Opacity;
			}

			aiTextureType TextureType = aiTextureType_NONE;
			if (SourceMaterial->GetTextureCount(aiTextureType_BASE_COLOR) == 1)
				TextureType = aiTextureType_BASE_COLOR;
			else if (SourceMaterial->GetTextureCount(aiTextureType_DIFFUSE) == 1)
				TextureType = aiTextureType_DIFFUSE;
			else if (SourceMaterial->GetTextureCount(aiTextureType_BASE_COLOR) > 1 ||
				SourceMaterial->GetTextureCount(aiTextureType_DIFFUSE) > 1)
			{
				if (!AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
					ESceneImportDiagnosticCategory::UnsupportedMaterialProperty,
					std::format("material:{}", MaterialIndex), "layered-diffuse-texture",
					"Layered diffuse textures are not mapped to base color."))
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
						"diagnostics", "Import diagnostic limit exceeded.");
				}
			}
			if (TextureType != aiTextureType_NONE)
			{
				aiString TexturePath;
				unsigned int UVChannel = 0;
				if (SourceMaterial->GetTexture(TextureType, 0, &TexturePath, nullptr, &UVChannel) != aiReturn_SUCCESS)
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
						std::format("material:{}", MaterialIndex), "Assimp material texture reference is invalid.");
				}
				if (UVChannel >= MaxImportedUVChannels)
				{
					return FailImport(Result, ESceneImportDiagnosticCategory::InvalidReference,
						TexturePath.C_Str(), "Material texture UV channel exceeds the imported UV limit.");
				}
				FImportedTextureBinding Binding;
				Binding.Semantic = EImportedTextureSemantic::BaseColor;
				Binding.UVChannel = UVChannel;
				if (!ImportAssimpImage(Scene, RootPath, RootSourcePath,
					TexturePath.C_Str(), Result, ImageIndices,
					EmbeddedByteCount, Binding.ImageIndex))
					return false;
				Material.TextureBindings.push_back(Binding);
			}
			Result.Scene.Materials.push_back(std::move(Material));
		}
		return true;
	}

	auto ImportAssimpFormat(
		const aiScene& Scene,
		const FImportedSceneContext& Context) -> bool
	{
		return ImportAssimpMaterials(
			Scene,
			Context.RootPath,
			Context.RootSourcePath,
			Context.Result);
	}
}
