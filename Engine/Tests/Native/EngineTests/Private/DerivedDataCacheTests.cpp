#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataCache.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/DerivedDataCacheStorage.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/CompressedCacheValue.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryFormat.h"

#include <latch>
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#endif

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	static_assert(sizeof(FCacheBucket) == sizeof(void*));
	static_assert(sizeof(FCacheKey) == 24);
	static_assert(std::is_trivially_copyable_v<FCacheKey>);
	static_assert(sizeof(FCacheKeyProxy) == sizeof(FCacheKey));

	class FScopedCacheDirectory
	{
	public:
		explicit FScopedCacheDirectory(std::string_view Name)
			: Previous(FPaths::DerivedDataCacheDir())
			, Root(Testing::GetTestWorkDirectory() / Name)
		{
			Testing::RemoveTestWorkDirectory(Root);
			FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedCacheDirectory()
		{
			FPaths::SetDerivedDataCacheDirForTests(Previous);
			Testing::RemoveTestWorkDirectory(Root);
		}

		std::string Previous;
		std::filesystem::path Root;
	};

	auto MakeKey(const FCacheBucket& Bucket, char Fill) -> FCacheKey
	{
		return FCacheKey::FromString(Bucket, std::string(32, Fill));
	}

	auto Bytes(std::initializer_list<uint8> Values) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}
	auto RecordWith(const FCacheKey& Key, std::initializer_list<FSharedByteBuffer> Blocks) -> FCacheRecord
	{
		FBuildOutputBuilder Builder("Fixture.Output", 1);
		uint32 Index = 0;
		for (const auto& Block : Blocks) Builder.AddValue(FValueId::FromName("Block").MakeIndexed(Index++), Block);
		Builder.AddMeta(FValueId::FromName("Metadata"), MakeBuildMetadata(FSharedByteBuffer::Take(Bytes({7, 8}))).value());
		Builder.AddMessage(EBuildMessageSeverity::Warning, "fixture warning");
		return FCacheRecord::FromOutput(Key, std::move(Builder).Build().value()).value();
	}
	auto BlobKey(const FValue& Value) -> FCacheKey
	{ return FCacheKey::FromHash(FCacheBucket::FromString("Content"), Value.GetRawHash()); }
	auto EntryPath(const FScopedCacheDirectory& Directory, const FCacheKey& Key, std::string_view Namespace) -> std::filesystem::path
	{ return Directory.Root / Namespace / Key.GetBucket().ToString() / Key.ToString().substr(0, 2) / (Key.ToString() + ".bin"); }
}

TEST(FCacheValueTests, CompressesLargeValuesAndStoresSmallOrIncompressibleValuesRaw)
{
	FByteBuffer Random(8192); uint32 State = 123;
	for (auto& Byte : Random) { State ^= State << 13; State ^= State >> 17; State ^= State << 5; Byte = std::byte(State & 255); }
	const FValue Small(FSharedByteBuffer::Take(Bytes({1, 2, 3})));
	const FValue Noise(FSharedByteBuffer::Take(std::move(Random)));
	const FValue Large(FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{9})));
	for (const auto& Value : {FValue(FSharedByteBuffer{}), Small, Noise, Large})
	{
		auto Blob = FCompressedCacheValue::Compress(Value, 16384, 16 * 1024 * 1024); ASSERT_TRUE(Blob);
		EXPECT_EQ(Blob->IsCompressed(), Value.GetRawHash() == Large.GetRawHash());
		auto Loaded = FCompressedCacheValue::Load(Blob->GetEncoded(), 8192, 16384); ASSERT_TRUE(Loaded);
		auto Raw = Loaded->Decompress(1024 * 1024); ASSERT_TRUE(Raw);
		EXPECT_TRUE(Raw->HasData()); EXPECT_EQ(Raw->GetRawHash(), Value.GetRawHash());
		EXPECT_TRUE(std::ranges::equal(Raw->GetData().GetBytes(), Value.GetData().GetBytes()));
		if (!Loaded->IsCompressed() && Value.GetRawSize()) EXPECT_TRUE(Raw->GetData().SharesStorageWith(Blob->GetEncoded()));
	}
}

