#pragma once

#include "Serialization/SerializationDefinitions.h"
#include "Serialization/Archive.h"

namespace Durin
{
	// Defines the allocation boundary and stable diagnostic identity for one complete payload.
	struct FBoundedArchivePayloadPolicy
	{
		uint64 MaximumBytes = 0;
		std::string_view DiagnosticName;
	};

	inline auto ToPayloadArchiveFailureCode(EDecodeError Code) -> EArchiveFailureCode
	{
		return Code == EDecodeError::Incompatible
			? EArchiveFailureCode::UnsupportedVersion
			: EArchiveFailureCode::InvalidData;
	}

	// Adapts one complete encoded value to an Archive region. Loading consumes the
	// entire remaining bounded region and publishes only a successfully decoded value.
	template<typename T, typename EncodeFn, typename DecodeFn>
	auto SerializeBoundedArchivePayload(
		FArchive& Ar,
		T& Value,
		const FBoundedArchivePayloadPolicy& Policy,
		EncodeFn&& Encode,
		DecodeFn&& Decode) -> void
	{
		static_assert(std::is_default_constructible_v<T>,
			"Bounded payload values must be default constructible.");
		static_assert(std::is_move_assignable_v<T>,
			"Bounded payload values must be move assignable.");
		static_assert(std::is_invocable_r_v<bool, EncodeFn, const T&,
			FByteArray&, std::string&>,
			"Bounded payload encoders must accept the source value, bytes, and error.");
		static_assert(std::is_invocable_r_v<FDecodeResult, DecodeFn,
			std::span<const std::byte>, T&>,
			"Bounded payload decoders must accept bytes and a detached value.");

		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			FByteArray Bytes;
			std::string Error;
			if (!Encode(std::as_const(Value), Bytes, Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Error);
				return;
			}
			if (Bytes.size() > Policy.MaximumBytes)
			{
				Ar.Fail(EArchiveFailureCode::LimitExceeded,
					std::string(Policy.DiagnosticName) + " exceeds its stored-size limit.");
				return;
			}
			Ar.WriteBytes(std::as_bytes(std::span<const std::byte>(Bytes)));
			return;
		}

		if (!Ar.HasCapability(EArchiveCapability::RemainingPayload))
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				std::string(Policy.DiagnosticName) + " requires a bounded input archive.");
			return;
		}
		const uint64 ByteCount = Ar.GetRemainingPayloadBytes();
		if (ByteCount == std::numeric_limits<uint64>::max())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedCapability,
				std::string(Policy.DiagnosticName) + " requires a bounded input archive.");
			return;
		}
		if (ByteCount > Policy.MaximumBytes
			|| ByteCount > static_cast<uint64>(FByteArray().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				std::string(Policy.DiagnosticName) + " exceeds its stored-size limit.");
			return;
		}

		FByteArray Bytes(static_cast<size_t>(ByteCount));
		Ar.ReadBytes(std::as_writable_bytes(std::span<std::byte>(Bytes)));
		if (Ar.HasError()) return;

		T LoadedValue;
		const FDecodeResult Result = Decode(
			std::span<const std::byte>(Bytes), LoadedValue);
		if (!Result)
		{
			Ar.Fail(ToPayloadArchiveFailureCode(Result.Code), Result.Message);
			return;
		}
		Value = std::move(LoadedValue);
	}
}
