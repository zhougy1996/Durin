#include "Animation/AnimationClip.h"

#include "AssetSystem.h"
#include "DObject/Property.h"
#include "Math/Operations.h"
#include "Serialization/Archive.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"
#include "SkeletalMesh/SkeletalDerivedData.h"

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
		uint32 SkeletonBoneCount,
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
				|| Track.BoneIndex >= SkeletonBoneCount
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

	auto ValidateAnimationClipPayload(
		const FAnimationClipPayloadData& Payload,
		const DSkeleton& Skeleton,
		std::string& OutError) -> bool
	{
		return ValidateAnimationClipPayload(
			Payload, Skeleton.GetBoneCount(), OutError);
	}

	DAnimationClip::DAnimationClip(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer) {}

	auto DAnimationClip::PublishBuiltProduct(
		FAnimationClipPublicationCandidate InData,
		std::string& OutError) -> bool
	{
		if (!InData.Skeleton || !InData.Payload || InData.ClipName.IsNone())
			return Fail(OutError, "Animation imported data requires a Skeleton, payload, and clip name.");
		const DSkeleton* ValidationSkeleton = InData.ValidationSkeleton
			? InData.ValidationSkeleton : InData.Skeleton;
		if (InData.SkeletonCompatibilityIdentity != ValidationSkeleton->GetCompatibilityIdentity())
			return Fail(OutError, "Animation imported data is incompatible with its Skeleton.");
		if (!ValidateAnimationClipPayload(*InData.Payload, *ValidationSkeleton, OutError)) return false;
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
		DerivedDataKey = std::move(InData.DerivedDataKey);
		PayloadData = std::move(InData.Payload);
		bLoadedFromDerivedDataCache = false;
		PayloadStorageDiagnostic = std::move(InData.DiagnosticMessage);
		if (PayloadStorageDiagnostic.empty() && !DerivedDataKey.empty())
			PayloadStorageDiagnostic = std::format(
				"Published built AnimationClip key {}.", DerivedDataKey);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DAnimationClip::Validate(std::string& OutError) const -> bool
	{
		if (!Skeleton)
			return Fail(OutError, "AnimationClip has no Skeleton reference.");
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
			return Fail(OutError, "AnimationClip compatibility identity does not match its Skeleton.");
		if (ClipName.IsNone() || !std::isfinite(Summary.DurationSeconds) || Summary.DurationSeconds < 0.0f
			|| Summary.TrackCount == 0 || Summary.TrackCount > MaximumAnimationClipTracks
			|| Summary.KeyCount == 0 || Summary.KeyCount > MaximumAnimationKeysPerClip)
			return Fail(OutError, "AnimationClip authored summary or name is invalid.");
		if (PayloadData)
		{
			if (!ValidateAnimationClipPayload(*PayloadData, ProspectiveSkeleton, OutError)) return false;
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
		if (!Validate(OutError))
		{
			OutError = std::format("{}: {}", GetName(), OutError);
			return false;
		}
		if (Asset::IsAssetMigrationLoad()) return true;
		if (PayloadData) return true;
		if (Asset::GetPackageLoadContext().Mode == Asset::EPackageLoadMode::CookedRuntime)
			return LoadCookedPayload(OutError);
		if (!DerivedDataKey.empty() && !LoadDerivedDataPayload(OutError))
		{
			if (!IsSkeletalDerivedDataRepairLoadActive()) return false;
			ReportMissingSkeletalDerivedDataAsset(this);
			OutError.clear();
		}
		return true;
	}

	auto DAnimationClip::LoadDerivedDataPayload(std::string& OutError) -> bool
	{
		std::string CacheMessage;
		FAnimationClipPayloadData Candidate;
		const FSkeletalPayloadSerializationContext SerializationContext{
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		if (!InvokeAnimationClipUncookedPayloadLoader(
			DerivedDataKey, SerializationContext, Candidate, CacheMessage))
		{
			PayloadStorageDiagnostic = std::format(
				"AnimationClip DDC miss for key {}: {}", DerivedDataKey, CacheMessage);
			return Fail(OutError, PayloadStorageDiagnostic);
		}
		uint64 KeyCount = 0;
		for (const FAnimationTrackData& Track : Candidate.Tracks) KeyCount += Track.Times.size();
		if (Candidate.DurationSeconds != Summary.DurationSeconds
			|| Candidate.Tracks.size() != Summary.TrackCount || KeyCount != Summary.KeyCount)
			return Fail(OutError, "AnimationClip DDC payload does not match authored summary.");
		PayloadData = std::make_shared<const FAnimationClipPayloadData>(std::move(Candidate));
		bLoadedFromDerivedDataCache = true;
		PayloadStorageDiagnostic = std::format(
			"Loaded AnimationClip DDC key {}.", DerivedDataKey);
		OutError.clear();
		return true;
	}

	auto DAnimationClip::LoadCookedPayload(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			PayloadStorageDiagnostic = std::format(
				"Cooked AnimationClip '{}': {}", GetObjectPath(), Message);
			OutError = PayloadStorageDiagnostic;
			return false;
		};
		if (CookedPayload.PayloadId != AnimationClipPrimaryCookedPayloadId
			|| CookedPayload.LocationKind
				!= static_cast<uint32>(Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedPayload.PayloadSchemaVersion != AnimationClipPayloadSchemaVersion
			|| CookedPayload.TargetPlatform
				!= static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedPayload.TargetProfile
				!= static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required DANM descriptor is missing or incompatible.");
		const Asset::FPackageLoadContext& Context = Asset::GetPackageLoadContext();
		std::filesystem::path PackagePath;
		std::filesystem::path CompanionPath;
		if (!GetPackage()
			|| !Asset::ResolveCookedPackagePath(
				Context.CookRoot, GetPackage()->GetPackagePath(), PackagePath, &OutError)
			|| !Asset::ResolveCookedCompanionPath(
				Context.CookRoot, PackagePath, CompanionPath, &OutError))
			return FailCooked(OutError.empty()
				? "package companion path could not be resolved." : OutError);
		Asset::FCookedBulkContainer Container;
		if (!Asset::LoadCookedBulkFile(
			CompanionPath, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, Container, &OutError))
			return FailCooked(OutError);
		std::span<const uint8> Bytes;
		if (!Asset::ResolveCookedPayload(Container, CookedPayload, Bytes, &OutError))
			return FailCooked(OutError);
		FAnimationClipPayloadData Candidate;
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload);
		Candidate.Serialize(Ar, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError()) return FailCooked(Ar.GetFailure()->Message);
		uint64 KeyCount = 0;
		for (const FAnimationTrackData& Track : Candidate.Tracks) KeyCount += Track.Times.size();
		if (Candidate.DurationSeconds != Summary.DurationSeconds
			|| Candidate.Tracks.size() != Summary.TrackCount || KeyCount != Summary.KeyCount)
			return FailCooked("payload does not match authored summary.");
		PayloadData = std::make_shared<const FAnimationClipPayloadData>(std::move(Candidate));
		DerivedDataKey.clear();
		bLoadedFromDerivedDataCache = false;
		PayloadStorageDiagnostic = std::format(
			"Loaded cooked AnimationClip payload for '{}'.", GetObjectPath());
		OutError.clear();
		return true;
	}

	auto DAnimationClip::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError,
		bool bRetainDiagnosticEditorMetadata) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
			return Fail(OutError, std::format(
				"AnimationClip '{}' supports only the Win64 game cook target.", GetObjectPath()));
		if (!PayloadData && !PostLoad(OutError)) return false;
		if (!PayloadData) return Fail(OutError, "AnimationClip has no CPU payload to cook.");
		std::vector<uint8> PayloadBytes;
		FCanonicalMemoryWriter Ar(PayloadBytes, EArchivePurpose::CookedPayload);
		const_cast<FAnimationClipPayloadData&>(*PayloadData).Serialize(Ar, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game});
		if (Ar.HasError()) return Fail(OutError, Ar.GetFailure()->Message);
		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = AnimationClipPrimaryCookedPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = AnimationClipPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = SkeletalPayloadAlignment,
			.Bytes = std::move(PayloadBytes)};
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this, bRetainDiagnosticEditorMetadata](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<uint8>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId != AnimationClipPrimaryCookedPayloadId)
				{
					if (Error) *Error = "AnimationClip cook did not produce its required descriptor.";
					return false;
				}
				const Asset::FCookedPayloadDescriptor SavedDescriptor = CookedPayload;
				const std::string SavedKey = DerivedDataKey;
				CookedPayload = Descriptors.front();
				if (!bRetainDiagnosticEditorMetadata) DerivedDataKey.clear();
				Asset::FAssetPackageSerializationOptions Options;
				if (!bRetainDiagnosticEditorMetadata)
					Options.PropertyFilter = [this](const DObject* Object, const FProperty* Property) {
						return Object != this || Property->NamePrivate != FName("DerivedDataKey");
					};
				const Asset::FAssetResult Serialized = Asset::SerializeAssetPackageBytes(
					GetPackage(), OutPackageBytes, Options);
				CookedPayload = SavedDescriptor;
				DerivedDataKey = SavedKey;
				if (!Serialized)
				{
					if (Error) *Error = Serialized.Message;
					return false;
				}
				return true;
			}, &OutError);
	}

	auto DAnimationClip::PrepareImportedStateExchange(
		DAnimationClip& Candidate,
		std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>
	{
		if (!Candidate.GetSkeleton())
			return Fail(OutError, "Candidate AnimationClip has no Skeleton reference."), nullptr;
		return PrepareImportedStateExchange(Candidate, *Candidate.GetSkeleton(), OutError);
	}

	auto DAnimationClip::PrepareImportedStateExchange(
		DAnimationClip& Candidate,
		const DSkeleton& ProspectiveSkeleton,
		std::string& OutError) -> std::unique_ptr<FAnimationClipImportedStateExchange>
	{
		if (&Candidate == this)
			return Fail(OutError, "AnimationClip imported-state exchange requires distinct assets."), nullptr;
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
		std::swap(Target->CookedPayload, Candidate->CookedPayload);
		std::swap(Target->DerivedDataKey, Candidate->DerivedDataKey);
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