TEST(FCacheValueTests, BoundsWorkspaceAndValidatesRawIdentityAfterEnvelopeValidation)
{
	const FValue Value(FSharedByteBuffer::Take(FByteBuffer(4096, std::byte{3})));
	EXPECT_FALSE(FCompressedCacheValue::Compress(Value.RemoveData(), 8192, 8192));
	EXPECT_FALSE(FCompressedCacheValue::Compress(Value, 63, 8192));
	EXPECT_FALSE(FCompressedCacheValue::Compress(Value, 8192, 63));
	auto Raw = FCompressedCacheValue::Compress(Value, 8192, 8192); ASSERT_TRUE(Raw);
	EXPECT_FALSE(Raw->IsCompressed()); // Insufficient compression workspace uses bounded raw fallback.
	EXPECT_FALSE(FCompressedCacheValue::Load(Raw->GetEncoded(), 4095, 8192));
	EXPECT_FALSE(FCompressedCacheValue::Load(Raw->GetEncoded(), 4096, 64));
	FByteBuffer Body(Raw->GetEncoded().begin(), Raw->GetEncoded().end() - 16); Body[48] ^= std::byte{1};
	FBinaryWriter Writer; Writer.WriteBytes(Body); Writer.WriteHash128(FXxHash128::HashBuffer(Body));
	auto Forged = FCompressedCacheValue::Load(FSharedByteBuffer::Take(Writer.TakeBytes()), 4096, 8192); ASSERT_TRUE(Forged);
	EXPECT_FALSE(Forged->Decompress(8192));
	auto Compressed = FCompressedCacheValue::Compress(Value, 8192, 16 * 1024 * 1024); ASSERT_TRUE(Compressed);
	ASSERT_TRUE(Compressed->IsCompressed()); EXPECT_FALSE(Compressed->Decompress(4095));
	FByteBuffer Damaged(Compressed->GetEncoded().begin(), Compressed->GetEncoded().end()); Damaged.back() ^= std::byte{1};
	EXPECT_FALSE(FCompressedCacheValue::Load(FSharedByteBuffer::Take(std::move(Damaged)), 4096, 8192));
}

TEST(FCacheTests, ReusesContentAcrossBucketsAndRoundTripsCompleteOutputs)
{
	FScopedCacheDirectory Directory("ContentReuse");
	const auto A = MakeKey(FCacheBucket::FromString("FamilyA"), 'a');
	const auto B = MakeKey(FCacheBucket::FromString("FamilyB"), 'b');
	const auto Block = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{4}));
	const auto First = RecordWith(A, {Block, {}}), Second = RecordWith(B, {Block});
	ASSERT_TRUE(GetCache().Put({First})); ASSERT_TRUE(GetCache().Put({Second}));
	uint32 Blobs = 0;
	for (const auto& Entry : std::filesystem::recursive_directory_iterator(Directory.Root / "values/v1"))
		if (Entry.is_regular_file()) ++Blobs;
	EXPECT_EQ(Blobs, 2u); // One shared nonempty blob and one explicit empty blob.
	const auto Manifest = GetCacheStorage().Get({A, 4096, ECacheStorageNamespace::Records}); ASSERT_TRUE(Manifest); ASSERT_TRUE(*Manifest);
	auto Descriptor = FCacheRecord::DecodeManifest(A, **Manifest); ASSERT_TRUE(Descriptor);
	EXPECT_FALSE(Descriptor->ToOutput(A));
	EXPECT_FALSE(FCacheRecord::DecodeManifest(B, **Manifest));
	EXPECT_FALSE(FCacheRecord::DecodeManifest(A, **Manifest, {.MaximumTotalBytes = 100}));
	EXPECT_FALSE(Descriptor->WithValues(Second.GetValues()));
	for (const auto& Record : {First, Second})
	{
		auto Loaded = GetCache().Get({Record.GetKey()}); ASSERT_TRUE(Loaded); ASSERT_TRUE(*Loaded);
		auto Warm = (**Loaded).ToOutput(Record.GetKey()); ASSERT_TRUE(Warm);
		ASSERT_EQ(Warm->GetValues().size(), Record.GetValues().size());
		for (size_t I = 0; I < Warm->GetValues().size(); ++I)
		{
			EXPECT_TRUE(Warm->GetValues()[I].Value.HasData());
			EXPECT_EQ(Warm->GetValues()[I].Value.GetRawHash(), Record.GetValues()[I].Value.GetRawHash());
			EXPECT_TRUE(std::ranges::equal(Warm->GetValues()[I].Value.GetData().GetBytes(), Record.GetValues()[I].Value.GetData().GetBytes()));
		}
		EXPECT_EQ(GetBuildMetadataPayload(*Warm).GetSize(), 2u);
		EXPECT_EQ(Warm->GetMessages()[0].Text, "fixture warning");
	}
}

