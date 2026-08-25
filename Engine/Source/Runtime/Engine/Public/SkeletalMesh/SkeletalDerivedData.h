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

	// Records a load-time DDC failure for every active repair scope.
	ENGINE_API auto ReportMissingSkeletalDerivedDataAsset(DObject* Asset) -> void;

	// Allows an authoritative reimport transaction to materialize authored skeletal
	// metadata and identify payloads whose disposable DDC objects must be rebuilt.
	class ENGINE_API FScopedSkeletalDerivedDataRepairLoad
	{
	public:
		FScopedSkeletalDerivedDataRepairLoad();
		~FScopedSkeletalDerivedDataRepairLoad();
		FScopedSkeletalDerivedDataRepairLoad(
			const FScopedSkeletalDerivedDataRepairLoad&) = delete;
		auto operator=(const FScopedSkeletalDerivedDataRepairLoad&)
			-> FScopedSkeletalDerivedDataRepairLoad& = delete;

		// Returns skeletal assets whose disposable payload could not be loaded while
		// this scope was active. The pointers remain owned by their packages.
		auto GetMissingAssets() const -> std::span<DObject* const> { return MissingAssets; }

	private:
		std::vector<DObject*> MissingAssets;
		friend ENGINE_API auto ReportMissingSkeletalDerivedDataAsset(DObject*) -> void;
	};

	ENGINE_API auto IsSkeletalDerivedDataRepairLoadActive() -> bool;

}
