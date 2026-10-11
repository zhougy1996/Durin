#include "DerivedDataCache/DerivedDataCache.h"

#include "DerivedDataCacheStorage.h"
#include "FileSystemCacheBackend.h"
#include "CompressedCacheValue.h"
#include "Misc/Paths.h"

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
				FCacheQueryStats Stats;
				const auto Start = std::chrono::steady_clock::now();
				struct FPublishStats
				{
					FCacheQueryStats& Stats; FCacheQueryStats* Destination;
					std::chrono::steady_clock::time_point Start;
					~FPublishStats()
					{
						Stats.DurationNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count();
						if (Destination) *Destination = Stats;
					}
				} Publish{Stats, Request.Stats, Start};
				const auto Canceled = [&] { return Request.ShouldCancel && Request.ShouldCancel(); };
				const auto CancelError = [] { return std::unexpected(FCacheError{ECacheError::Canceled, "Cache query canceled."}); };
				if (Request.Mode > ECacheGetMode::SelectedValues
					|| (Request.Mode != ECacheGetMode::SelectedValues && !Request.RequiredValues.empty())
					|| Request.RequiredValues.size() > std::min<uint32>(Request.OutputLimits.MaximumValues, 4096))
					return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Cache query mode or selection count is invalid."});
				std::vector<FValueId> Required(Request.RequiredValues.begin(), Request.RequiredValues.end());
				std::ranges::sort(Required);
				if (std::ranges::any_of(Required, [](const auto& Id) { return Id.IsNull(); })
					|| std::adjacent_find(Required.begin(), Required.end()) != Required.end())
					return std::unexpected(FCacheError{ECacheError::InvalidRequest, "Selected value IDs must be valid and unique."});
				if (Canceled()) return CancelError();
				auto Stored = GCacheStorage.Get({Request.Key,
					std::min(Request.MaximumEncodedBytes, Request.MaximumWorkingSetBytes), ECacheStorageNamespace::Records});
				if (Stored && *Stored) Stats.ReadBytes = Stats.PeakWorkingSetBytes = (**Stored).GetSize();
				if (Canceled()) return CancelError();
				if (!Stored) return std::unexpected(std::move(Stored.error()));
				if (!*Stored) return std::optional<FCacheRecord>{};
				const uint64 ManifestBytes = (**Stored).GetSize();
				auto Record = FCacheRecord::DecodeManifest(Request.Key, std::move(**Stored),
					Request.OutputLimits, Request.MaximumEncodedBytes);
				if (!Record) return std::unexpected(std::move(Record.error()));
				if (Canceled()) return CancelError();
				for (const auto& Id : Required)
					if (!Record->FindValue(Id)) return std::unexpected(FCacheError{ECacheError::MissingValue, "Required value is absent from manifest."});
				uint64 EncodedBytes = ManifestBytes, ResidentBytes = ManifestBytes;
				std::vector<FValueWithId> Values;
				for (const auto& Item : Record->GetValues())
				{
					if (Request.Mode == ECacheGetMode::MetadataOnly
						|| (Request.Mode == ECacheGetMode::SelectedValues && !std::ranges::binary_search(Required, Item.Id))) continue;
					if (Canceled()) return CancelError();
					if (EncodedBytes == Request.MaximumEncodedBytes || ResidentBytes == Request.MaximumWorkingSetBytes)
						return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "No budget remains for referenced value data."});
					const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("Content"), Item.Value.GetRawHash());
					uint64 ReadBytes = 0;
					auto Blob = GCacheStorage.Get({Key, std::min(Request.MaximumEncodedBytes - EncodedBytes,
						Request.MaximumWorkingSetBytes - ResidentBytes), ECacheStorageNamespace::Values, &ReadBytes});
					if (!Blob) return std::unexpected(std::move(Blob.error()));
					if (!*Blob) return std::unexpected(FCacheError{Request.Mode == ECacheGetMode::SelectedValues ? ECacheError::MissingValue : ECacheError::Corrupt, "Manifest references a missing value blob."});
					const uint64 BlobBytes = (**Blob).GetSize(); EncodedBytes += BlobBytes;
					Stats.ReadBytes += ReadBytes; ++Stats.ValuesRead;
					Stats.PeakWorkingSetBytes = std::max(Stats.PeakWorkingSetBytes, ResidentBytes + BlobBytes);
					if (Canceled()) return CancelError();
					auto Compressed = FCompressedCacheValue::Load(std::move(**Blob), Item.Value.GetRawSize(), BlobBytes);
					if (!Compressed) return std::unexpected(std::move(Compressed.error()));
					if (Compressed->GetRawHash() != Item.Value.GetRawHash() || Compressed->GetRawSize() != Item.Value.GetRawSize())
						return std::unexpected(FCacheError{ECacheError::Corrupt, "Blob identity differs from manifest."});
					if (Compressed->GetDecompressionWorkingBytes() > Request.MaximumWorkingSetBytes - ResidentBytes - BlobBytes)
						return std::unexpected(FCacheError{ECacheError::ValueTooLarge, "Decompression exceeds query workspace."});
					Stats.PeakWorkingSetBytes = std::max(Stats.PeakWorkingSetBytes, ResidentBytes + BlobBytes + Compressed->GetDecompressionWorkingBytes());
					auto Value = Compressed->Decompress(Request.MaximumWorkingSetBytes - ResidentBytes - BlobBytes);
					if (!Value) return std::unexpected(std::move(Value.error()));
					Stats.DecodedBytes += Value->GetRawSize();
					if (Canceled()) return CancelError();
					ResidentBytes += Compressed->IsCompressed() ? Value->GetRawSize() : BlobBytes;
					Values.push_back({Item.Id, std::move(*Value)});
				}
				auto Complete = Request.Mode == ECacheGetMode::FullRecord ? Record->WithValues(Values) : Record->WithSelectedValues(Values);
				if (!Complete) return std::unexpected(std::move(Complete.error()));
				return std::optional<FCacheRecord>{std::move(*Complete)};
			}
			catch (const std::bad_alloc&) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Cache read allocation failed."}); }

			auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
			try
			{
				const auto Canceled = [&] { return Request.ShouldCancel && Request.ShouldCancel(); };
				const auto CancelError = [] { return std::unexpected(FCacheError{ECacheError::Canceled, "Cache persistence canceled."}); };
				if (Canceled()) return CancelError();
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
					if (Canceled()) return CancelError();
					auto Blob = FCompressedCacheValue::Compress(Item.Value, Request.MaximumEncodedBytes - EncodedBytes,
						Request.MaximumWorkingSetBytes - ResidentBytes - Manifest->GetSize());
					if (!Blob) return std::unexpected(std::move(Blob.error()));
					if (Canceled()) return CancelError();
					EncodedBytes += Blob->GetEncoded().GetSize();
					const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("Content"), Item.Value.GetRawHash());
					auto Saved = GCacheStorage.Put({Key, Blob->GetEncoded().GetBytes(), Blob->GetEncoded().GetSize(), ECacheStorageNamespace::Values});
					if (!Saved) return std::unexpected(std::move(Saved.error()));
				}
				// All required content is complete before the manifest becomes visible.
				if (Canceled()) return CancelError();
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
		if (Request.ReadBytes) *Request.ReadBytes = 0;
		auto Read = [&]
		{
			auto Result = FFileSystemCacheBackend().Get(Request);
			if (Request.ReadBytes && Result && *Result) *Request.ReadBytes = (**Result).GetSize();
			return Result;
		};
		if (Request.Namespace != ECacheStorageNamespace::Values) return Read();
		static FCacheReadCoalescer Coalescer;
		const auto Root = FPaths::DerivedDataCacheDir(); const auto Bucket = Request.Key.GetBucket().ToString();
		const auto Identity = std::format("{}:{}|{}:{}|{}|{}", Root.size(), Root, Bucket.size(), Bucket,
			Request.Key.ToString(), Request.MaximumValueBytes);
		return Coalescer.Get(Identity, Read, Request.ReadBytes);
	}
	auto FCacheReadCoalescer::Get(std::string Key, std::function<FCacheStorageGetResult()> Read, uint64* ReadBytes) -> FCacheStorageGetResult
	{
		if (ReadBytes) *ReadBytes = 0;
		std::shared_future<FCacheStorageGetResult> Future;
		std::promise<FCacheStorageGetResult> Promise;
		bool Leader;
		{
			std::lock_guard Lock(Mutex);
			const auto Existing = Pending.find(Key); Leader = Existing == Pending.end();
			if (Leader) { Future = Promise.get_future().share(); Pending.emplace(Key, Future); }
			else { Future = Existing->second; ++CoalescedReads; }
		}
		if (Leader)
		{
			try
			{
				auto Result = Read();
				if (ReadBytes && Result && *Result) *ReadBytes = (**Result).GetSize();
				Promise.set_value(std::move(Result));
			}
			catch (...) { Promise.set_exception(std::current_exception()); }
			{ std::lock_guard Lock(Mutex); Pending.erase(Key); }
		}
		try { return Future.get(); }
		catch (...) { return std::unexpected(FCacheError{ECacheError::StorageFailure, "Coalesced cache read threw."}); }
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
