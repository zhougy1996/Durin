#pragma once

#include "CookedAsset.h"
#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "SkeletalMesh/Skeleton.h"

#include "AnimationClip.gen.h"

namespace Durin
{
	inline constexpr uint32 MaximumAnimationClipTracks = 65536;
	inline constexpr uint32 MaximumAnimationKeysPerTrack = 16777216;
	inline constexpr uint32 MaximumAnimationKeysPerClip = 100000000;
	inline constexpr uint64 MaximumAnimationClipPayloadBytes = 8ull * 1024ull * 1024ull * 1024ull;

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

		auto operator==(const FAnimationClipPayloadData&) const -> bool = default;
	};

	struct FAnimationClipImportedData
	{
		DSkeleton* Skeleton = nullptr;
		// Optional prospective state for failure-atomic multi-asset publication.
		DSkeleton* ValidationSkeleton = nullptr;
		std::string SkeletonCompatibilityIdentity;
		FName ClipName;
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
		Asset::FCookedPayloadDescriptor CookedPayload;
	};

	ENGINE_API auto ValidateAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		std::string& OutError) -> bool;

	class FAnimationClipImportedStateExchange;

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
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		auto GetPayloadData() const -> std::shared_ptr<const FAnimationClipPayloadData> { return PayloadData; }

		ENGINE_API auto InitializeFromImportedData(
			FAnimationClipImportedData InData,
			std::string& OutError) -> bool;
		ENGINE_API auto Validate(std::string& OutError) const -> bool;
		ENGINE_API auto ValidateAgainstSkeleton(
			const DSkeleton& ProspectiveSkeleton,
			std::string& OutError) const -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PrepareImportedStateExchange(
			DAnimationClip& Candidate,
			std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>;
		ENGINE_API auto PrepareImportedStateExchange(
			DAnimationClip& Candidate,
			const DSkeleton& ProspectiveSkeleton,
			std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>;

	private:
		DPROPERTY()
		TObjectPtr<DSkeleton> Skeleton;

		DPROPERTY()
		std::string SkeletonCompatibilityIdentity;

		DPROPERTY()
		FName ClipName;

		DPROPERTY()
		FAnimationClipSummary Summary;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::shared_ptr<const FAnimationClipPayloadData> PayloadData;

		friend class FAnimationClipImportedStateExchange;
	};

	class ENGINE_API FAnimationClipImportedStateExchange
	{
	public:
		~FAnimationClipImportedStateExchange();
		FAnimationClipImportedStateExchange(const FAnimationClipImportedStateExchange&) = delete;
		auto operator=(const FAnimationClipImportedStateExchange&)
			-> FAnimationClipImportedStateExchange& = delete;

		auto Commit() noexcept -> void;
		auto Reverse() noexcept -> void;
		auto Finalize() noexcept -> void;

	private:
		FAnimationClipImportedStateExchange(DAnimationClip& InTarget, DAnimationClip& InCandidate);
		auto Swap() noexcept -> void;

		DAnimationClip* Target = nullptr;
		DAnimationClip* Candidate = nullptr;
		bool bCommitted = false;

		friend class DAnimationClip;
	};
}