TEST(FCacheTests, RejectsMissingOrDamagedBlobsAndRepairsThemOnPut)
{
	FScopedCacheDirectory Directory("ContentRepair");
	const auto Key = MakeKey(FCacheBucket::FromString("Repair"), 'c');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1, 2, 3}))});
	ASSERT_TRUE(GetCache().Put({Record}));
	const auto Blob = BlobKey(Record.GetValues()[0].Value);
	ASSERT_TRUE(GetCacheStorage().Put({Blob, Bytes({9}), 4096, ECacheStorageNamespace::Values}));
	auto Damaged = GetCache().Get({Key}); ASSERT_FALSE(Damaged); EXPECT_EQ(Damaged.error().Code, ECacheError::Corrupt);
	ASSERT_TRUE(GetCache().Put({Record})); EXPECT_TRUE(GetCache().Get({Key}).value().has_value());
	ASSERT_TRUE(std::filesystem::remove(EntryPath(Directory, Blob, "values/v1")));
	auto Missing = GetCache().Get({Key}); ASSERT_FALSE(Missing); EXPECT_EQ(Missing.error().Code, ECacheError::Corrupt);
	ASSERT_TRUE(GetCache().Put({Record})); EXPECT_TRUE(GetCache().Get({Key}).value().has_value());
}

TEST(FCacheTests, FailedPublicationRetainsOrphansAndExistingReaders)
{
	FScopedCacheDirectory Directory("ContentPublicationFailure");
	const auto ExistingKey = MakeKey(FCacheBucket::FromString("Existing"), 'a');
	const auto Existing = RecordWith(ExistingKey, {FSharedByteBuffer::Take(Bytes({9}))});
	ASSERT_TRUE(GetCache().Put({Existing}));
	const auto Key = MakeKey(FCacheBucket::FromString("Interrupted"), 'd');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(Bytes({1})), FSharedByteBuffer::Take(Bytes({2}))});
	const auto Blocked = EntryPath(Directory, BlobKey(Record.GetValues()[1].Value), "values/v1");
	std::filesystem::create_directories(Blocked); // Fail the second blob after the first is published.
	const auto Failed = GetCache().Put({Record}); ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.error().Code, ECacheError::StorageFailure);
	EXPECT_FALSE(GetCache().Get({Key}).value().has_value());
	EXPECT_TRUE(std::filesystem::is_regular_file(EntryPath(Directory, BlobKey(Record.GetValues()[0].Value), "values/v1")));
	EXPECT_TRUE(GetCache().Get({ExistingKey}).value().has_value());
	const auto ManifestBlockedKey = MakeKey(FCacheBucket::FromString("ManifestBlocked"), 'e');
	std::filesystem::create_directories(EntryPath(Directory, ManifestBlockedKey, "records/v3"));
	EXPECT_FALSE(GetCache().Put({RecordWith(ManifestBlockedKey, {Existing.GetValues()[0].Value.GetData()})}));
	EXPECT_TRUE(GetCache().Get({ExistingKey}).value().has_value());
}

TEST(FCacheTests, EnforcesRawEncodedAndResidentBudgetsBeforePublishingManifest)
{
	FScopedCacheDirectory Directory("ContentBudgets");
	const auto Key = MakeKey(FCacheBucket::FromString("Budgets"), 'f');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(4096, std::byte{8}))});
	EXPECT_FALSE(GetCache().Put({Record, 8192, 100, 16 * 1024 * 1024}));
	EXPECT_FALSE(GetCache().Put({Record, 64, 8192, 16 * 1024 * 1024}));
	EXPECT_FALSE(GetCache().Put({Record, 8192, 8192, 4096}));
	const auto Backing = FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{3}));
	EXPECT_FALSE(GetCache().Put({RecordWith(Key, {Backing.MakeView(0, 1)}), 8192, 8192, 4096}));
	EXPECT_FALSE(GetCache().Get({Key}).value().has_value());
	ASSERT_TRUE(GetCache().Put({Record}));
	const auto Manifest = GetCacheStorage().Get({Key, 8192, ECacheStorageNamespace::Records}); ASSERT_TRUE(Manifest); ASSERT_TRUE(*Manifest);
	const auto Blob = GetCacheStorage().Get({BlobKey(Record.GetValues()[0].Value), 8192, ECacheStorageNamespace::Values}); ASSERT_TRUE(Blob); ASSERT_TRUE(*Blob);
	const auto AggregateLimit = (**Manifest).GetSize() + (**Blob).GetSize() - 1;
	EXPECT_FALSE(GetCache().Put({Record, AggregateLimit}));
	EXPECT_TRUE(GetCache().Get({Key}).value().has_value()); // Failed replacement preserves the published manifest.
	EXPECT_FALSE(GetCache().Get({Key, {}, AggregateLimit}));
	EXPECT_FALSE(GetCache().Get({Key, {.MaximumTotalBytes = 100}}));
	EXPECT_FALSE(GetCache().Get({Key, {}, 64}));
	EXPECT_FALSE(GetCache().Get({Key, {}, 8192, 4096}));
}

