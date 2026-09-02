#include "DerivedDataCache/DerivedDataCache.h"

#include "FileSystemCacheBackend.h"

namespace Durin::DerivedData
{
	namespace
	{
		// Bucket locks are retained only while an operation is active. The short
		// registry lock never covers backend IO.
		std::mutex GBucketLockRegistryMutex;
		std::unordered_map<std::string, std::weak_ptr<std::shared_mutex>> GBucketLocks;
		FDerivedDataCache GDerivedDataCache;

		auto AcquireBucketLock(const FCacheBucket& Bucket) -> std::shared_ptr<std::shared_mutex>
		{
			std::lock_guard RegistryLock(GBucketLockRegistryMutex);
			for (auto It = GBucketLocks.begin(); It != GBucketLocks.end();)
			{
				It = It->second.expired() ? GBucketLocks.erase(It) : std::next(It);
			}
			auto& WeakLock = GBucketLocks[std::string(Bucket.ToString())];
			std::shared_ptr<std::shared_mutex> Lock = WeakLock.lock();
			if (!Lock)
			{
				Lock = std::make_shared<std::shared_mutex>();
				WeakLock = Lock;
			}
			return Lock;
		}

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
		const std::shared_ptr BucketLock = AcquireBucketLock(Request.Bucket);
		std::shared_lock Lock(*BucketLock);
		return FFileSystemCacheBackend().Get(Request);
	}

	auto FDerivedDataCache::Put(const FCachePutRequest& Request) const -> FCachePutResult
	{
		const std::shared_ptr BucketLock = AcquireBucketLock(Request.Bucket);
		std::shared_lock Lock(*BucketLock);
		return FFileSystemCacheBackend().Put(Request);
	}

	auto GetCache() -> FDerivedDataCache&
	{
		return GDerivedDataCache;
	}
}
