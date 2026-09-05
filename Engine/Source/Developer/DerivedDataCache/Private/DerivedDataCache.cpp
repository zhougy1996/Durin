#include "DerivedDataCache/DerivedDataCache.h"

#include "FileSystemCacheBackend.h"

namespace Durin::DerivedData
{
	namespace
	{
		struct FCacheBucketEntry
		{
			explicit FCacheBucketEntry(std::string InName)
				: Name(std::move(InName))
			{
			}

			std::string Name;
		};

		struct FCacheBucketRegistry
		{
			std::mutex Mutex;
			std::unordered_map<std::string, std::unique_ptr<FCacheBucketEntry>> Entries;
		};

		// Bucket identities remain valid until process exit, including while other
		// static objects are being destroyed.
		auto GetCacheBucketRegistry() -> FCacheBucketRegistry&
		{
			static FCacheBucketRegistry* Registry = new FCacheBucketRegistry;
			return *Registry;
		}

		auto InternCacheBucket(std::string Name) -> const char*
		{
			FCacheBucketRegistry& Registry = GetCacheBucketRegistry();
			std::lock_guard Lock(Registry.Mutex);
			if (const auto Found = Registry.Entries.find(Name);
				Found != Registry.Entries.end()) return Found->second->Name.c_str();
			auto Entry = std::make_unique<FCacheBucketEntry>(std::move(Name));
			const char* Identity = Entry->Name.c_str();
			const std::string LookupKey = Entry->Name;
			Registry.Entries.emplace(LookupKey, std::move(Entry));
			return Identity;
		}

		// Bucket locks are retained only while an operation is active. The short
		// registry lock never covers backend IO.
		std::mutex GBucketLockRegistryMutex;
		std::unordered_map<const char*, std::weak_ptr<std::shared_mutex>> GBucketLocks;
		FDerivedDataCache GDerivedDataCache;

		auto AcquireBucketLock(const FCacheBucket& Bucket) -> std::shared_ptr<std::shared_mutex>
		{
			std::lock_guard RegistryLock(GBucketLockRegistryMutex);
			for (auto It = GBucketLocks.begin(); It != GBucketLocks.end();)
			{
				It = It->second.expired() ? GBucketLocks.erase(It) : std::next(It);
			}
			auto& WeakLock = GBucketLocks[Bucket.ToString().data()];
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
		const std::string Value = Path.generic_string();
		if (Value.size() > FCacheBucket::MaximumNameLength)
		{
			SetError(OutError, "Cache bucket exceeds its maximum name length.");
			return Result;
		}
		Result.Name = InternCacheBucket(Value);
		if (OutError) OutError->clear();
		return Result;
	}

	auto FCacheBucket::ToString() const -> std::string_view
	{
		return Name ? std::string_view(Name) : std::string_view{};
	}

	auto FCacheKey::FromString(FCacheBucket InBucket, std::string_view InValue,
		std::string* OutError)
		-> FCacheKey
	{
		if (!InBucket.IsValid())
		{
			SetError(OutError, "Cache key bucket is invalid.");
			return {};
		}
		if (InValue.size() != 32 || !std::ranges::all_of(InValue, [](char Character) {
			return Character >= '0' && Character <= '9'
				|| Character >= 'a' && Character <= 'f';
		}))
		{
			SetError(OutError, "Cache key must be a lowercase 128-bit hexadecimal identity.");
			return {};
		}
		FCacheKey Result = FromHash(
			std::move(InBucket), FXxHash128::FromString(InValue));
		if (!Result.IsValid())
		{
			SetError(OutError, "Cache key must not be the zero identity.");
			return {};
		}
		if (OutError) OutError->clear();
		return Result;
	}

	auto FCacheKey::FromHash(FCacheBucket InBucket, FXxHash128 InHash) -> FCacheKey
	{
		FCacheKey Result;
		if (InBucket.IsValid() && !InHash.IsZero())
		{
			Result.Bucket = std::move(InBucket);
			Result.Hash = InHash;
		}
		return Result;
	}

	auto FCacheKey::ToString() const -> std::string
	{
		return IsValid() ? Hash.ToString() : std::string{};
	}

	auto FDerivedDataCache::Get(const FCacheGetRequest& Request) const -> FCacheGetResult
	{
		const std::shared_ptr BucketLock = AcquireBucketLock(Request.Key.GetBucket());
		std::shared_lock Lock(*BucketLock);
		return FFileSystemCacheBackend().Get(Request);
	}

	auto FDerivedDataCache::Put(const FCachePutRequest& Request) const -> FCachePutResult
	{
		const std::shared_ptr BucketLock = AcquireBucketLock(Request.Key.GetBucket());
		std::shared_lock Lock(*BucketLock);
		return FFileSystemCacheBackend().Put(Request);
	}

	auto GetCache() -> FDerivedDataCache&
	{
		return GDerivedDataCache;
	}
}