TEST(FCacheTests, ConcurrentContentWritersAndReadersSeeCompleteRecords)
{
	FScopedCacheDirectory Directory("ContentConcurrency");
	const auto Key = MakeKey(FCacheBucket::FromString("Concurrent"), '7');
	const auto Record = RecordWith(Key, {FSharedByteBuffer::Take(FByteBuffer(8192, std::byte{5}))});
	ASSERT_TRUE(GetCache().Put({Record}));
	std::latch Start(1); std::vector<std::thread> Threads;
	for (uint32 I = 0; I < 8; ++I) Threads.emplace_back([&] {
		Start.wait();
		for (uint32 Attempt = 0; Attempt < 8; ++Attempt)
		{
			EXPECT_TRUE(GetCache().Put({Record}));
			auto Loaded = GetCache().Get({Key}); ASSERT_TRUE(Loaded); ASSERT_TRUE(*Loaded);
			EXPECT_TRUE((**Loaded).ToOutput(Key));
		}
	});
	Start.count_down(); for (auto& Thread : Threads) Thread.join();
}

TEST(FCacheTests, StoresAndLoadsValidatedStructuredRecords)
{
	FScopedCacheDirectory Directory("StructuredCacheReadWrite");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Records"), '7');
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"),
		FSharedByteBuffer::Take(Bytes({1, 2, 3, 4}))));
	auto Output = std::move(Builder).Build(); ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
	ASSERT_TRUE(GetCache().Put({std::move(*Record), 4096}));

	auto Loaded = GetCache().Get({Key, {}, 4096}); ASSERT_TRUE(Loaded);
	ASSERT_TRUE(*Loaded);
	auto Warm = (**Loaded).ToOutput(Key); ASSERT_TRUE(Warm);
	const FValue* Value = Warm->FindValue(FValueId::FromName("Data"));
	ASSERT_NE(Value, nullptr);
	EXPECT_TRUE(std::ranges::equal(Value->GetData().GetBytes(), Bytes({1, 2, 3, 4})));
}

TEST(FCacheTests, RejectsCorruptStorageBeforeReturningARecord)
{
	FScopedCacheDirectory Directory("StructuredCacheCorruption");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Records"), '8');
	ASSERT_TRUE(GetCacheStorage().Put({Key, Bytes({9, 9, 9}), 4096, ECacheStorageNamespace::Records}));
	auto Loaded = GetCache().Get({Key, {}, 4096});
	ASSERT_FALSE(Loaded);
	EXPECT_EQ(Loaded.error().Code, ECacheError::Corrupt);
}

TEST(FCacheStorageTests, GetsAndAtomicallyReplacesCanonicalEntries)
{
	FScopedCacheDirectory Directory("CacheReadWrite");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'a');
	const Durin::FByteBuffer First = Bytes({1, 2, 3});
	const Durin::FByteBuffer Second = Bytes({4, 5});
	ASSERT_TRUE(Cache.Put({Key, First, 1024}));
	ASSERT_TRUE(Cache.Put({Key, Second, 1024}));

	const FCacheStorageGetResult Get = Cache.Get({Key, 1024});
	ASSERT_TRUE(Get);
	EXPECT_TRUE(std::ranges::equal((*Get)->GetBytes(), Second));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Directory.Root / "Test" / "Objects" / "aa" / (std::string(32, 'a') + ".bin")));
}

