#pragma once

#include "EngineAPI.h"
#include "DerivedDataCacheKeyProxy.h"
#include "Serialization/Archive.h"

namespace Durin
{
	namespace DerivedData { struct FCacheGetResult; struct FCachePutResult; }
	enum class EAssetCacheError : uint8 { None, Read, Write, Decode, Encode };
	// Cache outcomes remain independent of successful in-memory asset publication.
	struct FAssetCacheDiagnostic
	{
		EAssetCacheError Code = EAssetCacheError::None;
		FCacheKeyProxy Key;
		uint64 MaximumValueBytes = 0;
		uint64 DurationNanoseconds = 0;
		std::optional<FArchiveFailure> ArchiveCause;
		std::shared_ptr<const DerivedData::FCacheGetResult> ReadCause;
		std::shared_ptr<const DerivedData::FCachePutResult> WriteCause;
	};
	struct FAssetCacheDiagnostics
	{
		FAssetCacheDiagnostic Read;
		FAssetCacheDiagnostic Write;
	};
	ENGINE_API auto FormatAssetCacheDiagnostics(const FAssetCacheDiagnostics& Diagnostic) -> std::string;
	ENGINE_API auto FormatAssetCacheDiagnostic(const FAssetCacheDiagnostic& Diagnostic) -> std::string;
}
