#include "Misc/DerivedDataCache.h"
#include "Misc/Paths.h"

#include <gtest/gtest.h>

namespace
{
	auto NormalizeDirectory(std::string_view Directory) -> std::filesystem::path
	{
		std::filesystem::path Path(Directory);
		if (Path.filename().empty()) Path = Path.parent_path();
		return Path.lexically_normal();
	}
}

TEST(FDerivedDataCacheTests, ResolvesProjectFallbackAndTestOverrideRoots)
{
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
	ASSERT_TRUE(Durin::FPaths::SetProjectFile({}));
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()),
		(std::filesystem::path(Durin::FPaths::EngineDir()) / "DerivedDataCache").lexically_normal());

	const std::filesystem::path ProjectDir = std::filesystem::path(DURIN_TEST_WORK_DIR) / "CacheProject";
	std::filesystem::create_directories(ProjectDir);
	const std::filesystem::path ProjectFile = ProjectDir / "CacheProject.dproject";
	{
		std::ofstream Stream(ProjectFile);
		Stream << R"({"ProjectName":"CacheProject"})";
	}
	ASSERT_TRUE(Durin::FPaths::SetProjectFile(ProjectFile.generic_string()));
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()), (ProjectDir / "DerivedDataCache").lexically_normal());

	const std::filesystem::path Override = std::filesystem::path(DURIN_TEST_WORK_DIR) / "IsolatedCache";
	Durin::FPaths::SetDerivedDataCacheDirForTests(Override.generic_string());
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()), Override.lexically_normal());
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FDerivedDataCacheTests, SerializesDeterministicallyAndRejectsIncompatibleOrUnboundedData)
{
	constexpr Durin::uint32 Magic = 0x48434143;
	auto MakeBytes = [] {
		Durin::DerivedDataCache::FWriter Writer;
		Writer.WriteHeader({Magic, 3, 2});
		Writer.WriteString("/Game/Textures/Test");
		Writer.WriteU64(42);
		Writer.WriteI64(-123456789);
		return Writer.TakeBytes();
	};

	const std::vector<Durin::uint8> First = MakeBytes();
	const std::vector<Durin::uint8> Second = MakeBytes();
	EXPECT_EQ(First, Second);

	Durin::DerivedDataCache::FReader Reader(First);
	ASSERT_TRUE(Reader.ReadAndValidateHeader(Magic, 3, 2));
	std::string Path;
	Durin::uint64 Size = 0;
	Durin::int64 Ticks = 0;
	EXPECT_TRUE(Reader.ReadString(Path));
	EXPECT_TRUE(Reader.ReadU64(Size));
	EXPECT_TRUE(Reader.ReadI64(Ticks));
	EXPECT_TRUE(Reader.IsAtEnd());
	EXPECT_EQ(Path, "/Game/Textures/Test");
	EXPECT_EQ(Size, 42);
	EXPECT_EQ(Ticks, -123456789);

	Durin::DerivedDataCache::FReader WrongVersion(First);
	EXPECT_FALSE(WrongVersion.ReadAndValidateHeader(Magic, 4, 2));

	Durin::DerivedDataCache::FWriter Oversized;
	Oversized.WriteU64(Durin::DerivedDataCache::MaximumCacheStringBytes + 1);
	Durin::DerivedDataCache::FReader Bounded(Oversized.GetBytes());
	EXPECT_FALSE(Bounded.ReadString(Path));
}

TEST(FDerivedDataCacheTests, StableFileTicksRoundTripAtNanosecondPrecision)
{
	constexpr Durin::int64 ExpectedTicks = -1234567890100;
	const auto Time = std::filesystem::file_time_type{
		std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(ExpectedTicks))};
	const Durin::int64 Ticks = Durin::DerivedDataCache::FileTimeToStableTicks(Time);
	EXPECT_EQ(Ticks, ExpectedTicks);
	EXPECT_EQ(Durin::DerivedDataCache::FileTimeToStableTicks(Durin::DerivedDataCache::StableTicksToFileTime(Ticks)), Ticks);
}

TEST(FDerivedDataCacheTests, AtomicallyReplacesAnExistingCacheFile)
{
	const std::filesystem::path Path = std::filesystem::path(DURIN_TEST_WORK_DIR) / "Atomic" / "Index.bin";
	const std::vector<Durin::uint8> First{1, 2, 3};
	const std::vector<Durin::uint8> Second{4, 5};
	std::string Error;
	ASSERT_TRUE(Durin::DerivedDataCache::WriteFileAtomically(Path, First, &Error)) << Error;
	ASSERT_TRUE(Durin::DerivedDataCache::WriteFileAtomically(Path, Second, &Error)) << Error;
	std::ifstream Stream(Path, std::ios::binary);
	const std::vector<Durin::uint8> Bytes((std::istreambuf_iterator<char>(Stream)), std::istreambuf_iterator<char>());
	EXPECT_EQ(Bytes, Second);
	EXPECT_FALSE(std::filesystem::exists(Path.string() + ".tmp"));
}
