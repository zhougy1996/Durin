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
	// entire remaining bounded region and publishes only a successfully parsed candidate.
	template<typename Candidate, typename BuildFn, typename ParseFn, typename CommitFn>
	auto SerializeBoundedArchivePayload(
		FArchive& Ar,
		const FBoundedArchivePayloadPolicy& Policy,
		BuildFn&& Build,
		ParseFn&& Parse,
		CommitFn&& Commit) -> void
	{
		if (Ar.HasError()) return;
		if (Ar.IsSaving())
		{
			std::vector<uint8> Bytes;
			std::string Error;
			if (!Build(Bytes, Error))
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

		Candidate LoadedCandidate;
		const FPayloadDecodeResult Result = Parse(
			std::span<const uint8>(Bytes), LoadedCandidate);
		if (!Result)
		{
			Ar.Fail(ToPayloadArchiveFailureCode(Result.Code), Result.Message);
			return;
		}
		Commit(std::move(LoadedCandidate));
	}
}
