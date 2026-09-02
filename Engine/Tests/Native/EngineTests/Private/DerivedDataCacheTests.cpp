#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <latch>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;

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

	auto MakeKey(char Fill) -> FCacheKey
	{
		return FCacheKey::FromString(std::string(32, Fill));
	}

	auto Bytes(std::initializer_list<uint8> Values) -> Durin::FByteArray
	{
		Durin::FByteArray Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}
}

TEST(FDerivedDataCacheTests, GetsAndAtomicallyReplacesCanonicalEntries)
{
	FScopedCacheDirectory Directory("CacheReadWrite");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('a');
	const Durin::FByteArray First = Bytes({1, 2, 3});
	const Durin::FByteArray Second = Bytes({4, 5});
	ASSERT_TRUE(Cache.Put({Bucket, Key, First, 1024}));
	ASSERT_TRUE(Cache.Put({Bucket, Key, Second, 1024}));

	const FCacheGetResult Get = Cache.Get({Bucket, Key, 1024});
	ASSERT_EQ(Get.Status, ECacheGetStatus::Hit);
	EXPECT_TRUE(std::ranges::equal(Get.Value.GetBytes(), Second));
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
	EXPECT_FALSE(FCacheKey::FromString("ABC", &Error).IsValid());
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('b');
	const Durin::FByteArray Value = Bytes({1, 2, 3, 4});
	EXPECT_EQ(Cache.Put({Bucket, Key, Value, 3}).Status, ECachePutStatus::ValueTooLarge);
	EXPECT_EQ(Cache.Get({Bucket, Key, 3}).Status, ECacheGetStatus::Miss);
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "escape.bin"));
}

TEST(FDerivedDataCacheTests, RejectsOversizedAndNonregularStoredEntries)
{
	FScopedCacheDirectory Directory("CacheStoredEntryValidation");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('c');
	const Durin::FByteArray Value(8, std::byte{1});
	ASSERT_TRUE(Cache.Put({Bucket, Key, Value, 8}));
	EXPECT_EQ(Cache.Get({Bucket, Key, 4}).Status, ECacheGetStatus::ValueTooLarge);
	const auto DirectoryEntry = Directory.Root / "Test" / "Objects" / "dd" / (std::string(32, 'd') + ".bin");
	std::filesystem::create_directories(DirectoryEntry);
	EXPECT_EQ(Cache.Get({Bucket, MakeKey('d'), 8}).Status, ECacheGetStatus::StorageFailure);
}

TEST(FDerivedDataCacheTests, ConcurrentSameKeyCallsPublishCompleteValues)
{
	FScopedCacheDirectory Directory("CacheConcurrency");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('e');
	const Durin::FByteArray First(128, std::byte{1});
	const Durin::FByteArray Second(128, std::byte{2});
	std::vector<std::thread> Threads;
	for (uint32 Index = 0; Index < 8; ++Index)
		Threads.emplace_back([&, Index] {
			const auto& Value = Index % 2 ? First : Second;
			EXPECT_TRUE(Cache.Put({Bucket, Key, Value, 1024}));
			const FCacheGetResult Get = Cache.Get({Bucket, Key, 1024});
			ASSERT_EQ(Get.Status, ECacheGetStatus::Hit);
			EXPECT_TRUE(std::ranges::equal(Get.Value.GetBytes(), First)
				|| std::ranges::equal(Get.Value.GetBytes(), Second));
		});
	for (std::thread& Thread : Threads) Thread.join();
}

TEST(FDerivedDataCacheTests, PutNeverEvictsExistingEntries)
{
	FScopedCacheDirectory Directory("CacheNoRequestEviction");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/NoEviction");
	const Durin::FByteArray Value(64, std::byte{5});
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FCacheKey Key = FCacheKey::FromString(
			std::format("{:032x}", Index + 1));
		ASSERT_TRUE(Cache.Put({Bucket, Key, Value, Value.size()}));
	}
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		const FCacheKey Key = FCacheKey::FromString(
			std::format("{:032x}", Index + 1));
		EXPECT_EQ(Cache.Get({Bucket, Key, Value.size()}).Status,
			ECacheGetStatus::Hit);
	}
}

TEST(FDerivedDataCacheTests, UnrelatedBucketsAndKeysMakeConcurrentProgress)
{
	FScopedCacheDirectory Directory("CacheParallelBuckets");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	const FCacheBucket FirstBucket = FCacheBucket::FromString("Test/First");
	const FCacheBucket SecondBucket = FCacheBucket::FromString("Test/Second");
	const Durin::FByteArray Value(4096, std::byte{7});
	std::latch Start(1);
	std::vector<std::future<bool>> Operations;
	for (uint32 Index = 0; Index < 16; ++Index)
	{
		Operations.push_back(std::async(std::launch::async, [&, Index] {
			Start.wait();
			const FCacheBucket& Bucket = Index % 2 ? FirstBucket : SecondBucket;
			const FCacheKey Key = FCacheKey::FromString(std::format("{:032x}", Index + 1));
			if (!Cache.Put({Bucket, Key, Value, Value.size()})) return false;
			const FCacheGetResult Get = Cache.Get({Bucket, Key, Value.size()});
			return Get && std::ranges::equal(Get.Value.GetBytes(), Value);
		}));
	}
	Start.count_down();
	for (auto& Operation : Operations) EXPECT_TRUE(Operation.get());
}

TEST(FDerivedDataCacheTests, BlockedStorageReturnsFailuresWithoutEscapingRoot)
{
	FScopedCacheDirectory Directory("CacheBlockedStorage");
	const std::filesystem::path Blocker = Directory.Root / "blocked";
	const Durin::FByteArray Value = Bytes({1, 2, 3});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Blocker));
	const FCacheBucket Bucket = FCacheBucket::FromString("blocked/Bucket");
	FDerivedDataCache& Cache = DerivedData::GetCache();
	EXPECT_EQ(Cache.Put({Bucket, MakeKey('1'), Value, 1024}).Status,
		ECachePutStatus::StorageFailure);
	EXPECT_EQ(Cache.Get({Bucket, MakeKey('1'), 1024}).Status,
		ECacheGetStatus::StorageFailure);
}

TEST(FDerivedDataCacheTests, SymlinkEntriesAreNeverRead)
{
	FScopedCacheDirectory Directory("CacheSymlinkSafety");
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('9');
	const Durin::FByteArray Value = Bytes({9, 8, 7});
	const std::filesystem::path Outside = Directory.Root / "outside.bin";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, Outside));
	const std::filesystem::path Link = Directory.Root / "Test" / "Objects"
		/ "99" / (std::string(32, '9') + ".bin");
	std::filesystem::create_directories(Link.parent_path());
	std::error_code Error;
	std::filesystem::create_symlink(Outside, Link, Error);
	if (Error) GTEST_SKIP() << "Host cannot create a test symlink: " << Error.message();
	FDerivedDataCache& Cache = DerivedData::GetCache();
	EXPECT_EQ(Cache.Get({Bucket, Key, 1024}).Status,
		ECacheGetStatus::StorageFailure);
	EXPECT_TRUE(std::filesystem::exists(Outside));
	EXPECT_TRUE(std::filesystem::is_symlink(Link));
}