TEST(FCacheStorageTests, NormalizesRootAndContainsBinaryKeyPaths)
{
	FScopedCacheDirectory Directory("CacheNormalizedRoot");
	FPaths::SetDerivedDataCacheDirForTests(
		(Directory.Root / "unused" / "..").generic_string());
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Nested/Objects");
	const FByteBuffer Value = Bytes({7, 8});
	for (const FXxHash128 Hash : {FXxHash128{1, 0}, FXxHash128{0, 1},
		FXxHash128{0xffffffffffffffffull, 0xffffffffffffffffull}})
	{
		const FCacheKey Key = FCacheKey::FromHash(Bucket, Hash);
		ASSERT_TRUE(Cache.Put({Key, Value, 1024}));
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_TRUE(Result);
		EXPECT_TRUE(std::ranges::equal((*Result)->GetBytes(), Value));
		const std::string Text = Key.ToString();
		EXPECT_TRUE(std::filesystem::is_regular_file(Directory.Root / "Test"
			/ "Nested" / "Objects" / Text.substr(0, 2) / (Text + ".bin")));
	}
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "unused"));
}

TEST(FCacheStorageTests, ValidatesRequestsAndBoundsValuesTransactionally)
{
	FScopedCacheDirectory Directory("CacheValidation");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	std::string Error;
	EXPECT_FALSE(FCacheBucket::FromString("../escape", &Error).IsValid());
	EXPECT_FALSE(FCacheBucket::FromString("/absolute", &Error).IsValid());
	EXPECT_FALSE(FCacheBucket::FromString(std::string(64, 'a'), &Error).IsValid());
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheBucket InternedBucket = FCacheBucket::FromString("Test/Objects");
	EXPECT_EQ(InternedBucket, Bucket);
	EXPECT_EQ(InternedBucket.ToString().data(), Bucket.ToString().data());
	EXPECT_FALSE(FCacheKey::FromString(Bucket, "ABC", &Error).IsValid());
	EXPECT_FALSE(FCacheKey::FromString(
		Bucket, std::string(32, '0'), &Error).IsValid());
	EXPECT_EQ(Error, "Cache key must not be the zero identity.");
	const FCacheKey EmptyKey = FCacheKey::FromHash(Bucket, {});
	EXPECT_FALSE(EmptyKey.IsValid());
	EXPECT_TRUE(EmptyKey.ToString().empty());
	const FXxHash128 Hash{0x0123456789abcdefull, 0xfedcba9876543210ull};
	const FCacheKey HashKey = FCacheKey::FromHash(Bucket, Hash);
	EXPECT_TRUE(HashKey.IsValid());
	EXPECT_EQ(HashKey.GetBucket(), Bucket);
	EXPECT_EQ(HashKey.GetHash(), Hash);
	EXPECT_EQ(FCacheKey::FromString(Bucket, HashKey.ToString()), HashKey);
	const FCacheKey Key = MakeKey(Bucket, 'b');
	const Durin::FByteBuffer Value = Bytes({1, 2, 3, 4});
	{
		const auto Result = Cache.Put({Key, Value, 3});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::ValueTooLarge);
	}
	{
		const auto Result = Cache.Get({Key, 3});
		ASSERT_TRUE(Result);
		EXPECT_FALSE(*Result);
	}
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "escape.bin"));
}

TEST(FCacheStorageTests, CacheKeyProxyPreservesBinaryIdentity)
{
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'd');
	const FCacheKeyProxy Proxy(Key);
	EXPECT_TRUE(Proxy.IsValid());
	EXPECT_EQ(*Proxy.AsCacheKey(), Key);
	EXPECT_EQ(Proxy.AsCacheKey()->GetBucket(), Bucket);
	EXPECT_EQ(Proxy.ToString(), Key.ToString());
	FCacheKeyProxy Copy = Proxy;
	EXPECT_EQ(Copy, Proxy);
	FCacheKeyProxy Moved = std::move(Copy);
	EXPECT_EQ(Moved, Proxy);
	Copy = Moved;
	EXPECT_EQ(Copy, Proxy);
	EXPECT_FALSE(FCacheKeyProxy{}.AsCacheKey()->IsValid());
	EXPECT_NE(Key, MakeKey(FCacheBucket::FromString("Test/Other"), 'd'));
}

