#include "DerivedDataCache/DerivedDataCache.h"

#include "FileSystemCacheBackend.h"

namespace Durin::DerivedData
{
	namespace
	{
		std::mutex GCacheMutex;
		FDerivedDataCache GDerivedDataCache;

		auto SetError(std::string* OutError, std::string Message) -> void
		{
			if (OutError) *OutError = std::move(Message);
		}
	}

	auto FCacheBucket::FromString(std::string_view InValue, std::string* OutError)
		-> FCacheBucket
	{
		FCacheBucket Result;
		const std::filesystem::path Path(InValue);
		if (Path.empty() || Path.is_absolute() || Path.has_root_path()
			|| Path.lexically_normal() != Path
			|| std::ranges::any_of(Path, [](const std::filesystem::path& Part) {
				return Part.empty() || Part == "." || Part == "..";
			}))
		{
			SetError(OutError, "Cache bucket must be a canonical relative path.");
			return Result;
		}
		Result.Value = Path.generic_string();
		if (OutError) OutError->clear();
		return Result;
	}

	auto FCacheKey::FromString(std::string_view InValue, std::string* OutError)
		-> FCacheKey
	{
		FCacheKey Result;
		if (InValue.size() != 32 || !std::ranges::all_of(InValue, [](char Character) {
			return Character >= '0' && Character <= '9'
				|| Character >= 'a' && Character <= 'f';
		}))
		{
			SetError(OutError, "Cache key must be a lowercase 128-bit hexadecimal identity.");
			return Result;
		}
		Result.Value.assign(InValue);
		if (OutError) OutError->clear();
		return Result;
	}

	auto FDerivedDataCache::Get(const FCacheGetRequest& Request) const -> FCacheGetResult
	{
		std::lock_guard Lock(GCacheMutex);
		return FFileSystemCacheBackend().Get(Request);
	}

	auto FDerivedDataCache::Put(const FCachePutRequest& Request) const -> FCachePutResult
	{
		std::lock_guard Lock(GCacheMutex);
		return FFileSystemCacheBackend().Put(Request);
	}

	auto FDerivedDataCache::Trim(const FCacheTrimRequest& Request) const -> FCacheTrimResult
	{
		std::lock_guard Lock(GCacheMutex);
		return FFileSystemCacheBackend().TrimToBudget(Request);
	}

	auto GetDerivedDataCache() -> FDerivedDataCache&
	{
		return GDerivedDataCache;
	}
}
