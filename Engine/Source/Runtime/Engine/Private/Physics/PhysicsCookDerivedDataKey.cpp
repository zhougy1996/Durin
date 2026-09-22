#include "Physics/PhysicsCookDerivedDataKey.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataCache/DerivedDataCache.h"
#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		auto ValidateTargetPlatform(
			FArchive& Ar,
			EAssetPayloadTargetPlatform TargetPlatform) -> bool
		{
			if (Ar.IsLoading())
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
					"Physics build-key input is save-only.");
				return false;
			}
			if (TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Physics derived-data target is unsupported.");
				return false;
			}
			return true;
		}

		template<typename T>
		auto BuildKeyBytes(const T& Input) -> std::expected<FByteBuffer, FPhysicsCookKeyError>
		{
			if (Input.TargetPlatform != EAssetPayloadTargetPlatform::Win64)
				return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::UnsupportedTarget, .TargetPlatform = Input.TargetPlatform});
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataKey);
			const_cast<T&>(Input).Serialize(Ar);
			if (Ar.IsError())
				return std::unexpected(FPhysicsCookKeyError{.Code = EPhysicsCookKeyError::Archive, .TargetPlatform = Input.TargetPlatform,
					.ArchiveCode = Ar.GetFailure()->Code, .ArchivePath = Ar.GetFailure()->Path});
			return Bytes;
		}

		template<typename T>
		auto BuildKey(const T& Input, std::string_view Bucket) -> std::expected<FCacheKeyProxy, FPhysicsCookKeyError>
		{
			auto Encoded = BuildKeyBytes(Input);
			if (!Encoded) return std::unexpected(std::move(Encoded.error()));
			return FCacheKeyProxy(DerivedData::FCacheKey::FromHash(
				DerivedData::FCacheBucket::FromString(Bucket), FXxHash128::HashBuffer(*Encoded)));
		}

	}

	auto FPhysicsCookKeyInput::Serialize(FArchive& Ar) -> void
	{
		if (!ValidateTargetPlatform(Ar, TargetPlatform)) return;
		uint32 KeySchemaVersion = PhysicsCollisionKeySchemaVersion;
		uint8 Mode = static_cast<uint8>(SourceMode);
		uint8 Policy = static_cast<uint8>(QueryPolicy);
		uint32 Platform = static_cast<uint32>(TargetPlatform);
		Ar << KeySchemaVersion << GeometryHash.HashLow << GeometryHash.HashHigh
			<< Mode << Policy
			<< WeldToleranceBits << BuilderVersion << PayloadSchemaVersion << Platform;
	}

	auto FormatPhysicsCookKeyError(const FPhysicsCookKeyError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EPhysicsCookKeyError::None: return {};
		case EPhysicsCookKeyError::UnsupportedTarget:
			return std::format("Physics derived-data target {} is unsupported.", static_cast<uint32>(Error.TargetPlatform));
		case EPhysicsCookKeyError::Archive:
			return std::format("Physics derived-data key encoding failed (Archive code {}, path '{}').",
				Error.ArchiveCode ? static_cast<int>(*Error.ArchiveCode) : -1, Error.ArchivePath);
		}
		return {};
	}

	auto BuildPhysicsCookDerivedDataKeyBytes(const FPhysicsCookKeyInput& Input) -> std::expected<FByteBuffer, FPhysicsCookKeyError>
	{
		return BuildKeyBytes(Input);
	}
	auto BuildPhysicsCookDerivedDataKey(const FPhysicsCookKeyInput& Input) -> std::expected<FCacheKeyProxy, FPhysicsCookKeyError>
	{
		return BuildKey(Input, PhysicsCollisionCacheBucket);
	}
}
#endif
