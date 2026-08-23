#pragma once

#include "AssetAuthoring.h"
#include "AssetImportCore.h"
#include "AssetForgeAPI.h"
#include "CoreFwd.h"
#include "Hash/XxHash.h"
#include "Math/Vector.h"
#include "Animation/AnimationClip.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin::Asset::Forge
{
	inline constexpr uint32 MaxImportedUVChannels = 4;
	inline constexpr uint32 ImportedSceneParserVersion = 3;
	inline constexpr uint32 MaxImportedSourceMaterials = 4096;
	inline constexpr uint32 MaxImportedImages = 4096;
	inline constexpr uint32 MaxImportedTextureBindingsPerMaterial = 16;
	inline constexpr uint32 MaxImportedDependencies = 8192;
	inline constexpr uint32 MaxImportDiagnostics = 4096;
	inline constexpr uint64 MaxImportedImageEncodedBytes = 512ull * 1024ull * 1024ull;
	inline constexpr uint64 MaxImportedEmbeddedImageBytes = 2ull * 1024ull * 1024ull * 1024ull;
	inline constexpr uint32 MaxImportedTextureDimension = 16384;
	inline constexpr uint64 MaxImportedDecodedPixels = 268435456;
	inline constexpr uint64 MaxImportedSceneSourceBytes = 2ull * 1024ull * 1024ull * 1024ull;
	inline constexpr uint32 MaxImportedSceneNodes = 1000000;
	inline constexpr uint32 MaxImportedSourceMeshes = 65536;
	inline constexpr uint32 MaxImportedPrimitivesPerMesh = 65536;
	inline constexpr uint32 MaxImportedSkins = 4096;
	inline constexpr uint32 MaxImportedSkeletalMeshes = 16384;
	inline constexpr uint32 MaxImportedAnimations = 4096;
	inline constexpr uint32 MaxImportedAnimationClips = 65536;
	inline constexpr uint64 MaxImportedSkeletalDecodedBytes = 16ull * 1024ull * 1024ull * 1024ull;

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
	enum class EImportedDependencyRole : uint8 { RootScene, GeometryBuffer, Image };
	enum class ESceneImportDiagnosticCategory : uint8
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
		ResourceLimitExceeded,
		UnsupportedFeature,
		MalformedSource,
		LossyNormalization
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
		std::vector<std::byte> EmbeddedEncodedBytes;
	};

	struct FImportedDependency
	{
		EImportedDependencyRole Role = EImportedDependencyRole::RootScene;
		std::string StableIdentity;
		FSourcePath Source;
		FXxHash128 ContentHash;
		uint64 ByteCount = 0;
	};

	struct FSceneImportDiagnostic
	{
		EImportDiagnosticSeverity Severity = EImportDiagnosticSeverity::Warning;
		ESceneImportDiagnosticCategory Category = ESceneImportDiagnosticCategory::InvalidValue;
		std::string SourceIdentity;
		std::string Subject;
		std::string Message;
	};

	struct FMeshImportOptions
	{
		FMatrix4f SourceToEngine{1.0f};
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
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaxImportedUVChannels> UVChannels;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		uint32 SourceMaterialIndex = 0;
	};

	struct FImportedSceneNode
	{
		uint32 SourceNodeIndex = 0;
		int32 ParentNodeIndex = -1;
		std::string SourceName;
		FMatrix4f LocalTransform{1.0f};
		FMatrix4f GlobalTransform{1.0f};
		std::optional<uint32> MeshIndex;
		std::optional<uint32> SkinIndex;
	};

	struct FImportedSkeletonData
	{
		std::string StableIdentity;
		std::string SuggestedName;
		uint32 SourceSkinIndex = 0;
		std::vector<FSkeletonBone> Bones;
		std::string CompatibilityIdentity;
	};

	struct FImportedSkeletalMeshData
	{
		std::string StableIdentity;
		std::string SuggestedName;
		uint32 SourceNodeIndex = 0;
		uint32 SourceMeshIndex = 0;
		uint32 SkeletonIndex = 0;
		FSkeletonTransform MeshNodeBindTransform;
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
	};

	struct FImportedAnimationClipData
	{
		std::string StableIdentity;
		std::string SuggestedName;
		uint32 SourceAnimationIndex = 0;
		uint32 SkeletonIndex = 0;
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
	};

	struct FImportedSceneData
	{
		std::vector<FImportedImage> Images;
		std::vector<FImportedMaterial> Materials;
		std::vector<FImportedMaterialSlot> MaterialSlots;
		std::vector<FImportedMeshData> Meshes;
		std::vector<FImportedSceneNode> Nodes;
		std::vector<FImportedSkeletonData> Skeletons;
		std::vector<FImportedSkeletalMeshData> SkeletalMeshes;
		std::vector<FImportedAnimationClipData> AnimationClips;
		std::vector<FImportedDependency> Dependencies;
		std::vector<FSceneImportDiagnostic> Diagnostics;
	};

	ASSETFORGE_API auto IsSceneSurfaceImageEncodingSupported(
		EImportedImageEncoding Encoding) -> bool;

	ASSETFORGE_API auto ImportFromFile(
		std::string_view FilePath,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options = {}) -> bool;
	// Geometry-only single-asset import from an immutable captured source. The
	// extension hint selects the Assimp decoder without reopening the source.
	ASSETFORGE_API auto ImportGeometryFromMemory(
		std::span<const std::byte> EncodedBytes,
		std::string_view ExtensionHint,
		FImportedSceneData& OutData,
		const FMeshImportOptions& Options = {}) -> bool;
}
