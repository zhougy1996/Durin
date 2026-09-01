#include "Animation/AnimationClip.h"

#include "Asset/AssetCook.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Math/Operations.h"
#include "Serialization/Archive.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"
#include "SkeletalMesh/SkeletalDerivedData.h"

namespace Durin
{
	const FGuid AnimationClipImportedDataPayloadId{
		0x87d4a296, 0x63574b64, 0x92d39c5e, 0x2881e292};

	namespace
	{
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
		uint32 SkeletonBoneCount,
		std::string& OutError) -> bool
	{
		if (!std::isfinite(Payload.DurationSeconds) || Payload.DurationSeconds < 0.0f)
			return Fail("Animation payload duration is invalid.", &OutError);
		if (Payload.Tracks.empty() || Payload.Tracks.size() > MaximumAnimationClipTracks)
			return Fail("Animation payload track count is outside the supported range.", &OutError);

		std::set<std::pair<uint16, EAnimationTrackPath>> TrackIdentities;
		uint64 TotalKeys = 0;
		uint64 PayloadBytes = 0;
		if (!AddPayloadBytes(Payload.Tracks.size(), sizeof(FAnimationTrackData), PayloadBytes))
			return Fail("Animation payload exceeds the supported byte limit.", &OutError);
		for (const FAnimationTrackData& Track : Payload.Tracks)
		{
			if ((Track.Path != EAnimationTrackPath::Translation
					&& Track.Path != EAnimationTrackPath::Rotation
					&& Track.Path != EAnimationTrackPath::Scale)
				|| Track.BoneIndex >= SkeletonBoneCount
				|| !TrackIdentities.emplace(Track.BoneIndex, Track.Path).second)
				return Fail("Animation payload contains an invalid or duplicate track target.", &OutError);
			if (Track.Interpolation != EAnimationInterpolation::Step
				&& Track.Interpolation != EAnimationInterpolation::Linear)
				return Fail("Animation payload interpolation is unsupported.", &OutError);
			if (Track.Times.empty() || Track.Times.size() > MaximumAnimationKeysPerTrack
				|| TotalKeys > MaximumAnimationKeysPerClip - Track.Times.size())
				return Fail("Animation payload key count is outside the supported range.", &OutError);
			TotalKeys += Track.Times.size();
			if (!AddPayloadBytes(Track.Times.size(), sizeof(float), PayloadBytes)
				|| !AddPayloadBytes(Track.VectorValues.size(), sizeof(FVector3f), PayloadBytes)
				|| !AddPayloadBytes(Track.RotationValues.size(), sizeof(FVector4f), PayloadBytes))
				return Fail("Animation payload exceeds the supported byte limit.", &OutError);
			for (size_t KeyIndex = 0; KeyIndex < Track.Times.size(); ++KeyIndex)
			{
				const float Time = Track.Times[KeyIndex];
				if (!std::isfinite(Time) || Time < 0.0f || Time > Payload.DurationSeconds
					|| (KeyIndex > 0 && Time <= Track.Times[KeyIndex - 1]))
					return Fail("Animation payload key times must be finite, bounded, and strictly increasing.", &OutError);
			}

			if (Track.Path == EAnimationTrackPath::Rotation)
			{
				if (!Track.VectorValues.empty() || Track.RotationValues.size() != Track.Times.size())
					return Fail("Animation rotation track value count is invalid.", &OutError);
				for (size_t KeyIndex = 0; KeyIndex < Track.RotationValues.size(); ++KeyIndex)
				{
					const FVector4f& Value = Track.RotationValues[KeyIndex];
					if (!IsUnitQuaternion(Value))
						return Fail("Animation rotation track contains an invalid quaternion.", &OutError);
					if (KeyIndex == 0 && !IsCanonicalQuaternion(Value))
						return Fail("Animation rotation track has a non-canonical first quaternion.", &OutError);
					if (KeyIndex > 0)
					{
						const float Dot = Math::Dot(Track.RotationValues[KeyIndex - 1], Value);
						if (Dot < 0.0f || (Dot == 0.0f && !IsCanonicalQuaternion(Value)))
							return Fail("Animation rotation track has discontinuous quaternion signs.", &OutError);
					}
				}
			}
			else
			{
				if (!Track.RotationValues.empty() || Track.VectorValues.size() != Track.Times.size())
					return Fail("Animation vector track value count is invalid.", &OutError);
				if (std::ranges::any_of(Track.VectorValues, [](const FVector3f& Value) {
					return !Math::IsFinite(Value);
				})) return Fail("Animation vector track contains a non-finite value.", &OutError);
				if (Track.Path == EAnimationTrackPath::Scale
					&& std::ranges::any_of(Track.VectorValues, [](const FVector3f& Value) {
						return std::abs(Value.x) <= 1.0e-8f || std::abs(Value.y) <= 1.0e-8f
							|| std::abs(Value.z) <= 1.0e-8f;
					})) return Fail("Animation scale track contains a singular value.", &OutError);
			}
		}
		OutError.clear();
		return true;
	}

