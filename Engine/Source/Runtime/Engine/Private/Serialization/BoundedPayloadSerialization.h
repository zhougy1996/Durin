#pragma once

#include "PayloadDecodeResult.h"
#include "Serialization/Archive.h"

namespace Durin
{
	// Defines the allocation boundary and stable diagnostic identity for one complete payload.
	struct FBoundedArchivePayloadPolicy
	{
		uint64 MaximumBytes = 0;
		std::string_view DiagnosticName;
	};

	inline auto ToPayloadArchiveFailureCode(EPayloadDecodeError Code) -> EArchiveFailureCode
	{
		return Code == EPayloadDecodeError::Incompatible
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
			std::vector<uint8>&, std::string&>,
			"Bounded payload encoders must accept the source value, bytes, and error.");
		static_assert(std::is_invocable_r_v<FPayloadDecodeResult, DecodeFn,
			std::span<const uint8>, T&>,
			"Bounded payload decoders must accept bytes and a detached value.");

		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
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
			Ar.WriteBytes(std::as_bytes(std::span<const uint8>(Bytes)));
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
			|| ByteCount > static_cast<uint64>(std::vector<uint8>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded,
				std::string(Policy.DiagnosticName) + " exceeds its stored-size limit.");
			return;
		}

		std::vector<uint8> Bytes(static_cast<size_t>(ByteCount));
		Ar.ReadBytes(std::as_writable_bytes(std::span<uint8>(Bytes)));
		if (Ar.HasError()) return;

		T LoadedValue;
		const FPayloadDecodeResult Result = Decode(
			std::span<const uint8>(Bytes), LoadedValue);
		if (!Result)
		{
			Ar.Fail(ToPayloadArchiveFailureCode(Result.Code), Result.Message);
			return;
		}
		Value = std::move(LoadedValue);
	}
}
