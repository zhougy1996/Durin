#include "Animation/AnimationClip.h"

#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

		auto IsCanonicalQuaternion(const FVector4f& Value) -> bool
		{
			if (!Math::IsFinite(Value)
				|| std::abs(Math::LengthSquared(Value) - 1.0f) > 1.0e-5f) return false;
			for (const float Component : {Value.w, Value.z, Value.y, Value.x})
			{
				if (Component > 0.0f) return true;
				if (Component < 0.0f) return false;
			}
			return false;
		}

		auto IsUnitQuaternion(const FVector4f& Value) -> bool
		{
			return Math::IsFinite(Value)
				&& std::abs(Math::LengthSquared(Value) - 1.0f) <= 1.0e-5f;
		}

		auto AddPayloadBytes(uint64 Count, uint64 ElementSize, uint64& Total) -> bool
		{
			if (Count > MaximumAnimationClipPayloadBytes / ElementSize) return false;
			const uint64 Bytes = Count * ElementSize;
			if (Total > MaximumAnimationClipPayloadBytes - Bytes) return false;
			Total += Bytes;
			return true;
		}
	}

	auto ValidateAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		std::string& OutError) -> bool
	{
		if (!std::isfinite(Payload.DurationSeconds) || Payload.DurationSeconds < 0.0f)
			return Fail(OutError, "Animation payload duration is invalid.");
		if (Payload.Tracks.empty() || Payload.Tracks.size() > MaximumAnimationClipTracks)
			return Fail(OutError, "Animation payload track count is outside the supported range.");

		std::set<std::pair<uint16, EAnimationTrackPath>> TrackIdentities;
		uint64 TotalKeys = 0;
		uint64 PayloadBytes = 0;
		if (!AddPayloadBytes(Payload.Tracks.size(), sizeof(FAnimationTrackData), PayloadBytes))
			return Fail(OutError, "Animation payload exceeds the supported byte limit.");
		for (const FAnimationTrackData& Track : Payload.Tracks)
		{
			if ((Track.Path != EAnimationTrackPath::Translation
					&& Track.Path != EAnimationTrackPath::Rotation
					&& Track.Path != EAnimationTrackPath::Scale)
				|| Track.BoneIndex >= Skeleton.GetBoneCount()
				|| !TrackIdentities.emplace(Track.BoneIndex, Track.Path).second)
				return Fail(OutError, "Animation payload contains an invalid or duplicate track target.");
			if (Track.Interpolation != EAnimationInterpolation::Step
				&& Track.Interpolation != EAnimationInterpolation::Linear)
				return Fail(OutError, "Animation payload interpolation is unsupported.");
			if (Track.Times.empty() || Track.Times.size() > MaximumAnimationKeysPerTrack
				|| TotalKeys > MaximumAnimationKeysPerClip - Track.Times.size())
				return Fail(OutError, "Animation payload key count is outside the supported range.");
			TotalKeys += Track.Times.size();
			if (!AddPayloadBytes(Track.Times.size(), sizeof(float), PayloadBytes)
				|| !AddPayloadBytes(Track.VectorValues.size(), sizeof(FVector3f), PayloadBytes)
				|| !AddPayloadBytes(Track.RotationValues.size(), sizeof(FVector4f), PayloadBytes))
				return Fail(OutError, "Animation payload exceeds the supported byte limit.");
			for (size_t KeyIndex = 0; KeyIndex < Track.Times.size(); ++KeyIndex)
			{
				const float Time = Track.Times[KeyIndex];
				if (!std::isfinite(Time) || Time < 0.0f || Time > Payload.DurationSeconds
					|| (KeyIndex > 0 && Time <= Track.Times[KeyIndex - 1]))
					return Fail(OutError, "Animation payload key times must be finite, bounded, and strictly increasing.");
			}

			if (Track.Path == EAnimationTrackPath::Rotation)
			{
				if (!Track.VectorValues.empty() || Track.RotationValues.size() != Track.Times.size())
					return Fail(OutError, "Animation rotation track value count is invalid.");
				for (size_t KeyIndex = 0; KeyIndex < Track.RotationValues.size(); ++KeyIndex)
				{
					const FVector4f& Value = Track.RotationValues[KeyIndex];
					if (!IsUnitQuaternion(Value))
						return Fail(OutError, "Animation rotation track contains an invalid quaternion.");
					if (KeyIndex == 0 && !IsCanonicalQuaternion(Value))
						return Fail(OutError, "Animation rotation track has a non-canonical first quaternion.");
					if (KeyIndex > 0)
					{
						const float Dot = Math::Dot(Track.RotationValues[KeyIndex - 1], Value);
						if (Dot < 0.0f || (Dot == 0.0f && !IsCanonicalQuaternion(Value)))
							return Fail(OutError, "Animation rotation track has discontinuous quaternion signs.");
					}
				}
			}
			else
			{
				if (!Track.RotationValues.empty() || Track.VectorValues.size() != Track.Times.size())
					return Fail(OutError, "Animation vector track value count is invalid.");
				if (std::ranges::any_of(Track.VectorValues, [](const FVector3f& Value) {
					return !Math::IsFinite(Value);
				})) return Fail(OutError, "Animation vector track contains a non-finite value.");
				if (Track.Path == EAnimationTrackPath::Scale
					&& std::ranges::any_of(Track.VectorValues, [](const FVector3f& Value) {
						return std::abs(Value.x) <= 1.0e-8f || std::abs(Value.y) <= 1.0e-8f
							|| std::abs(Value.z) <= 1.0e-8f;
					})) return Fail(OutError, "Animation scale track contains a singular value.");
			}
		}
		OutError.clear();
		return true;
	}

	DAnimationClip::DAnimationClip(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	auto DAnimationClip::InitializeFromImportedData(
		FAnimationClipImportedData InData,
		std::string& OutError) -> bool
	{
		if (!InData.Skeleton || !InData.Payload || InData.ClipName.IsNone())
			return Fail(OutError, "Animation imported data requires a Skeleton, payload, and clip name.");
		if (InData.SkeletonCompatibilityIdentity != InData.Skeleton->GetCompatibilityIdentity())
			return Fail(OutError, "Animation imported data is incompatible with its Skeleton.");
		if (!ValidateAnimationClipPayload(*InData.Payload, *InData.Skeleton, OutError)) return false;
		uint64 KeyCount = 0;
		for (const FAnimationTrackData& Track : InData.Payload->Tracks) KeyCount += Track.Times.size();

		Skeleton = InData.Skeleton;
		SkeletonCompatibilityIdentity = std::move(InData.SkeletonCompatibilityIdentity);
		ClipName = InData.ClipName;
		Summary = {
			.DurationSeconds = InData.Payload->DurationSeconds,
			.TrackCount = static_cast<uint32>(InData.Payload->Tracks.size()),
			.KeyCount = static_cast<uint32>(KeyCount)};
		CookedPayload = InData.CookedPayload;
		PayloadData = std::move(InData.Payload);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DAnimationClip::Validate(std::string& OutError) const -> bool
	{
		if (!Skeleton)
			return Fail(OutError, "AnimationClip has no Skeleton reference.");
		if (!Skeleton->Validate(OutError))
		{
			OutError = std::format("AnimationClip references an invalid Skeleton: {}", OutError);
			return false;
		}
		if (SkeletonCompatibilityIdentity != Skeleton->GetCompatibilityIdentity())
			return Fail(OutError, "AnimationClip compatibility identity does not match its Skeleton.");
		if (ClipName.IsNone() || !std::isfinite(Summary.DurationSeconds) || Summary.DurationSeconds < 0.0f
			|| Summary.TrackCount == 0 || Summary.TrackCount > MaximumAnimationClipTracks
			|| Summary.KeyCount == 0 || Summary.KeyCount > MaximumAnimationKeysPerClip)
			return Fail(OutError, "AnimationClip authored summary or name is invalid.");
		if (PayloadData)
		{
			if (!ValidateAnimationClipPayload(*PayloadData, *Skeleton, OutError)) return false;
			uint64 KeyCount = 0;
			for (const FAnimationTrackData& Track : PayloadData->Tracks) KeyCount += Track.Times.size();
			if (PayloadData->DurationSeconds != Summary.DurationSeconds
				|| PayloadData->Tracks.size() != Summary.TrackCount || KeyCount != Summary.KeyCount)
				return Fail(OutError, "AnimationClip payload does not match its authored summary.");
		}
		OutError.clear();
		return true;
	}

	auto DAnimationClip::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (Validate(OutError)) return true;
		OutError = std::format("{}: {}", GetName(), OutError);
		return false;
	}

	auto DAnimationClip::PrepareImportedStateExchange(
		DAnimationClip& Candidate,
		std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>
	{
		if (&Candidate == this)
			return Fail(OutError, "AnimationClip imported-state exchange requires distinct assets."), nullptr;
		if (!Validate(OutError))
		{
			OutError = std::format("Target AnimationClip is invalid: {}", OutError);
			return nullptr;
		}
		if (!Candidate.Validate(OutError))
		{
			OutError = std::format("Candidate AnimationClip is invalid: {}", OutError);
			return nullptr;
		}
		OutError.clear();
		return std::unique_ptr<FAnimationClipImportedStateExchange>(
			new FAnimationClipImportedStateExchange(*this, Candidate));
	}

	FAnimationClipImportedStateExchange::FAnimationClipImportedStateExchange(
		DAnimationClip& InTarget,
		DAnimationClip& InCandidate)
		: Target(&InTarget), Candidate(&InCandidate) {}

	FAnimationClipImportedStateExchange::~FAnimationClipImportedStateExchange() = default;

	auto FAnimationClipImportedStateExchange::Swap() noexcept -> void
	{
		check(Target && Candidate && Target != Candidate);
		std::swap(Target->Skeleton, Candidate->Skeleton);
		std::swap(Target->SkeletonCompatibilityIdentity, Candidate->SkeletonCompatibilityIdentity);
		std::swap(Target->ClipName, Candidate->ClipName);
		std::swap(Target->Summary, Candidate->Summary);
		std::swap(Target->CookedPayload, Candidate->CookedPayload);
		std::swap(Target->PayloadData, Candidate->PayloadData);
		Target->MarkPackageDirty();
	}

	auto FAnimationClipImportedStateExchange::Commit() noexcept -> void
	{
		if (bCommitted) return;
		Swap();
		bCommitted = true;
	}

	auto FAnimationClipImportedStateExchange::Reverse() noexcept -> void
	{
		if (!bCommitted) return;
		Swap();
		bCommitted = false;
	}

	auto FAnimationClipImportedStateExchange::Finalize() noexcept -> void
	{
		Target = nullptr;
		Candidate = nullptr;
	}
}