TEST(FCacheStorageTests, BucketIsPartOfTheRecordIdentity)
{
	FScopedCacheDirectory Directory("CacheBucketIdentity");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheKey FirstKey = MakeKey(
		FCacheBucket::FromString("Test/First"), '8');
	const FCacheKey SecondKey = MakeKey(
		FCacheBucket::FromString("Test/Second"), '8');
	const FByteBuffer First = Bytes({1});
	const FByteBuffer Second = Bytes({2});
	ASSERT_TRUE(Cache.Put({FirstKey, First, 1}));
	ASSERT_TRUE(Cache.Put({SecondKey, Second, 1}));
	const auto FirstGet = Cache.Get({FirstKey, 1});
	const auto SecondGet = Cache.Get({SecondKey, 1});
	ASSERT_TRUE(FirstGet);
	ASSERT_TRUE(SecondGet);
	EXPECT_TRUE(std::ranges::equal((*FirstGet)->GetBytes(), First));
	EXPECT_TRUE(std::ranges::equal((*SecondGet)->GetBytes(), Second));
}

TEST(FCacheStorageTests, InternsBucketNamesAcrossThreads)
{
	std::vector<std::future<FCacheBucket>> Operations;
	for (uint32 Index = 0; Index < 16; ++Index)
		Operations.push_back(std::async(std::launch::async, [] {
			return FCacheBucket::FromString("Concurrent/SharedBucket");
		}));
	const FCacheBucket Expected = Operations.front().get();
	ASSERT_TRUE(Expected.IsValid());
	for (size_t Index = 1; Index < Operations.size(); ++Index)
	{
		const FCacheBucket Bucket = Operations[Index].get();
		EXPECT_EQ(Bucket, Expected);
		EXPECT_EQ(Bucket.ToString().data(), Expected.ToString().data());
	}
}

TEST(FCacheStorageTests, BucketIdentityRemainsStableAsRegistryGrows)
{
	const FCacheBucket Original = FCacheBucket::FromString("Stable/GrowingRegistry");
	ASSERT_TRUE(Original.IsValid());
	const char* Identity = Original.ToString().data();
	for (uint32 Index = 0; Index < 512; ++Index)
		ASSERT_TRUE(FCacheBucket::FromString(std::format("Stable/GrowingRegistry{}", Index)).IsValid());
	const FCacheBucket Repeated = FCacheBucket::FromString("Stable/GrowingRegistry");
	EXPECT_EQ(Original.ToString(), "Stable/GrowingRegistry");
	EXPECT_EQ(Original.ToString().data(), Identity);
	EXPECT_EQ(Repeated, Original);
	EXPECT_EQ(Repeated.ToString().data(), Identity);
}

TEST(FCacheTests, RejectsDamagedRawAndCompressedRecordsWithoutAStorageEnvelope)
{
	FScopedCacheDirectory Directory("CacheContentValidation");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Objects"), 'f');
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"),
		FSharedByteBuffer::Take(Bytes({1, 2, 3, 4}))));
	auto Output = std::move(Builder).Build(); ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
	auto Raw = Record->Encode(4096); ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecord::CompressEncoded(*Raw, 4096); ASSERT_TRUE(Compressed);
	const auto Path = Directory.Root / "Test" / "Objects" / "ff"
		/ (std::string(32, 'f') + ".bin");
	for (const auto& Encoded : {*Raw, *Compressed})
	{
		ASSERT_TRUE(GetCacheStorage().Put({Key, Encoded, 4096}));
		auto Stored = FFileHelper::LoadFileToArray(Path); ASSERT_TRUE(Stored);
		EXPECT_TRUE(std::ranges::equal(*Stored, Encoded.GetBytes()));
		auto Loaded = FCacheRecord::Decode(Key, Encoded, {}, 4096); ASSERT_TRUE(Loaded);
		Stored->back() ^= std::byte{1};
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(*Stored, Path));
		auto Result = FCacheRecord::Decode(Key, FSharedByteBuffer::Take(*Stored), {}, 4096);
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::Corrupt);
		EXPECT_FALSE(Result.error().Diagnostic.empty());
		FByteBuffer Truncated(Encoded.begin(), Encoded.end() - 1);
		ASSERT_TRUE(GetCacheStorage().Put({Key, Truncated, 4096}));
		auto Short = FCacheRecord::Decode(Key, FSharedByteBuffer::Copy(Truncated), {}, 4096); ASSERT_FALSE(Short);
		EXPECT_EQ(Short.error().Code, ECacheError::Corrupt);
		FByteBuffer Appended(Encoded.begin(), Encoded.end());
		Appended.push_back(std::byte{0});
		ASSERT_TRUE(GetCacheStorage().Put({Key, Appended, 4096}));
		auto Long = FCacheRecord::Decode(Key, FSharedByteBuffer::Copy(Appended), {}, 4096); ASSERT_FALSE(Long);
		EXPECT_EQ(Long.error().Code, ECacheError::Corrupt);
	}
}

