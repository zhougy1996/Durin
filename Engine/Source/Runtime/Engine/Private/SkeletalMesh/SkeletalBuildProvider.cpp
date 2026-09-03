#include "SkeletalMesh/SkeletalBuild.h"
#include "SkeletalMesh/SkeletalDerivedDataKey.h"

#include "Asset/AssetDerivedDataCache.h"
#include "Serialization/Archive.h"

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		constexpr std::string_view SkeletalMeshBucket = "SkeletalMesh/Objects";
		constexpr std::string_view AnimationClipBucket = "AnimationClip/Objects";

		template<typename T>
		auto EncodePayload(const T& Payload,
			const FSkeletalPayloadSerializationContext& Context,
			FByteArray& OutBytes,
			std::string& OutError) -> bool
		{
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload);
			const_cast<T&>(Payload).Serialize(Ar, Context);
			if (!Ar.HasError())
			{
				OutError.clear();
				return true;
			}
			OutError = Ar.GetFailure()->Message;
			OutBytes.clear();
			return false;
		}

		template<typename T>
		auto DecodePayload(std::span<const std::byte> Bytes,
			const FSkeletalPayloadSerializationContext& Context,
			T& OutPayload,
			std::string& OutError) -> bool
		{
			T Candidate;
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			Candidate.Serialize(Ar, Context);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
					: "Skeletal payload has trailing bytes.";
				return false;
			}
			OutPayload = std::move(Candidate);
			OutError.clear();
			return true;
		}

		auto SetInvocationError(EFeatureInvokeStatus Status,
			std::string& OutError) -> void
		{
			if (Status == EFeatureInvokeStatus::Unavailable)
				OutError = "The skeletal build provider is unavailable.";
			else if (Status == EFeatureInvokeStatus::Ambiguous)
				OutError = "Multiple skeletal build providers are registered.";
			else if (Status == EFeatureInvokeStatus::VisitorFailed)
				OutError = "The skeletal build provider invocation failed.";
			else if (OutError.empty())
				OutError = "The skeletal build provider failed without a diagnostic.";
		}
	}

