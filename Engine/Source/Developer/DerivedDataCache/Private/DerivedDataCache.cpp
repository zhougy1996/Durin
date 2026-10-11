#include "DerivedDataCache/DerivedDataCache.h"

#include "DerivedDataCacheStorage.h"
#include "FileSystemCacheBackend.h"
#include "CompressedCacheValue.h"

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
			try
			{
				auto Stored = GCacheStorage.Get({Request.Key,
					std::min(Request.MaximumEncodedBytes, Request.MaximumWorkingSetBytes), ECacheStorageNamespace::Records});
				if (!Stored) return std::unexpected(std::move(Stored.error()));
				if (!*Stored) return std::optional<FCacheRecord>{};
				const uint64 ManifestBytes = (**Stored).GetSize();
				auto Record = FCacheRecord::DecodeManifest(Request.Key, std::move(**Stored),
					Request.OutputLimits, Request.MaximumEncodedBytes);
				if (!Record) return std::unexpected(std::move(Record.error()));
				uint64 EncodedBytes = ManifestBytes, ResidentBytes = ManifestBytes;
				std::vector<FValueWithId> Values;
				for (const auto& Item : Record->GetValues())
				{
					if (EncodedBytes == Request.MaximumEncodedBytes || ResidentBytes == Request.MaximumWorkingSetBytes)
						return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "No budget remains for referenced value data."});
					const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("Content"), Item.Value.GetRawHash());
					auto Blob = GCacheStorage.Get({Key, std::min(Request.MaximumEncodedBytes - EncodedBytes,
						Request.MaximumWorkingSetBytes - ResidentBytes), ECacheStorageNamespace::Values});
					if (!Blob) return std::unexpected(std::move(Blob.error()));
					if (!*Blob) return std::unexpected(FCacheError{ECacheError::Corrupt, "Manifest references a missing value blob."});
					const uint64 BlobBytes = (**Blob).GetSize(); EncodedBytes += BlobBytes;
					auto Compressed = FCompressedCacheValue::Load(std::move(**Blob), Item.Value.GetRawSize(), BlobBytes);
					if (!Compressed) return std::unexpected(std::move(Compressed.error()));
					if (Compressed->GetRawHash() != Item.Value.GetRawHash() || Compressed->GetRawSize() != Item.Value.GetRawSize())
						return std::unexpected(FCacheError{ECacheError::Corrupt, "Blob identity differs from manifest."});
					auto Value = Compressed->Decompress(Request.MaximumWorkingSetBytes - ResidentBytes - BlobBytes);
					if (!Value) return std::unexpected(std::move(Value.error()));
					ResidentBytes += Compressed->IsCompressed() ? Value->GetRawSize() : BlobBytes;
					Values.push_back({Item.Id, std::move(*Value)});
				}
				auto Complete = Record->WithValues(Values);
				if (!Complete) return std::unexpected(std::move(Complete.error()));
				return std::optional<FCacheRecord>{std::move(*Complete)};
			}
			catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache read allocation failed."}); }

			auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
			try
			{
				if (!Request.Record.IsValid())
					return std::unexpected(FCacheError{ECacheError::InvalidRequest,
						"Cache put request has no record."});
				{
					auto Output = Request.Record.ToOutput(Request.Record.GetKey());
					if (!Output) return std::unexpected(std::move(Output.error()));
					if (auto Valid = Output->CheckLimits({.MaximumTotalBytes = Request.MaximumRawBytes}); !Valid)
						return std::unexpected(FCacheError{ECacheError::ValueTooLarge, std::move(Valid.error())});
				}
				uint64 ResidentBytes = 0;
				std::vector<FSharedByteBuffer> Backings;
				auto Retain = [&](FSharedByteBuffer Buffer) -> bool
				{
					if (std::ranges::any_of(Backings, [&](const auto& Other) { return Buffer.SharesStorageWith(Other); })) return true;
					if (Buffer.GetRetainedCapacityBytes() > Request.MaximumWorkingSetBytes - ResidentBytes) return false;
					ResidentBytes += Buffer.GetRetainedCapacityBytes(); Backings.push_back(std::move(Buffer)); return true;
				};
				for (const auto& Item : Request.Record.GetValues())
					if (!Retain(Item.Value.GetData())) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Resident value storage exceeds persistence workspace."});
				for (const auto& Meta : Request.Record.GetMetadata())
					if (!Retain(Meta.Object.GetBytes())) return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Resident metadata storage exceeds persistence workspace."});
				for (const auto& Message : Request.Record.GetMessages())
				{
					if (Message.Text.capacity() > Request.MaximumWorkingSetBytes - ResidentBytes)
						return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Resident messages exceed persistence workspace."});
					ResidentBytes += Message.Text.capacity();
				}
				auto Manifest = Request.Record.EncodeManifest(std::min(Request.MaximumEncodedBytes, Request.MaximumWorkingSetBytes - ResidentBytes));
				if (!Manifest) return std::unexpected(std::move(Manifest.error()));
				uint64 EncodedBytes = Manifest->GetSize();
				for (const auto& Item : Request.Record.GetValues())
				{
					auto Blob = FCompressedCacheValue::Compress(Item.Value, Request.MaximumEncodedBytes - EncodedBytes,
						Request.MaximumWorkingSetBytes - ResidentBytes - Manifest->GetSize());
					if (!Blob) return std::unexpected(std::move(Blob.error()));
					EncodedBytes += Blob->GetEncoded().GetSize();
					const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("Content"), Item.Value.GetRawHash());
					auto Saved = GCacheStorage.Put({Key, Blob->GetEncoded().GetBytes(), Blob->GetEncoded().GetSize(), ECacheStorageNamespace::Values});
					if (!Saved) return std::unexpected(std::move(Saved.error()));
				}
				// All required content is complete before the manifest becomes visible.
				return GCacheStorage.Put({Request.Record.GetKey(), Manifest->GetBytes(), Manifest->GetSize(), ECacheStorageNamespace::Records});
			}
			catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache write allocation failed."}); }
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