TEST(FCacheTests, RejectsLegacyStorageEnvelopesAndReplacesThemOnPut)
{
	FScopedCacheDirectory Directory("CacheLegacyEnvelope");
	const FCacheKey Key = MakeKey(FCacheBucket::FromString("Test/Objects"), 'f');
	FBuildOutputBuilder Builder("Fixture.Output", 1);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"),
		FSharedByteBuffer::Take(Bytes({1, 2, 3, 4}))));
	auto Output = std::move(Builder).Build(); ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
	auto Raw = Record->Encode(4096); ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecord::CompressEncoded(*Raw, 4096); ASSERT_TRUE(Compressed);
	FBinaryWriter Legacy;
	Legacy.WriteHeader({0x43444444, 1, 1});
	Legacy.WriteU64(Compressed->GetSize());
	Legacy.WriteHash128(FXxHash128::HashBuffer(Compressed->GetBytes()));
	Legacy.WriteBytes(Compressed->GetBytes());
	ASSERT_TRUE(GetCacheStorage().Put({Key, Legacy.GetBytes(), 4096}));
	auto Rejected = GetCache().Get({Key, {}, 4096}); ASSERT_TRUE(Rejected);
	EXPECT_FALSE(*Rejected); // Legacy namespaces are cold misses, never decoded.
	ASSERT_TRUE(GetCache().Put({*Record, 4096}));
	auto Loaded = GetCache().Get({Key, {}, 4096}); ASSERT_TRUE(Loaded);
	ASSERT_TRUE(*Loaded);
	EXPECT_TRUE((**Loaded).ToOutput(Key));
}

TEST(FCacheStorageTests, RejectsOversizedAndNonregularStoredEntries)
{
	FScopedCacheDirectory Directory("CacheStoredEntryValidation");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'c');
	const Durin::FByteBuffer Value(8, std::byte{1});
	ASSERT_TRUE(Cache.Put({Key, Value, 8}));
	{
		const auto Result = Cache.Get({Key, 4});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::ValueTooLarge);
	}
	const auto DirectoryEntry = Directory.Root / "Test" / "Objects" / "dd" / (std::string(32, 'd') + ".bin");
	std::filesystem::create_directories(DirectoryEntry);
	{
		const auto Result = Cache.Get({MakeKey(Bucket, 'd'), 8});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
}

TEST(FCacheStorageTests, ConcurrentSameKeyCallsPublishCompleteValues)
{
	FScopedCacheDirectory Directory("CacheConcurrency");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'e');
	const Durin::FByteBuffer First(128, std::byte{1});
	const Durin::FByteBuffer Second(128, std::byte{2});
	std::vector<std::thread> Threads;
	for (uint32 Index = 0; Index < 8; ++Index)
		Threads.emplace_back([&, Index] {
			const auto& Value = Index % 2 ? First : Second;
			EXPECT_TRUE(Cache.Put({Key, Value, 1024}));
			const FCacheStorageGetResult Get = Cache.Get({Key, 1024});
			ASSERT_TRUE(Get);
			EXPECT_TRUE(std::ranges::equal((*Get)->GetBytes(), First)
				|| std::ranges::equal((*Get)->GetBytes(), Second));
		});
	for (std::thread& Thread : Threads) Thread.join();
}

TEST(FCacheStorageTests, PutNeverEvictsExistingEntries)
{
	FScopedCacheDirectory Directory("CacheNoRequestEviction");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/NoEviction");
	const Durin::FByteBuffer Value(64, std::byte{5});
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FCacheKey Key = FCacheKey::FromString(Bucket,
			std::format("{:032x}", Index + 1));
		ASSERT_TRUE(Cache.Put({Key, Value, Value.size()}));
	}
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FCacheKey Key = FCacheKey::FromString(Bucket,
			std::format("{:032x}", Index + 1));
		EXPECT_TRUE(Cache.Get({Key, Value.size()}));
	}
}