#endif
	auto BuildSkeletalMeshDerivedData(
		FSkeletalMeshDerivedDataRequest Request,
		FSkeletalMeshDerivedDataResult& OutResult,
		std::string& OutError) -> bool
	{
		OutResult = {};
#if !DURIN_WITH_EDITOR
		OutError = "SkeletalMesh authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		if (Request.ShouldCancel && Request.ShouldCancel())
		{
			OutError = "SkeletalMesh build was canceled.";
			return false;
		}
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ISkeletalBuildProvider>([&](ISkeletalBuildProvider& Provider) {
			const FSkeletalBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid() || (!Request.Payload && !Request.LoadPayload)
				|| Request.ImportedDataIdentity.IsZero()
				|| Request.SkeletonCompatibilityIdentity.empty())
			{
				OutError = "SkeletalMesh derived-data request is incomplete.";
				return false;
			}
			FByteArray Bytes;
			if (Request.Payload
				&& !EncodePayload(*Request.Payload, Request.Context, Bytes, OutError)) return false;
			FSkeletalMeshBuildKeyInput KeyInput;
			static_cast<FSkeletalBuildKeyFields&>(KeyInput) = {
				.ProviderIdentity = Descriptor.SkeletalMeshProducerIdentity,
				.ProviderVersion = Descriptor.SkeletalMeshProducerVersion,
				.ImportedDataIdentity = Request.ImportedDataIdentity,
				.PayloadInputFingerprint = Request.ImportedDataIdentity,
				.SkeletonCompatibilityIdentity = Request.SkeletonCompatibilityIdentity,
				.TargetPlatform = Request.Context.TargetPlatform,
				.TargetProfile = Request.Context.TargetProfile};
			const std::string Key = BuildSkeletalMeshDerivedDataKey(KeyInput, OutError);
			if (Key.empty()) return false;
			AssetDerivedDataCache::FOperationDiagnostic ReadDiagnostic;
			FByteArray CachedBytes;
			FSkeletalMeshPayloadData Candidate;
			bool bHit = AssetDerivedDataCache::Load(SkeletalMeshBucket, Key,
				MaximumSkeletalMeshPayloadBytes, CachedBytes, ReadDiagnostic)
				== AssetDerivedDataCache::ELoadResult::Hit;
			if (bHit)
			{
				bHit = (!Request.Payload || CachedBytes == Bytes)
					&& DecodePayload(CachedBytes, Request.Context, Candidate, ReadDiagnostic.Message)
					&& ValidateSkeletalMeshPayload(Candidate,
						Request.Context.SkeletonBoneCount,
						Request.Context.MaterialSlotCount, ReadDiagnostic.Message);
				if (!bHit && ReadDiagnostic.Message.empty())
					ReadDiagnostic.Message = "Cached SkeletalMesh payload does not match the canonical input.";
			}
			AssetDerivedDataCache::FOperationDiagnostic WriteDiagnostic;
			std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
			if (bHit)
			{
				Payload = std::make_shared<const FSkeletalMeshPayloadData>(std::move(Candidate));
			}
			else
			{
				OutError.clear();
				if (!Request.Payload && Request.LoadPayload)
					Request.Payload = Request.LoadPayload(OutError);
				if (!Request.Payload) return false;
				FSkeletalMeshRecipeProduct Product;
				if (!Provider.BuildSkeletalMesh({Request.Payload, Request.Context,
					Request.ShouldCancel}, Product, OutError) || !Product.Payload) return false;
				Payload = std::move(Product.Payload);
				if (!EncodePayload(*Payload, Request.Context, Bytes, OutError)) return false;
				if (Request.ShouldCancel && Request.ShouldCancel())
				{
					OutError = "SkeletalMesh build was canceled.";
					return false;
				}
				if (Request.bPersistDerivedData)
					AssetDerivedDataCache::Store(SkeletalMeshBucket, Key, Bytes,
						MaximumSkeletalMeshPayloadBytes, WriteDiagnostic);
			}
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutError = "SkeletalMesh build was canceled.";
				return false;
			}
			OutResult = {
				.Payload = std::move(Payload), .Key = Key,
				.Origin = bHit ? ESkeletalDerivedDataOrigin::CacheHit
					: ESkeletalDerivedDataOrigin::Rebuilt,
				.Descriptor = Descriptor,
				.CacheReadNanoseconds = ReadDiagnostic.DurationNanoseconds,
				.CacheWriteNanoseconds = WriteDiagnostic.DurationNanoseconds,
				.PayloadBytes = bHit ? CachedBytes.size() : Bytes.size(),
				.Diagnostic = AssetDerivedDataCache::CombineDiagnostics(
					ReadDiagnostic, WriteDiagnostic)};
			return true;
		});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutResult = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}

	auto BuildAnimationClipDerivedData(
		FAnimationClipDerivedDataRequest Request,
		FAnimationClipDerivedDataResult& OutResult,
		std::string& OutError) -> bool
	{
		OutResult = {};
#if !DURIN_WITH_EDITOR
		OutError = "AnimationClip authored build orchestration is unavailable outside editor builds.";
		return false;
#else
		if (Request.ShouldCancel && Request.ShouldCancel())
		{
			OutError = "AnimationClip build was canceled.";
			return false;
		}
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ISkeletalBuildProvider>([&](ISkeletalBuildProvider& Provider) {
			const FSkeletalBuildProviderDescriptor Descriptor = Provider.GetDescriptor();
			if (!Descriptor.IsValid() || (!Request.Payload && !Request.LoadPayload)
				|| Request.ImportedDataIdentity.IsZero()
				|| Request.SkeletonCompatibilityIdentity.empty())
			{
				OutError = "AnimationClip derived-data request is incomplete.";
				return false;
			}
			FByteArray Bytes;
			if (Request.Payload
				&& !EncodePayload(*Request.Payload, Request.Context, Bytes, OutError)) return false;
			FAnimationClipBuildKeyInput KeyInput;
			static_cast<FSkeletalBuildKeyFields&>(KeyInput) = {
				.ProviderIdentity = Descriptor.AnimationClipProducerIdentity,
				.ProviderVersion = Descriptor.AnimationClipProducerVersion,
				.ImportedDataIdentity = Request.ImportedDataIdentity,
				.PayloadInputFingerprint = Request.ImportedDataIdentity,
				.SkeletonCompatibilityIdentity = Request.SkeletonCompatibilityIdentity,
				.TargetPlatform = Request.Context.TargetPlatform,
				.TargetProfile = Request.Context.TargetProfile};
			const std::string Key = BuildAnimationClipDerivedDataKey(KeyInput, OutError);
			if (Key.empty()) return false;
			AssetDerivedDataCache::FOperationDiagnostic ReadDiagnostic;
			FByteArray CachedBytes;
			FAnimationClipPayloadData Candidate;
			bool bHit = AssetDerivedDataCache::Load(AnimationClipBucket, Key,
				MaximumAnimationClipPayloadBytes, CachedBytes, ReadDiagnostic)
				== AssetDerivedDataCache::ELoadResult::Hit;
			if (bHit)
			{
				bHit = (!Request.Payload || CachedBytes == Bytes)
					&& DecodePayload(CachedBytes, Request.Context, Candidate, ReadDiagnostic.Message)
					&& ValidateAnimationClipPayload(Candidate,
						Request.Context.SkeletonBoneCount, ReadDiagnostic.Message);
				if (!bHit && ReadDiagnostic.Message.empty())
					ReadDiagnostic.Message = "Cached AnimationClip payload does not match the canonical input.";
			}
			AssetDerivedDataCache::FOperationDiagnostic WriteDiagnostic;
			std::shared_ptr<const FAnimationClipPayloadData> Payload;
			if (bHit)
			{
				Payload = std::make_shared<const FAnimationClipPayloadData>(std::move(Candidate));
			}
			else
			{
				OutError.clear();
				if (!Request.Payload && Request.LoadPayload)
					Request.Payload = Request.LoadPayload(OutError);
				if (!Request.Payload) return false;
				FAnimationClipRecipeProduct Product;
				if (!Provider.BuildAnimationClip({Request.Payload, Request.Context,
					Request.ShouldCancel}, Product, OutError) || !Product.Payload) return false;
				Payload = std::move(Product.Payload);
				if (!EncodePayload(*Payload, Request.Context, Bytes, OutError)) return false;
				if (Request.ShouldCancel && Request.ShouldCancel())
				{
					OutError = "AnimationClip build was canceled.";
					return false;
				}
				if (Request.bPersistDerivedData)
					AssetDerivedDataCache::Store(AnimationClipBucket, Key, Bytes,
						MaximumAnimationClipPayloadBytes, WriteDiagnostic);
			}
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutError = "AnimationClip build was canceled.";
				return false;
			}
			OutResult = {
				.Payload = std::move(Payload), .Key = Key,
				.Origin = bHit ? ESkeletalDerivedDataOrigin::CacheHit
					: ESkeletalDerivedDataOrigin::Rebuilt,
				.Descriptor = Descriptor,
				.CacheReadNanoseconds = ReadDiagnostic.DurationNanoseconds,
				.CacheWriteNanoseconds = WriteDiagnostic.DurationNanoseconds,
				.PayloadBytes = bHit ? CachedBytes.size() : Bytes.size(),
				.Diagnostic = AssetDerivedDataCache::CombineDiagnostics(
					ReadDiagnostic, WriteDiagnostic)};
			return true;
		});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value)
		{
			OutError.clear();
			return true;
		}
		OutResult = {};
		SetInvocationError(Invocation.Status, OutError);
		return false;
#endif
	}
}
