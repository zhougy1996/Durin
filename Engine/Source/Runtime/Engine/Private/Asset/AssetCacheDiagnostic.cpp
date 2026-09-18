#include "Asset/AssetCacheDiagnostic.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataCache/DerivedDataCache.h"
#endif

namespace Durin
{
	auto FormatAssetCacheDiagnostics(const FAssetCacheDiagnostics& Diagnostic) -> std::string
	{
		const auto Read = FormatAssetCacheDiagnostic(Diagnostic.Read);
		const auto Write = FormatAssetCacheDiagnostic(Diagnostic.Write);
		constexpr size_t MaximumBytes = 2048;
		if (Read.empty()) return Write.substr(0, MaximumBytes);
		if (Write.empty()) return Read.substr(0, MaximumBytes);
		constexpr size_t Budget = (MaximumBytes - 13) / 2;
		return "Read: " + Read.substr(0, Budget) + "; Put: " + Write.substr(0, Budget);
	}

	auto FormatAssetCacheDiagnostic(const FAssetCacheDiagnostic& Diagnostic) -> std::string
	{
		if (Diagnostic.ArchiveCause) return Diagnostic.ArchiveCause->Message;
		switch (Diagnostic.Code)
		{
		case EAssetCacheError::None: return {};
		case EAssetCacheError::Decode: return "DDC payload is invalid or has trailing bytes.";
		case EAssetCacheError::Encode: return "DDC payload serialization failed.";
		case EAssetCacheError::Read:
#if DURIN_WITH_EDITOR
			if (Diagnostic.ReadCause) return Diagnostic.ReadCause->Diagnostic;
#endif
			return "DDC read failed.";
		case EAssetCacheError::Write:
#if DURIN_WITH_EDITOR
			if (Diagnostic.WriteCause) return Diagnostic.WriteCause->Diagnostic;
#endif
			return "DDC write failed.";
		}
		return {};
	}
}
