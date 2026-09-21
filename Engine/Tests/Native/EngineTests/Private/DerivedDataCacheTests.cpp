#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataCache.h"
#include "Asset/DerivedDataCacheKeyProxy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

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
}

TEST(FDerivedDataCacheTests, GetsAndAtomicallyReplacesCanonicalEntries)
{
	FScopedCacheDirectory Directory("CacheReadWrite");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'a');
	const Durin::FByteBuffer First = Bytes({1, 2, 3});
	const Durin::FByteBuffer Second = Bytes({4, 5});
	ASSERT_TRUE(Cache.Put({Key, First, 1024}));
	ASSERT_TRUE(Cache.Put({Key, Second, 1024}));

	const FCacheGetResult Get = Cache.Get({Key, 1024});
	ASSERT_TRUE(Get);
	EXPECT_TRUE(std::ranges::equal(Get->GetBytes(), Second));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Directory.Root / "Test" / "Objects" / "aa" / (std::string(32, 'a') + ".bin")));
}

TEST(FDerivedDataCacheTests, ValidatesRequestsAndBoundsValuesTransactionally)
{
	FScopedCacheDirectory Directory("CacheValidation");
	FDerivedDataCache& Cache = DerivedData::GetCache();
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
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::Miss);
	}
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "escape.bin"));
}

TEST(FDerivedDataCacheTests, CacheKeyProxyPreservesBinaryIdentity)
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

TEST(FDerivedDataCacheTests, BucketIsPartOfTheRecordIdentity)
{
	FScopedCacheDirectory Directory("CacheBucketIdentity");
	FDerivedDataCache& Cache = DerivedData::GetCache();
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
	EXPECT_TRUE(std::ranges::equal(FirstGet->GetBytes(), First));
	EXPECT_TRUE(std::ranges::equal(SecondGet->GetBytes(), Second));
}

TEST(FDerivedDataCacheTests, InternsBucketNamesAcrossThreads)
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

TEST(FDerivedDataCacheTests, RejectsContentThatDoesNotMatchStoredHash)
{
	FScopedCacheDirectory Directory("CacheContentValidation");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'f');
	const FByteBuffer Value = Bytes({1, 2, 3, 4});
	ASSERT_TRUE(Cache.Put({Key, Value, Value.size()}));
	const std::filesystem::path Path = Directory.Root / "Test" / "Objects"
		/ "ff" / (std::string(32, 'f') + ".bin");
	FByteBuffer Stored;
	auto StoredRead = FFileHelper::LoadFileToArray(Path);
	ASSERT_TRUE(StoredRead) << StoredRead.error().ToString();
	Stored = std::move(*StoredRead);
	ASSERT_GT(Stored.size(), Value.size());
	Stored.back() ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Stored, Path));
	const FCacheGetResult Result = Cache.Get({Key, Value.size()});
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, ECacheError::Corrupt);
	EXPECT_FALSE(Result.error().Diagnostic.empty());
}

TEST(FDerivedDataCacheTests, RejectsOversizedAndNonregularStoredEntries)
{
	FScopedCacheDirectory Directory("CacheStoredEntryValidation");
	FDerivedDataCache& Cache = DerivedData::GetCache();
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

TEST(FDerivedDataCacheTests, ConcurrentSameKeyCallsPublishCompleteValues)
{
	FScopedCacheDirectory Directory("CacheConcurrency");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'e');
	const Durin::FByteBuffer First(128, std::byte{1});
	const Durin::FByteBuffer Second(128, std::byte{2});
	std::vector<std::thread> Threads;
	for (uint32 Index = 0; Index < 8; ++Index)
		Threads.emplace_back([&, Index] {
			const auto& Value = Index % 2 ? First : Second;
			EXPECT_TRUE(Cache.Put({Key, Value, 1024}));
			const FCacheGetResult Get = Cache.Get({Key, 1024});
			ASSERT_TRUE(Get);
			EXPECT_TRUE(std::ranges::equal(Get->GetBytes(), First)
				|| std::ranges::equal(Get->GetBytes(), Second));
		});
	for (std::thread& Thread : Threads) Thread.join();
}

TEST(FDerivedDataCacheTests, PutNeverEvictsExistingEntries)
{
	FScopedCacheDirectory Directory("CacheNoRequestEviction");
	FDerivedDataCache& Cache = DerivedData::GetCache();
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

TEST(FDerivedDataCacheTests, UnrelatedBucketsAndKeysMakeConcurrentProgress)
{
	FScopedCacheDirectory Directory("CacheParallelBuckets");
	FDerivedDataCache& Cache = DerivedData::GetCache();
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
			const FCacheGetResult Get = Cache.Get({Key, Value.size()});
			return Get && std::ranges::equal(Get->GetBytes(), Value);
		}));
	}
	Start.count_down();
	for (auto& Operation : Operations) EXPECT_TRUE(Operation.get());
}

TEST(FDerivedDataCacheTests, BlockedStorageReturnsFailuresWithoutEscapingRoot)
{
	FScopedCacheDirectory Directory("CacheBlockedStorage");
	const std::filesystem::path Blocker = Directory.Root / "blocked";
	const Durin::FByteBuffer Value = Bytes({1, 2, 3});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Blocker));
	const FCacheBucket Bucket = FCacheBucket::FromString("blocked/Bucket");
	FDerivedDataCache& Cache = DerivedData::GetCache();
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
TEST(FDerivedDataCacheTests, LockedEntryPreservesReadAndPublicationDiagnostics)
{
	FScopedCacheDirectory Directory("CacheLockedEntry");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey(Bucket, 'a');
	const Durin::FByteBuffer Value = Bytes({1, 2, 3});
	{
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::Miss);
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
	EXPECT_TRUE(std::ranges::equal(Get->GetBytes(), Value));
}
#endif

TEST(FDerivedDataCacheTests, SymlinkEntriesAreNeverRead)
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
	FDerivedDataCache& Cache = DerivedData::GetCache();
	{
		const auto Result = Cache.Get({Key, 1024});
		ASSERT_FALSE(Result);
		EXPECT_EQ(Result.error().Code, ECacheError::StorageFailure);
	}
	EXPECT_TRUE(std::filesystem::exists(Outside));
	EXPECT_TRUE(std::filesystem::is_symlink(Link));
}