TEST(FCacheStorageTests, UnrelatedBucketsAndKeysMakeConcurrentProgress)
{
	FScopedCacheDirectory Directory("CacheParallelBuckets");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket FirstBucket = FCacheBucket::FromString("Test/First");
	const FCacheBucket SecondBucket = FCacheBucket::FromString("Test/Second");
	const Durin::FByteBuffer Value(4096, std::byte{7});
	std::latch Start(1);
	std::vector<std::future<bool>> Operations;
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		Operations.push_back(std::async(std::launch::async, [&, Index] {
			Start.wait();
			const FCacheBucket& Bucket = Index % 2 ? FirstBucket : SecondBucket;
			const FCacheKey Key = FCacheKey::FromString(
				Bucket, std::format("{:032x}", Index + 1));
			if (!Cache.Put({Key, Value, Value.size()})) return false;
			const FCacheStorageGetResult Get = Cache.Get({Key, Value.size()});
			return Get && *Get && std::ranges::equal((*Get)->GetBytes(), Value);
		}));
	}
	Start.count_down();
	for (auto& Operation : Operations) EXPECT_TRUE(Operation.get());
}

TEST(FCacheStorageTests, BlockedStorageReturnsFailuresWithoutEscapingRoot)
{
	FScopedCacheDirectory Directory("CacheBlockedStorage");
	const std::filesystem::path Blocker = Directory.Root / "blocked";
	const Durin::FByteBuffer Value = Bytes({1, 2, 3});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Blocker));
	const FCacheBucket Bucket = FCacheBucket::FromString("blocked/Bucket");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	{
		const auto Result = Cache.Put({MakeKey(Bucket, '1'), Value, 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
	{
		const auto Result = Cache.Get({MakeKey(Bucket, '1'), 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
}

#if defined(_WIN32)
TEST(FCacheStorageTests, LockedEntryPreservesReadAndPublicationDiagnostics)
{
	FScopedCacheDirectory Directory("CacheLockedEntry");
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'a');
	const Durin::FByteBuffer Value = Bytes({1, 2, 3});
	{
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_TRUE(Result);
		EXPECT_FALSE(*Result);
	}
	ASSERT_TRUE(Cache.Put({Key, Value, 1024}));
	const Durin::FFilePath Path = Directory.Root / "Test" / "Objects"
		/ "aa" / (std::string(32, 'a') + ".bin");
	const HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, 0, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(File, INVALID_HANDLE_VALUE);
	{
		std::unique_ptr<void, decltype(&CloseHandle)> LockedFile(File, &CloseHandle);
		const auto Get = Cache.Get({Key, 1024});
		ASSERT_FALSE(Get);
		EXPECT_EQ(Get.error().Code, ECacheError::StorageFailure);
		EXPECT_NE(Get.error().Diagnostic.find("open for reading"), std::string::npos);
		EXPECT_NE(Get.error().Diagnostic.find(Path.generic_string()), std::string::npos);
		EXPECT_NE(Get.error().Diagnostic.find(std::format(":{}:", ERROR_SHARING_VIOLATION)), std::string::npos);
		const auto Put = Cache.Put({Key, Bytes({4, 5}), 1024});
		ASSERT_FALSE(Put);
		EXPECT_EQ(Put.error().Code, ECacheError::StorageFailure);
		EXPECT_NE(Put.error().Diagnostic.find("replace destination"), std::string::npos);
		EXPECT_NE(Put.error().Diagnostic.find(Path.generic_string()), std::string::npos);
	}
	const auto Get = Cache.Get({Key, 1024});
	ASSERT_TRUE(Get);
	EXPECT_TRUE(std::ranges::equal((*Get)->GetBytes(), Value));
}
#endif

TEST(FCacheStorageTests, SymlinkEntriesAreNeverRead)
{
	FScopedCacheDirectory Directory("CacheSymlinkSafety");
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, '9');
	const Durin::FByteBuffer Value = Bytes({9, 8, 7});
	const std::filesystem::path Outside = Directory.Root / "outside.bin";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Outside));
	const std::filesystem::path Link = Directory.Root / "Test" / "Objects"
		/ "99" / (std::string(32, '9') + ".bin");
	std::filesystem::create_directories(Link.parent_path());
	std::error_code Error;
	std::filesystem::create_symlink(Outside, Link, Error);
	if (Error) GTEST_SKIP() << "Host cannot create a test symlink: " << Error.message();
	FCacheStorage& Cache = DerivedData::GetCacheStorage();
	{
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
	EXPECT_TRUE(std::filesystem::exists(Outside));
	EXPECT_TRUE(std::filesystem::is_symlink(Link));
}
