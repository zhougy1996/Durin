#include <gtest/gtest.h>

#include "Developer/AssetBuildCore/Private/DerivedDataObjectStore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

namespace
{
	auto MakeKey(char Fill) -> std::string
	{
		return std::string(32, Fill);
	}
	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}
}

TEST(FDerivedDataObjectStoreTests, ReadsAndAtomicallyReplacesCanonicalObjects)
{
	const auto CacheRoot = Durin::Testing::GetTestWorkDirectory() / "ObjectStoreReadWrite";
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const Durin::Asset::Build::FDerivedDataObjectStore Store("Test/Objects", 1024);
	const std::string Key = MakeKey('a');
	const std::vector<std::byte> First = Bytes({1, 2, 3});
	const std::vector<std::byte> Second = Bytes({4, 5});
	std::string Error;
	ASSERT_TRUE(Store.Write(Key, First, &Error)) << Error;
	ASSERT_TRUE(Store.Write(Key, Second, &Error)) << Error;

	std::vector<std::byte> Loaded = Bytes({9});
	const auto Read = Store.Read(Key, Loaded);
	EXPECT_EQ(Read.Status, Durin::Asset::Build::EDerivedDataObjectReadStatus::Hit);
	EXPECT_EQ(Loaded, Second);
	EXPECT_EQ(Store.GetRoot(), CacheRoot / "Test" / "Objects");
	EXPECT_TRUE(std::filesystem::is_regular_file(CacheRoot / "Test" / "Objects" / "aa" / (Key + ".bin")));
}

TEST(FDerivedDataObjectStoreTests, RejectsInvalidKeysAndOversizedObjectsTransactionally)
{
	const auto CacheRoot = Durin::Testing::GetTestWorkDirectory() / "ObjectStoreValidation";
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const Durin::Asset::Build::FDerivedDataObjectStore Store("Test/Objects", 3);
	const std::vector<std::byte> BytesValue = Bytes({1, 2, 3, 4});
	std::string Error;
	EXPECT_FALSE(Store.Write("../escape", BytesValue, &Error));
	EXPECT_FALSE(Store.Write(MakeKey('b'), BytesValue, &Error));
	std::vector<std::byte> Sentinel = Bytes({7});
	EXPECT_EQ(Store.Read("../escape", Sentinel).Status, Durin::Asset::Build::EDerivedDataObjectReadStatus::InvalidKey);
	EXPECT_EQ(Sentinel, Bytes({7}));
	EXPECT_FALSE(std::filesystem::exists(CacheRoot / "escape.bin"));
}

TEST(FDerivedDataObjectStoreTests, CleanupIsBoundedAndOnlyDeletesCanonicalObjects)
{
	const auto CacheRoot = Durin::Testing::GetTestWorkDirectory() / "ObjectStoreCleanup";
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const Durin::Asset::Build::FDerivedDataObjectStore Store("Test/Objects", 1024);
	const std::vector<std::byte> Bytes(10, std::byte{1});
	std::string Error;
	const std::string OldKey = MakeKey('1');
	const std::string NewKey = MakeKey('2');
	ASSERT_TRUE(Store.Write(OldKey, Bytes, &Error)) << Error;
	ASSERT_TRUE(Store.Write(NewKey, Bytes, &Error)) << Error;
	const auto OldPath = CacheRoot / "Test" / "Objects" / "11" / (OldKey + ".bin");
	const auto NewPath = CacheRoot / "Test" / "Objects" / "22" / (NewKey + ".bin");
	std::filesystem::last_write_time(OldPath, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));
	std::filesystem::last_write_time(NewPath, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
	const auto IgnoredPath = CacheRoot / "Test" / "Objects" / "not-an-object.txt";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()}, IgnoredPath));

	const auto Bounded = Store.CleanupToBudget(0, 1);
	EXPECT_EQ(Bounded.BytesBefore, 20u);
	EXPECT_EQ(Bounded.DeletedObjects, 1u);
	EXPECT_FALSE(Bounded.bBudgetSatisfied);
	EXPECT_FALSE(std::filesystem::exists(OldPath));
	EXPECT_TRUE(std::filesystem::exists(NewPath));
	EXPECT_TRUE(std::filesystem::exists(IgnoredPath));

	const auto Finished = Store.CleanupToBudget(0, 8);
	EXPECT_TRUE(Finished.bBudgetSatisfied);
	EXPECT_EQ(Finished.DeletedObjects, 1u);
	EXPECT_TRUE(std::filesystem::exists(IgnoredPath));
}
