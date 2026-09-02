#pragma once

#include "EngineAPI.h"
#include "Animation/AnimationClip.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin
{
	inline constexpr uint32 SkeletalMeshPayloadSchemaVersion = 2;
	inline constexpr uint32 AnimationClipPayloadSchemaVersion = 2;
	// Retained in the version-1 wire header for byte compatibility; it is not a
	// runtime compatibility gate for future Build recipe revisions.
	inline constexpr uint32 SkeletalMeshPayloadProducerVersion = 2;
	inline constexpr uint32 AnimationClipPayloadProducerVersion = 2;
	inline constexpr uint32 SkeletalPayloadAlignment = 16;
	inline constexpr uint32 SkeletalPayloadHeaderSize = 64;
	inline constexpr uint32 SkeletalPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumSkeletalPayloadChunks = 64;
	inline constexpr uint32 MaximumSkeletalPayloadNameBytes = 1024;
	inline constexpr uint32 SkeletalMeshBuilderVersion = 1;
	inline constexpr uint32 AnimationClipBuilderVersion = 1;
	inline constexpr std::string_view SkeletalMeshBuilderIdentity =
		"Durin.SkeletalMesh.Builder.V1";
	inline constexpr std::string_view AnimationClipBuilderIdentity =
		"Durin.AnimationClip.Builder.V1";
	inline const FGuid SkeletalMeshPrimaryCookedPayloadId{
		0x716b7891, 0x4ce54f80, 0xa8c6d1b2, 0x173fa049};
	inline const FGuid AnimationClipPrimaryCookedPayloadId{
		0x4a921df3, 0x7d2b46ea, 0xb9180f5c, 0x62d48a31};

	enum class ESkeletalPayloadTargetPlatform : uint32
	{
		Unknown = 0,
		Win64 = 1
	};

	enum class ESkeletalPayloadTargetProfile : uint32
	{
		Unknown = 0,
		Game = 1
	};

	// Supplies stable owner facts needed to validate a detached skeletal payload.
	struct FSkeletalPayloadSerializationContext
	{
		uint32 SkeletonBoneCount = 0;
		uint32 MaterialSlotCount = 0;
		ESkeletalPayloadTargetPlatform TargetPlatform =
			ESkeletalPayloadTargetPlatform::Unknown;
		ESkeletalPayloadTargetProfile TargetProfile =
			ESkeletalPayloadTargetProfile::Unknown;
	};

}
