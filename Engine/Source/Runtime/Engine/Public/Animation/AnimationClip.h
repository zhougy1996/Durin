#pragma once

#include "Asset/Cook.h"
#include "Asset/BulkData.h"
#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "DObject/ObjectPtr.h"
#include "SkeletalMesh/Skeleton.h"

#include "AnimationClip.gen.h"

namespace Durin
{
	struct FSkeletalPayloadSerializationContext;

	inline constexpr uint32 MaximumAnimationClipTracks = 65536;
	inline constexpr uint32 MaximumAnimationKeysPerTrack = 16777216;
	inline constexpr uint32 MaximumAnimationKeysPerClip = 100000000;
	inline constexpr uint64 MaximumAnimationClipPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumAnimationClipImportedDataBytes = 1073700000ull;
	inline constexpr uint32 AnimationClipImportedDataSchemaVersion = 1;
	extern ENGINE_API const FGuid AnimationClipImportedDataPayloadId;

	DENUM()
	enum class EAnimationTrackPath : uint8
	{
		Translation,
		Rotation,
		Scale
	};

	DENUM()
	enum class EAnimationInterpolation : uint8
	{
		Step,
		Linear
	};

	DSTRUCT()
	struct FAnimationClipSummary
	{
		GENERATED_BODY()

		DPROPERTY()
		float DurationSeconds = 0.0f;

		DPROPERTY()
		uint32 TrackCount = 0;

		DPROPERTY()
		uint32 KeyCount = 0;

		auto operator==(const FAnimationClipSummary&) const -> bool = default;
	};

	struct FAnimationTrackData
	{
		uint16 BoneIndex = 0;
		EAnimationTrackPath Path = EAnimationTrackPath::Translation;
		EAnimationInterpolation Interpolation = EAnimationInterpolation::Linear;
		std::vector<float> Times;
		std::vector<FVector3f> VectorValues;
		std::vector<FVector4f> RotationValues;

		auto operator==(const FAnimationTrackData&) const -> bool = default;
	};

	// Detached immutable track storage; mutable playback state belongs to a later milestone.
	struct FAnimationClipPayloadData
	{
		float DurationSeconds = 0.0f;
		std::vector<FAnimationTrackData> Tracks;

		ENGINE_API auto Serialize(
			FArchive& Ar,
			const FSkeletalPayloadSerializationContext& Context) -> void;

		auto operator==(const FAnimationClipPayloadData&) const -> bool = default;
	};

	// Owns the canonical tracks and typed keys required to rebuild this clip.
	DSTRUCT()
	struct FAnimationClipImportedData
	{
		GENERATED_BODY()

		DPROPERTY()
		FEditorBulkData Tracks;

		DPROPERTY()
		uint32 SchemaVersion = AnimationClipImportedDataSchemaVersion;

		ENGINE_API auto Capture(
			const FAnimationClipPayloadData& Payload,
			uint32 SkeletonBoneCount,
			std::string& OutError) -> bool;
		ENGINE_API auto Decode(
			uint32 SkeletonBoneCount,
			std::string& OutError) const -> FAnimationClipPayloadData;
		ENGINE_API auto IsValid(uint32 SkeletonBoneCount) const -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
	};

	// Validated authored relationships and detached values for owner-thread replacement.
	struct FAnimationClipAssetData
	{
		DSkeleton* Skeleton = nullptr;
		// Optional prospective state for failure-atomic multi-asset publication.
		DSkeleton* ValidationSkeleton = nullptr;
		std::string SkeletonCompatibilityIdentity;
		FName ClipName;
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
		// Preserve lazy authored storage on PostLoad; otherwise capture from Payload.
		std::optional<FAnimationClipImportedData> ImportedData;
	};

	ENGINE_API auto ValidateAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		std::string& OutError) -> bool;
	ENGINE_API auto ValidateAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		uint32 SkeletonBoneCount,
		std::string& OutError) -> bool;

	DCLASS()
	class DAnimationClip : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DAnimationClip(const FObjectInitializer& ObjectInitializer);

		auto GetSkeleton() const -> DSkeleton* { return Skeleton.Get(); }
		auto GetSkeletonCompatibilityIdentity() const -> const std::string& { return SkeletonCompatibilityIdentity; }
		auto GetClipName() const -> FName { return ClipName; }
		auto GetSummary() const -> const FAnimationClipSummary& { return Summary; }
		ENGINE_API auto GetPayloadData() const -> std::shared_ptr<const FAnimationClipPayloadData>;
		auto GetCookedPlatformData() const -> const FBulkData& { return CookedPlatformData; }
		auto GetImportedData() const -> const FAnimationClipImportedData& { return ImportedData; }

		// Validates before replacement; does not retain operation history or dirty the package.
		ENGINE_API auto SetAssetData(
			FAnimationClipAssetData Candidate,
			std::string& OutError) -> bool;
		ENGINE_API auto Validate(std::string& OutError) const -> bool;
		ENGINE_API auto ValidateAgainstSkeleton(
			const DSkeleton& ProspectiveSkeleton,
			std::string& OutError) const -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;

		DPROPERTY()
		TObjectPtr<DSkeleton> Skeleton;

		DPROPERTY()
		std::string SkeletonCompatibilityIdentity;

		DPROPERTY()
		FName ClipName;

		DPROPERTY()
		FAnimationClipSummary Summary;

		FBulkData CookedPlatformData;

		DPROPERTY(EditorOnly)
		FAnimationClipImportedData ImportedData;

		std::shared_ptr<const FAnimationClipPayloadData> PayloadData;

		auto LoadCookedPayload(std::string& OutError) -> bool;
	};
}
