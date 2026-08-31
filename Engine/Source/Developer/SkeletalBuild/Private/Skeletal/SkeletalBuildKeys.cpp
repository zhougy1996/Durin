#include "Skeletal/SkeletalBuildOperations.h"

#include "Serialization/Archive.h"

namespace Durin::Asset
{
	namespace
	{
		auto ValidateKeyFields(FArchive& Ar, const FSkeletalBuildKeyFields& Input) -> bool
		{
			if (Ar.IsLoading())
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
					"Skeletal build-key input is save-only.");
				return false;
			}
			if (Input.ProviderIdentity.empty() || Input.ProviderVersion == 0
				|| Input.ImportedDataIdentity.IsZero()
				|| Input.PayloadInputFingerprint.IsZero()
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

		auto SerializeKeyFields(FArchive& Ar, FSkeletalBuildKeyFields& Input,
			std::string_view BuilderIdentity, uint32 BuilderVersion,
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
				<< Input.ImportedDataIdentity.HashLow
				<< Input.ImportedDataIdentity.HashHigh
				<< Input.PayloadInputFingerprint.HashLow
				<< Input.PayloadInputFingerprint.HashHigh;
			SerializeVersionOneString(Input.StableOutputIdentity);
			SerializeVersionOneString(Input.SkeletonCompatibilityIdentity);
		}

		template<typename T>
		auto MakeKeyBytes(const T& Input, std::string& OutError) -> FByteArray
		{
			FByteArray Bytes;
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
		std::string& OutError) -> FByteArray
	{
		return MakeKeyBytes(Input, OutError);
	}

	auto BuildSkeletalMeshDerivedDataKey(
		const FSkeletalMeshBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const FByteArray Bytes = MakeKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}

	auto BuildAnimationClipDerivedDataKeyBytes(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> FByteArray
	{
		return MakeKeyBytes(Input, OutError);
	}

	auto BuildAnimationClipDerivedDataKey(
		const FAnimationClipBuildKeyInput& Input,
		std::string& OutError) -> std::string
	{
		const FByteArray Bytes = MakeKeyBytes(Input, OutError);
		return Bytes.empty() ? std::string{} : FXxHash128::HashBuffer(Bytes).ToString();
	}
}
