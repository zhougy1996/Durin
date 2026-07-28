#pragma once

#include "Asset/SourcePath.h"
#include "AssetImportAPI.h"
#include "CoreFwd.h"
#include "Hash/XxHash.h"
#include "Math/MathFwd.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace Durin::Asset
{
	inline constexpr uint32 MaxImportedUVChannels = 4;
	inline constexpr uint32 StaticModelImporterVersion = 3;
	inline constexpr uint32 MaxImportedSourceMaterials = 4096;
	inline constexpr uint32 MaxImportedImages = 4096;
	inline constexpr uint32 MaxImportedTextureBindingsPerMaterial = 16;
	inline constexpr uint32 MaxImportedDependencies = 8192;
	inline constexpr uint32 MaxImportDiagnostics = 4096;
	inline constexpr uint64 MaxImportedImageEncodedBytes = 512ull * 1024ull * 1024ull;
	inline constexpr uint64 MaxImportedEmbeddedImageBytes = 2ull * 1024ull * 1024ull * 1024ull;
	inline constexpr uint32 MaxImportedTextureDimension = 16384;
	inline constexpr uint64 MaxImportedDecodedPixels = 268435456;
	inline constexpr uint64 MaxImportedSourceModelBytes = 2ull * 1024ull * 1024ull * 1024ull;

	enum class EImportedImageEncoding : uint8 { Png, Jpeg, Bmp, Tga };
	enum class EImportedTextureSemantic : uint8
	{
		BaseColor, MetallicRoughness, Normal, Occlusion, Emissive
	};
	enum class EImportedSamplerFilter : uint8
	{
		Nearest, Linear, NearestMipmapNearest, LinearMipmapNearest,
		NearestMipmapLinear, LinearMipmapLinear
	};
	enum class EImportedSamplerWrap : uint8 { Repeat, MirroredRepeat, ClampToEdge };
	enum class EImportedAlphaMode : uint8 { Opaque, Mask, Blend };
	enum class EImportedDependencyRole : uint8 { RootModel, GeometryBuffer, Image };
	enum class EImportDiagnosticSeverity : uint8 { Warning, Error };
	enum class EImportDiagnosticCategory : uint8
	{
		UnsupportedRequiredExtension,
		UnsupportedOptionalExtension,
		UnsupportedMaterialProperty,
		LossyMaterialMapping,
		UnsupportedSampler,
		UnsupportedAlphaMode,
		MissingDependency,
		UnsafeDependencyPath,
		InvalidReference,
		InvalidValue,
		UnsupportedEncoding,
		ResourceLimitExceeded
	};

	struct FImportedSampler
	{
		EImportedSamplerFilter MinFilter = EImportedSamplerFilter::LinearMipmapLinear;
		EImportedSamplerFilter MagFilter = EImportedSamplerFilter::Linear;
		EImportedSamplerWrap WrapU = EImportedSamplerWrap::Repeat;
		EImportedSamplerWrap WrapV = EImportedSamplerWrap::Repeat;
	};

	struct FImportedTextureBinding
	{
		EImportedTextureSemantic Semantic = EImportedTextureSemantic::BaseColor;
		uint32 ImageIndex = 0;
		uint32 UVChannel = 0;
		FVector2f Offset{0.0f};
		FVector2f Scale{1.0f};
		float RotationRadians = 0.0f;
		FImportedSampler Sampler;
		float Strength = 1.0f;
	};

	struct FImportedMaterial
	{
		uint32 SourceMaterialIndex = 0;
		std::string SourceName;
		FVector4f BaseColorFactor{1.0f};
		float MetallicFactor = 1.0f;
		float RoughnessFactor = 1.0f;
		FVector3f EmissiveFactor{0.0f};
		EImportedAlphaMode AlphaMode = EImportedAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool bDoubleSided = false;
		std::vector<FImportedTextureBinding> TextureBindings;
	};

	struct FImportedImage
	{
		std::string StableIdentity;
		std::string SuggestedName;
		EImportedImageEncoding Encoding = EImportedImageEncoding::Png;
		uint64 EncodedByteCount = 0;
		std::optional<uint32> ExternalDependencyIndex;
		std::vector<uint8> EmbeddedEncodedBytes;
	};

	struct FImportedDependency
	{
		EImportedDependencyRole Role = EImportedDependencyRole::RootModel;
		std::string StableIdentity;
		FSourcePath Source;
		FXxHash128 ContentHash;
		uint64 ByteCount = 0;
	};

	struct FImportDiagnostic
	{
		EImportDiagnosticSeverity Severity = EImportDiagnosticSeverity::Warning;
		EImportDiagnosticCategory Category = EImportDiagnosticCategory::InvalidValue;
		std::string SourceIdentity;
		std::string Subject;
		std::string Message;
	};

	struct FMeshImportOptions
	{
		glm::mat4 SourceToEngine{1.0f};
		FSourcePath RootSource;
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
		uint32 SourceMaterialIndex = 0;
	};

	struct FImportedSceneData
	{
		std::vector<FImportedImage> Images;
		std::vector<FImportedMaterial> Materials;
		std::vector<FImportedMaterialSlot> MaterialSlots;
		std::vector<FImportedMeshData> Meshes;
		std::vector<FImportedDependency> Dependencies;
		std::vector<FImportDiagnostic> Diagnostics;
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