	auto ValidateAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		std::string& OutError) -> bool
	{
		return ValidateAnimationClipPayload(
			Payload, Skeleton.GetBoneCount(), OutError);
	}

	auto FAnimationClipImportedData::Capture(
		const FAnimationClipPayloadData& Payload,
		uint32 SkeletonBoneCount,
		std::string& OutError) -> bool
	{
		if (!ValidateAnimationClipPayload(Payload, SkeletonBoneCount, OutError)) return false;
		FByteArray Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		const_cast<FAnimationClipPayloadData&>(Payload).Serialize(Ar, {
			.SkeletonBoneCount = SkeletonBoneCount,
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError() || Bytes.empty()
			|| Bytes.size() > MaximumAnimationClipImportedDataBytes)
			return Fail(Ar.HasError() ? Ar.GetFailure()->Message
				: "AnimationClip canonical imported data exceeds its authored bound.",
				&OutError);
		if (!Tracks.UpdatePayload(Bytes))
			return Fail("AnimationClip canonical imported data could not be retained.", &OutError);
		SchemaVersion = AnimationClipImportedDataSchemaVersion;
		OutError.clear();
		return true;
	}

	auto FAnimationClipImportedData::Decode(
		uint32 SkeletonBoneCount,
		std::string& OutError) const -> FAnimationClipPayloadData
	{
		FAnimationClipPayloadData Result;
		const FPackageResourceReadResult Payload = Tracks.GetPayload().Wait();
		const std::span<const std::byte> Bytes = Payload.Buffer.GetBytes();
		if (SchemaVersion != AnimationClipImportedDataSchemaVersion
			|| !Payload || Bytes.empty()
			|| Bytes.size() > MaximumAnimationClipImportedDataBytes)
		{
			OutError = "AnimationClip canonical imported-data header is missing or invalid.";
			return Result;
		}
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::BulkData);
		Result.Serialize(Ar, {
			.SkeletonBoneCount = SkeletonBoneCount,
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar)
			|| !ValidateAnimationClipPayload(Result, SkeletonBoneCount, OutError))
		{
			if (OutError.empty()) OutError = Ar.HasError()
				? Ar.GetFailure()->Message
				: "AnimationClip canonical imported data is invalid.";
			return {};
		}
		OutError.clear();
		return Result;
	}

	auto FAnimationClipImportedData::IsValid(uint32 SkeletonBoneCount) const -> bool
	{
		(void)SkeletonBoneCount;
		return SchemaVersion == AnimationClipImportedDataSchemaVersion
			&& Tracks.GetPayloadSize() > 0
			&& Tracks.GetPayloadSize() <= MaximumAnimationClipImportedDataBytes;
	}

	auto FAnimationClipImportedData::GetIdentity() const -> FXxHash128
	{
		if (SchemaVersion != AnimationClipImportedDataSchemaVersion
			|| Tracks.GetPayloadSize() == 0) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(Tracks.GetPayloadId());
		return Builder.Finalize();
	}

	DAnimationClip::DAnimationClip(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	auto DAnimationClip::GetPayloadData() const
		-> std::shared_ptr<const FAnimationClipPayloadData>
	{
		if (!PayloadData && GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedPlatformData.GetMetadata().LogicalSize != 0)
		{
			std::string Error;
			const_cast<DAnimationClip*>(this)->LoadCookedPayload(Error);
		}
		return PayloadData;
	}

	auto DAnimationClip::PublishBuiltProduct(
		FAnimationClipPublicationCandidate InData,
		std::string& OutError) -> bool
	{
		if (!InData.Skeleton || !InData.Payload || InData.ClipName.IsNone())
			return Fail("Animation imported data requires a Skeleton, payload, and clip name.", &OutError);
		const DSkeleton* ValidationSkeleton = InData.ValidationSkeleton
			? InData.ValidationSkeleton : InData.Skeleton;
		if (InData.SkeletonCompatibilityIdentity != ValidationSkeleton->GetCompatibilityIdentity())
			return Fail("Animation imported data is incompatible with its Skeleton.", &OutError);
		if (!ValidateAnimationClipPayload(*InData.Payload, *ValidationSkeleton, OutError)) return false;
		FAnimationClipImportedData ImportedCandidate;
		if (InData.bReplaceImportedData
			&& !ImportedCandidate.Capture(
				*InData.Payload, ValidationSkeleton->GetBoneCount(), OutError)) return false;
		if (!InData.bReplaceImportedData
			&& !ImportedData.IsValid(ValidationSkeleton->GetBoneCount()))
			return Fail("AnimationClip canonical imported data is missing or invalid.", &OutError);
		uint64 KeyCount = 0;
		for (const FAnimationTrackData& Track : InData.Payload->Tracks) KeyCount += Track.Times.size();

		Skeleton = InData.Skeleton;
		SkeletonCompatibilityIdentity = std::move(InData.SkeletonCompatibilityIdentity);
		ClipName = InData.ClipName;
		Summary = {
			.DurationSeconds = InData.Payload->DurationSeconds,
			.TrackCount = static_cast<uint32>(InData.Payload->Tracks.size()),
			.KeyCount = static_cast<uint32>(KeyCount)};
		CookedPlatformData = {};
		DerivedDataKey = std::move(InData.DerivedDataKey);
		if (InData.bReplaceImportedData) ImportedData = std::move(ImportedCandidate);
		PayloadData = std::move(InData.Payload);
		bLoadedFromDerivedDataCache = InData.bLoadedFromDerivedDataCache;
		PayloadStorageDiagnostic = std::move(InData.DiagnosticMessage);
		if (PayloadStorageDiagnostic.empty() && !DerivedDataKey.empty())
			PayloadStorageDiagnostic = std::format(
				"Published built AnimationClip key {}.", DerivedDataKey);
		if (InData.bMarkPackageDirty) MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DAnimationClip::Validate(std::string& OutError) const -> bool
	{
		if (!Skeleton)
			return Fail("AnimationClip has no Skeleton reference.", &OutError);
		return ValidateAgainstSkeleton(*Skeleton, OutError);
	}

	auto DAnimationClip::ValidateAgainstSkeleton(
		const DSkeleton& ProspectiveSkeleton,
		std::string& OutError) const -> bool
	{
		if (!ProspectiveSkeleton.Validate(OutError))
		{
			OutError = std::format("AnimationClip references an invalid Skeleton: {}", OutError);
			return false;
		}
		if (!Skeleton || SkeletonCompatibilityIdentity != ProspectiveSkeleton.GetCompatibilityIdentity())
			return Fail("AnimationClip compatibility identity does not match its Skeleton.", &OutError);
		if (ClipName.IsNone() || !std::isfinite(Summary.DurationSeconds) || Summary.DurationSeconds < 0.0f
			|| Summary.TrackCount == 0 || Summary.TrackCount > MaximumAnimationClipTracks
			|| Summary.KeyCount == 0 || Summary.KeyCount > MaximumAnimationKeysPerClip)
			return Fail("AnimationClip authored summary or name is invalid.", &OutError);
		if (PayloadData)
		{
			if (!ValidateAnimationClipPayload(*PayloadData, ProspectiveSkeleton, OutError)) return false;
			uint64 KeyCount = 0;
			for (const FAnimationTrackData& Track : PayloadData->Tracks) KeyCount += Track.Times.size();
			if (PayloadData->DurationSeconds != Summary.DurationSeconds
				|| PayloadData->Tracks.size() != Summary.TrackCount || KeyCount != Summary.KeyCount)
				return Fail("AnimationClip payload does not match its authored summary.", &OutError);
		}
		OutError.clear();
		return true;
	}

	auto DAnimationClip::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (!Validate(OutError))
		{
			OutError = std::format("{}: {}", GetName(), OutError);
			return false;
		}
		if (PayloadData) return true;
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedPlatformData.GetMetadata().LogicalSize == 0)
				return Fail(std::format(
					"Cooked AnimationClip '{}': required PlatformData field is missing.",
					GetObjectPath()), &OutError);
			DerivedDataKey.clear();
			bLoadedFromDerivedDataCache = false;
			PayloadStorageDiagnostic = std::format(
				"Loaded cooked AnimationClip metadata for '{}'.", GetObjectPath());
			OutError.clear();
			return true;
		}
		return InvokeAnimationClipUncookedPostLoad(*this, OutError);
	}

	auto DAnimationClip::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"AnimationClip cooked platform data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* FieldValue = &CookedPlatformData;
		if (Ar.IsSaving())
		{
			if (!PayloadData || !Skeleton)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"AnimationClip cooked platform data is unavailable.");
				return;
			}
			FByteArray Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload);
			const_cast<FAnimationClipPayloadData&>(*PayloadData).Serialize(Writer, {
				.SkeletonBoneCount = Skeleton->GetBoneCount(),
				.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
				.TargetProfile = ESkeletalPayloadTargetProfile::Game});
			std::string Error;
			if (Writer.HasError()
				|| !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					Error.empty() ? std::string(Writer.GetError()) : std::move(Error));
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DAnimationClip"),
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DAnimationClip::LoadCookedPayload(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			PayloadStorageDiagnostic = std::format(
				"Cooked AnimationClip '{}': {}", GetObjectPath(), Message);
			OutError = PayloadStorageDiagnostic;
			return false;
		};
		std::span<const std::byte> Bytes;
		if (!CookedPlatformData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);
		FAnimationClipPayloadData Candidate;
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload);
		Candidate.Serialize(Ar, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
		{
			const std::string Error(Ar.GetError());
			CookedPlatformData.UnlockReadOnly();
			return FailCooked(Error);
		}
		uint64 KeyCount = 0;
		for (const FAnimationTrackData& Track : Candidate.Tracks) KeyCount += Track.Times.size();
		if (Candidate.DurationSeconds != Summary.DurationSeconds
			|| Candidate.Tracks.size() != Summary.TrackCount || KeyCount != Summary.KeyCount)
		{
			CookedPlatformData.UnlockReadOnly();
			return FailCooked("payload does not match authored summary.");
		}
		if (!CookedPlatformData.UnlockReadOnly(&OutError)) return FailCooked(OutError);
		PayloadData = std::make_shared<const FAnimationClipPayloadData>(std::move(Candidate));
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		PayloadStorageDiagnostic = std::format(
			"Loaded cooked AnimationClip payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DAnimationClip::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
			return Fail(std::format(
				"AnimationClip '{}' supports only the Win64 game cook target.", GetObjectPath()), &OutError);
		if (!PayloadData && !PostLoad(OutError)) return false;
		if (!PayloadData) return Fail("AnimationClip has no CPU payload to cook.", &OutError);
		return Context.AddPackage(std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DAnimationClip::PrepareImportedStateExchange(
		DAnimationClip& Candidate,
		std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>
	{
		if (!Candidate.GetSkeleton())
			return Fail("Candidate AnimationClip has no Skeleton reference.", &OutError), nullptr;
		return PrepareImportedStateExchange(Candidate, *Candidate.GetSkeleton(), OutError);
	}

	auto DAnimationClip::PrepareImportedStateExchange(
		DAnimationClip& Candidate,
		const DSkeleton& ProspectiveSkeleton,
		std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>
	{
		if (&Candidate == this)
			return Fail("AnimationClip imported-state exchange requires distinct assets.", &OutError), nullptr;
		if (!Validate(OutError))
		{
			OutError = std::format("Target AnimationClip is invalid: {}", OutError);
			return nullptr;
		}
		if (!Candidate.ValidateAgainstSkeleton(ProspectiveSkeleton, OutError))
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
		std::swap(Target->CookedPlatformData, Candidate->CookedPlatformData);
		std::swap(Target->DerivedDataKey, Candidate->DerivedDataKey);
		std::swap(Target->ImportedData, Candidate->ImportedData);
		std::swap(Target->PayloadData, Candidate->PayloadData);
		std::swap(Target->bLoadedFromDerivedDataCache, Candidate->bLoadedFromDerivedDataCache);
		std::swap(Target->PayloadStorageDiagnostic, Candidate->PayloadStorageDiagnostic);
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
