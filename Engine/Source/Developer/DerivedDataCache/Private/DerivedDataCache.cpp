#include "DerivedDataCache/DerivedDataCache.h"

#include "DerivedDataCacheStorage.h"
#include "FileSystemCacheBackend.h"

namespace Durin::DerivedData
{
	namespace
	{
		struct FCacheBucketRegistry
		{
			std::mutex Mutex;
			std::unordered_set<std::string> Entries;
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
			// Entries are never modified or erased; rehash preserves element addresses.
			const auto Entry = Registry.Entries.emplace(std::move(Name)).first;
			return Entry->c_str();
		}

		FCacheStorage GCacheStorage;

		class FRecordCache final : public ICache
		{
		public:
			auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult override
			{
				auto Stored = GCacheStorage.Get({Request.Key, Request.MaximumEncodedBytes});
				if (!Stored) return std::unexpected(std::move(Stored.error()));
				if (!*Stored) return std::optional<FCacheRecord>{};
				auto Record = FCacheRecord::Decode(Request.Key, std::move(**Stored),
					Request.OutputLimits, Request.MaximumEncodedBytes);
				if (!Record) return std::unexpected(std::move(Record.error()));
				return std::optional<FCacheRecord>{std::move(*Record)};
			}

			auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
			{
				if (!Request.Record.IsValid())
					return std::unexpected(FCacheError{ECacheError::InvalidRequest,
						"Cache put request has no record."});
				auto Encoded = Request.Record.Encode(Request.MaximumEncodedBytes);
				if (!Encoded) return std::unexpected(std::move(Encoded.error()));
				Encoded = FCacheRecord::CompressEncoded(
					*Encoded, Request.MaximumEncodedBytes);
				if (!Encoded) return std::unexpected(std::move(Encoded.error()));
				return GCacheStorage.Put({Request.Record.GetKey(), *Encoded,
					Request.MaximumEncodedBytes});
			}
		};

		FRecordCache GRecordCache;

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

	auto FCacheStorage::Get(const FCacheStorageGetRequest& Request) const
		-> FCacheStorageGetResult
	{
		return FFileSystemCacheBackend().Get(Request);
	}

	auto FCacheStorage::Put(const FCacheStoragePutRequest& Request) const
		-> FCacheStoragePutResult
	{
		return FFileSystemCacheBackend().Put(Request);
	}

	auto GetCacheStorage() -> FCacheStorage&
	{
		return GCacheStorage;
	}

	auto GetCache() -> ICache& { return GRecordCache; }
}
