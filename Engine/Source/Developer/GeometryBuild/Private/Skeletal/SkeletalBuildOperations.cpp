#include "Skeletal/SkeletalBuildOperations.h"

#include "AssetBuild/BuildCache.h"
#include "DerivedDataObjectStore.h"
#include "Serialization/Archive.h"

namespace Durin::AssetBuild
{
	namespace
	{
		inline constexpr uint64 SkeletalDerivedDataBudgetBytes =
			32ull * 1024ull * 1024ull * 1024ull;
		inline constexpr uint32 SkeletalDerivedDataCleanupDeleteLimit = 256;

		auto GetSkeletalMeshStore() -> Asset::FDerivedDataObjectStore&
		{
			static Asset::FDerivedDataObjectStore Store(
				std::filesystem::path("SkeletalMesh/Objects"),
				MaximumSkeletalMeshPayloadBytes);
			return Store;
		}

		auto GetAnimationClipStore() -> Asset::FDerivedDataObjectStore&
		{
			static Asset::FDerivedDataObjectStore Store(
				std::filesystem::path("AnimationClip/Objects"),
				MaximumAnimationClipPayloadBytes);
			return Store;
		}

		auto ValidateKeyFields(FArchive& Ar, const FSkeletalBuildKeyFields& Input) -> bool
		{
			if (Ar.IsLoading())
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
					"Skeletal build-key input is save-only.");
				return false;
			}
			if (Input.ProviderIdentity.empty() || Input.ProviderVersion == 0
				|| Input.StableOutputIdentity.empty()
				|| Input.SkeletonCompatibilityIdentity.empty()
				|| Input.TargetPlatform != ESkeletalPayloadTargetPlatform::Win64
				|| Input.TargetProfile != ESkeletalPayloadTargetProfile::Game)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Skeletal build-key identity is incomplete or unsupported.");
				return false;
			}
			return true;
		}

		auto SerializeKeyFields(
			FArchive& Ar,
			FSkeletalBuildKeyFields& Input,
			std::string_view BuilderIdentity,
			uint32 BuilderVersion,
			uint32 PayloadVersion) -> void
		{
			if (!ValidateKeyFields(Ar, Input)) return;
			uint32 KeyVersion = SkeletalPayloadKeySchemaVersion;
			std::string Identity(BuilderIdentity);
			uint32 Platform = static_cast<uint32>(Input.TargetPlatform);
			uint32 Profile = static_cast<uint32>(Input.TargetProfile);
			auto SerializeVersionOneString = [&Ar](std::string& Value) {
				if (Value.size() > std::numeric_limits<uint32>::max())
				{
					Ar.Fail(EArchiveFailureCode::LimitExceeded,
						"Skeletal build-key string exceeds the version-1 wire bound.");
					return;
				}
				uint32 Size = static_cast<uint32>(Value.size());
				Ar << Size;
				if (!Ar.HasError()) Ar.Serialize(Value.data(), Value.size());
			};
			Ar << KeyVersion;
			SerializeVersionOneString(Identity);
			Ar << BuilderVersion << PayloadVersion << Platform << Profile;
			SerializeVersionOneString(Input.ProviderIdentity);
			Ar << Input.ProviderVersion
				<< Input.SourceClosureHash.HashLow << Input.SourceClosureHash.HashHigh
				<< Input.SettingsHash.HashLow << Input.SettingsHash.HashHigh
				<< Input.ProviderStateHash.HashLow << Input.ProviderStateHash.HashHigh
				<< Input.PayloadInputFingerprint.HashLow
				<< Input.PayloadInputFingerprint.HashHigh;
			SerializeVersionOneString(Input.StableOutputIdentity);
			SerializeVersionOneString(Input.SkeletonCompatibilityIdentity);
		}

		template<typename T>
		auto MakeKeyBytes(const T& Input, std::string& OutError) -> std::vector<uint8>
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
			const_cast<T&>(Input).Serialize(Ar);
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				Bytes.clear();
			}
			else OutError.clear();
			return Bytes;
		}

		template<typename T>
		auto SerializePayload(
			T& Payload,
			const FSkeletalPayloadSerializationContext& Context,
			std::vector<uint8>& OutBytes,
			std::string& OutError) -> bool
		{
			OutBytes.clear();
			FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::DerivedDataPayload);
			Payload.Serialize(Ar, Context);
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
		auto LoadPayload(
			Asset::FDerivedDataObjectStore& Store,
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			T& OutPayload,
			std::string& OutMessage) -> bool
		{
			const FBuildCacheQueryResult Read = FBuildCacheClient(Store).Query(
				Key, "SkeletalPayload", {});
			if (Read.Status != EBuildCacheQueryStatus::Hit)
			{
				OutMessage = Read.Diagnostic;
				return false;
			}
			T Candidate;
			FCanonicalMemoryReader Ar(Read.Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
			Candidate.Serialize(Ar, Context);
			if (Ar.HasError())
			{
				OutMessage = Ar.GetFailure()->Message;
				return false;
			}
			OutPayload = std::move(Candidate);
			OutMessage = Read.Diagnostic;
			return true;
		}

		auto StorePayload(
			Asset::FDerivedDataObjectStore& Store,
			std::string_view Key,
			std::span<const uint8> Bytes,
			std::string& OutError) -> bool
		{
			if (!FBuildCacheClient(Store).Store(Key,
				FBuildValue::FromOwned("SkeletalPayload", std::vector<uint8>(Bytes.begin(), Bytes.end())),
				{.bRequireStoreSuccess = true}, &OutError)) return false;
			const Asset::FDerivedDataObjectCleanupResult Cleanup = Store.CleanupToBudget(
				SkeletalDerivedDataBudgetBytes, SkeletalDerivedDataCleanupDeleteLimit);
			if (!Cleanup.Message.empty())
				DURIN_WARN("Skeletal DDC cleanup: {}", Cleanup.Message);
			OutError.clear();
			return true;
		}
	}

	auto FSkeletalMeshBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		SerializeKeyFields(Ar, *this, SkeletalMeshBuilderIdentity,
			SkeletalMeshBuilderVersion, SkeletalMeshPayloadSchemaVersion);
	}

	auto FAnimationClipBuildKeyInput::Serialize(FArchive& Ar) -> void
	{
		SerializeKeyFields(Ar, *this, AnimationClipBuilderIdentity,
			AnimationClipBuilderVersion, AnimationClipPayloadSchemaVersion);
	}

	auto BuildSkeletalMeshDerivedDataKeyBytes(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>
	{
		return MakeKeyBytes(Input, OutError);
	}

	auto BuildSkeletalMeshDerivedDataKey(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<uint8> Bytes = MakeKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}

	auto BuildAnimationClipDerivedDataKeyBytes(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>
	{
		return MakeKeyBytes(Input, OutError);
	}

	auto BuildAnimationClipDerivedDataKey(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const std::vector<uint8> Bytes = MakeKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}

	auto BuildSkeletalMeshProduct(
		FSkeletalMeshBuildRequest Request,
		FSkeletalMeshBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!Request.Payload || Request.SkeletonBoneCount == 0
			|| Request.SkeletonCompatibilityIdentity
				!= Request.KeyInput.SkeletonCompatibilityIdentity)
		{
			OutError = "SkeletalMesh Build request has incomplete relationship state.";
			return false;
		}
		const FSkeletalPayloadSerializationContext Context{
			.SkeletonBoneCount = Request.SkeletonBoneCount,
			.MaterialSlotCount = Request.MaterialSlotCount,
			.TargetPlatform = Request.KeyInput.TargetPlatform,
			.TargetProfile = Request.KeyInput.TargetProfile};
		std::vector<uint8> Bytes;
		FSkeletalMeshPayloadData& Payload =
			const_cast<FSkeletalMeshPayloadData&>(*Request.Payload);
		if (!SerializePayload(Payload, Context, Bytes, OutError)) return false;
		Request.KeyInput.PayloadInputFingerprint = FXxHash128::HashBuffer(Bytes);
		const std::string Key = BuildSkeletalMeshDerivedDataKey(Request.KeyInput, OutError);
		if (Key.empty()) return false;
		std::string StoreError;
		const bool bStored = StorePayload(
			GetSkeletalMeshStore(), Key, Bytes, StoreError);
		OutProduct = {
			.MeshNodeBindTransform = Request.MeshNodeBindTransform,
			.Payload = std::move(Request.Payload),
			.SkeletonCompatibilityIdentity = std::move(Request.SkeletonCompatibilityIdentity),
			.DerivedDataKey = Key,
			.Diagnostic = bStored
				? std::format("Stored SkeletalMesh DDC key {}.", Key)
				: std::format("SkeletalMesh DDC write failed for key {}: {}", Key, StoreError)};
		OutError.clear();
		return true;
	}

	auto BuildAnimationClipProduct(
		FAnimationClipBuildRequest Request,
		FAnimationClipBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!Request.Payload || Request.ClipName.IsNone() || Request.SkeletonBoneCount == 0
			|| Request.SkeletonCompatibilityIdentity
				!= Request.KeyInput.SkeletonCompatibilityIdentity)
		{
			OutError = "AnimationClip Build request has incomplete relationship state.";
			return false;
		}
		const FSkeletalPayloadSerializationContext Context{
			.SkeletonBoneCount = Request.SkeletonBoneCount,
			.TargetPlatform = Request.KeyInput.TargetPlatform,
			.TargetProfile = Request.KeyInput.TargetProfile};
		std::vector<uint8> Bytes;
		FAnimationClipPayloadData& Payload =
			const_cast<FAnimationClipPayloadData&>(*Request.Payload);
		if (!SerializePayload(Payload, Context, Bytes, OutError)) return false;
		Request.KeyInput.PayloadInputFingerprint = FXxHash128::HashBuffer(Bytes);
		const std::string Key = BuildAnimationClipDerivedDataKey(Request.KeyInput, OutError);
		if (Key.empty()) return false;
		std::string StoreError;
		const bool bStored = StorePayload(
			GetAnimationClipStore(), Key, Bytes, StoreError);
		OutProduct = {
			.ClipName = Request.ClipName,
			.Payload = std::move(Request.Payload),
			.SkeletonCompatibilityIdentity = std::move(Request.SkeletonCompatibilityIdentity),
			.DerivedDataKey = Key,
			.Diagnostic = bStored
				? std::format("Stored AnimationClip DDC key {}.", Key)
				: std::format("AnimationClip DDC write failed for key {}: {}", Key, StoreError)};
		OutError.clear();
		return true;
	}

	auto LoadSkeletalMeshDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FSkeletalMeshPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		return LoadPayload(GetSkeletalMeshStore(), Key, Context, OutPayload, OutMessage);
	}

	auto LoadAnimationClipDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		FAnimationClipPayloadData& OutPayload,
		std::string& OutMessage) -> bool
	{
		return LoadPayload(GetAnimationClipStore(), Key, Context, OutPayload, OutMessage);
	}

	auto StoreSkeletalMeshDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		const FSkeletalMeshPayloadData& Payload,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Bytes;
		if (!SerializePayload(const_cast<FSkeletalMeshPayloadData&>(Payload),
			Context, Bytes, OutError)) return false;
		return StorePayload(GetSkeletalMeshStore(), Key, Bytes, OutError);
	}

	auto StoreAnimationClipDerivedData(
		std::string_view Key,
		const FSkeletalPayloadSerializationContext& Context,
		const FAnimationClipPayloadData& Payload,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Bytes;
		if (!SerializePayload(const_cast<FAnimationClipPayloadData&>(Payload),
			Context, Bytes, OutError)) return false;
		return StorePayload(GetAnimationClipStore(), Key, Bytes, OutError);
	}
}
