#include <gtest/gtest.h>

#include "DerivedDataCache/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

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

	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}
}

TEST(FDerivedDataCacheTests, GetsAndAtomicallyReplacesCanonicalEntries)
{
	FScopedCacheDirectory Directory("CacheReadWrite");
	FDerivedDataCache& Cache = GetDerivedDataCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('a');
	const std::vector<std::byte> First = Bytes({1, 2, 3});
	const std::vector<std::byte> Second = Bytes({4, 5});
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
	FDerivedDataCache& Cache = GetDerivedDataCache();
	std::string Error;
	EXPECT_FALSE(FCacheBucket::FromString("../escape", &Error).IsValid());
	EXPECT_FALSE(FCacheBucket::FromString("/absolute", &Error).IsValid());
	EXPECT_FALSE(FCacheKey::FromString("ABC", &Error).IsValid());
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('b');
	const std::vector<std::byte> Value = Bytes({1, 2, 3, 4});
	EXPECT_EQ(Cache.Put({Bucket, Key, Value, 3}).Status, ECachePutStatus::ValueTooLarge);
	EXPECT_EQ(Cache.Get({Bucket, Key, 3}).Status, ECacheGetStatus::Miss);
	EXPECT_FALSE(std::filesystem::exists(Directory.Root / "escape.bin"));
}

TEST(FDerivedDataCacheTests, RejectsOversizedAndNonregularStoredEntries)
{
	FScopedCacheDirectory Directory("CacheStoredEntryValidation");
	FDerivedDataCache& Cache = GetDerivedDataCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('c');
	const std::vector<std::byte> Value(8, std::byte{1});
	ASSERT_TRUE(Cache.Put({Bucket, Key, Value, 8}));
	EXPECT_EQ(Cache.Get({Bucket, Key, 4}).Status, ECacheGetStatus::ValueTooLarge);
	const auto DirectoryEntry = Directory.Root / "Test" / "Objects" / "dd" / (std::string(32, 'd') + ".bin");
	std::filesystem::create_directories(DirectoryEntry);
	EXPECT_EQ(Cache.Get({Bucket, MakeKey('d'), 8}).Status, ECacheGetStatus::StorageFailure);
}

TEST(FDerivedDataCacheTests, TrimIsBoundedAndIgnoresNoncanonicalFiles)
{
	FScopedCacheDirectory Directory("CacheTrim");
	FDerivedDataCache& Cache = GetDerivedDataCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey OldKey = MakeKey('1');
	const FCacheKey NewKey = MakeKey('2');
	const std::vector<std::byte> Value(10, std::byte{1});
	ASSERT_TRUE(Cache.Put({Bucket, OldKey, Value, 1024}));
	ASSERT_TRUE(Cache.Put({Bucket, NewKey, Value, 1024}));
	const auto OldPath = Directory.Root / "Test" / "Objects" / "11" / (std::string(32, '1') + ".bin");
	const auto NewPath = Directory.Root / "Test" / "Objects" / "22" / (std::string(32, '2') + ".bin");
	std::filesystem::last_write_time(OldPath,
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));
	std::filesystem::last_write_time(NewPath,
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
	const auto IgnoredPath = Directory.Root / "Test" / "Objects" / "not-an-entry.txt";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Value, IgnoredPath));

	const FCacheTrimResult Bounded = Cache.Trim({Bucket, 0, 1});
	EXPECT_EQ(Bounded.Status, ECacheTrimStatus::Partial);
	EXPECT_EQ(Bounded.BytesBefore, 20u);
	EXPECT_EQ(Bounded.DeletedEntries, 1u);
	EXPECT_FALSE(std::filesystem::exists(OldPath));
	EXPECT_TRUE(std::filesystem::exists(NewPath));
	EXPECT_TRUE(std::filesystem::exists(IgnoredPath));

	const FCacheTrimResult Finished = Cache.Trim({Bucket, 0, 8});
	EXPECT_EQ(Finished.Status, ECacheTrimStatus::Complete);
	EXPECT_EQ(Finished.DeletedEntries, 1u);
	EXPECT_TRUE(std::filesystem::exists(IgnoredPath));
}

TEST(FDerivedDataCacheTests, ConcurrentSameKeyCallsPublishCompleteValues)
{
	FScopedCacheDirectory Directory("CacheConcurrency");
	FDerivedDataCache& Cache = GetDerivedDataCache();
	const FCacheBucket Bucket = FCacheBucket::FromString("Test/Objects");
	const FCacheKey Key = MakeKey('e');
	const std::vector<std::byte> First(128, std::byte{1});
	const std::vector<std::byte> Second(128, std::byte{2});
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
