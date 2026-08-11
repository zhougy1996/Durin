#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "PayloadDecodeResult.h"
#include "Animation/AnimationClip.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin
{
	inline constexpr uint32 SkeletalMeshPayloadMagic = 0x4D4B5344; // DSKM
	inline constexpr uint32 AnimationClipPayloadMagic = 0x4D4E4144; // DANM
	inline constexpr uint32 SkeletalMeshPayloadSchemaVersion = 1;
	inline constexpr uint32 AnimationClipPayloadSchemaVersion = 1;
	inline constexpr uint32 SkeletalMeshBuilderVersion = 1;
	inline constexpr uint32 AnimationClipBuilderVersion = 1;
	inline constexpr uint32 SkeletalPayloadKeySchemaVersion = 1;
	inline constexpr uint32 SkeletalPayloadAlignment = 16;
	inline constexpr uint32 SkeletalPayloadHeaderSize = 64;
	inline constexpr uint32 SkeletalPayloadChunkEntrySize = 32;
	inline constexpr uint32 MaximumSkeletalPayloadChunks = 64;
	inline constexpr uint32 MaximumSkeletalPayloadNameBytes = 1024;
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

	struct FSkeletalDerivedDataKeyInput
	{
		std::string ProviderIdentity;
		uint32 ProviderVersion = 0;
		FXxHash128 SourceClosureHash;
		FXxHash128 SettingsHash;
		FXxHash128 ProviderStateHash;
		FXxHash128 PayloadInputFingerprint;
		std::string StableOutputIdentity;
		std::string SkeletonCompatibilityIdentity;
		ESkeletalPayloadTargetPlatform TargetPlatform = ESkeletalPayloadTargetPlatform::Unknown;
		ESkeletalPayloadTargetProfile TargetProfile = ESkeletalPayloadTargetProfile::Unknown;
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
		friend auto ReportMissingSkeletalDerivedDataAsset(DObject*) -> void;
	};

	ENGINE_API auto IsSkeletalDerivedDataRepairLoadActive() -> bool;

	ENGINE_API auto BuildSkeletalMeshDerivedDataKeyBytes(
		const FSkeletalDerivedDataKeyInput& Input) -> std::vector<uint8>;
	ENGINE_API auto BuildSkeletalMeshDerivedDataKey(
		const FSkeletalDerivedDataKeyInput& Input) -> std::string;
	ENGINE_API auto BuildAnimationClipDerivedDataKeyBytes(
		const FSkeletalDerivedDataKeyInput& Input) -> std::vector<uint8>;
	ENGINE_API auto BuildAnimationClipDerivedDataKey(
		const FSkeletalDerivedDataKeyInput& Input) -> std::string;

	ENGINE_API auto EncodeSkeletalMeshPayload(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		ESkeletalPayloadTargetPlatform TargetPlatform,
		ESkeletalPayloadTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	ENGINE_API auto DecodeSkeletalMeshPayload(
		std::span<const uint8> Bytes,
		const DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		ESkeletalPayloadTargetPlatform ExpectedPlatform,
		ESkeletalPayloadTargetProfile ExpectedProfile,
		FSkeletalMeshPayloadData& OutPayload) -> FPayloadDecodeResult;

	ENGINE_API auto EncodeAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		ESkeletalPayloadTargetPlatform TargetPlatform,
		ESkeletalPayloadTargetProfile TargetProfile,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool;
	ENGINE_API auto DecodeAnimationClipPayload(
		std::span<const uint8> Bytes,
		const DSkeleton& Skeleton,
		ESkeletalPayloadTargetPlatform ExpectedPlatform,
		ESkeletalPayloadTargetProfile ExpectedProfile,
		FAnimationClipPayloadData& OutPayload) -> FPayloadDecodeResult;

	ENGINE_API auto ComputeSkeletalMeshPayloadInputFingerprint(
		const FSkeletalMeshPayloadData& Payload,
		const DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		FXxHash128& OutFingerprint,
		std::string& OutError) -> bool;
	ENGINE_API auto ComputeAnimationClipPayloadInputFingerprint(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		FXxHash128& OutFingerprint,
		std::string& OutError) -> bool;

	ENGINE_API auto LoadSkeletalMeshDerivedData(
		std::string_view Key,
		std::vector<uint8>& OutBytes,
		std::string& OutMessage) -> bool;
	ENGINE_API auto StoreSkeletalMeshDerivedData(
		std::string_view Key,
		std::span<const uint8> Bytes,
		std::string& OutError) -> bool;
	ENGINE_API auto LoadAnimationClipDerivedData(
		std::string_view Key,
		std::vector<uint8>& OutBytes,
		std::string& OutMessage) -> bool;
	ENGINE_API auto StoreAnimationClipDerivedData(
		std::string_view Key,
		std::span<const uint8> Bytes,
		std::string& OutError) -> bool;
}
