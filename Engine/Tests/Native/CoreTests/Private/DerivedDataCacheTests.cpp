#include "Misc/DerivedDataCache.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto NormalizeDirectory(std::string_view Directory) -> std::filesystem::path
	{
		std::filesystem::path Path(Directory);
		if (Path.filename().empty()) Path = Path.parent_path();
		return Path.lexically_normal();
	}

	auto PathLongerThan(
		const std::filesystem::path& Root,
		std::string_view FileName,
		size_t MinimumLength
	) -> std::filesystem::path
	{
		std::filesystem::path Parent = std::filesystem::absolute(Root).lexically_normal();
		const std::filesystem::path FileNamePath(FileName);
		for (size_t Index = 0; (Parent / FileNamePath).native().size() <= MinimumLength; ++Index)
		{
			Parent /= std::format("cache-segment-{:04}-abcdefgh", Index);
		}
		return Parent / FileNamePath;
	}
}

TEST(FDerivedDataCacheTests, ResolvesProjectFallbackAndTestOverrideRoots)
{
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
	ASSERT_TRUE(Durin::FPaths::SetProjectFile({}));
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()),
		(std::filesystem::path(Durin::FPaths::EngineDir()) / "DerivedDataCache").lexically_normal());

	const std::filesystem::path ProjectDir = Durin::Testing::GetTestWorkDirectory() / "CacheProject";
	std::filesystem::create_directories(ProjectDir);
	const std::filesystem::path ProjectFile = ProjectDir / "CacheProject.dproject";
	{
		std::ofstream Stream(ProjectFile);
		Stream << R"({"ProjectName":"CacheProject"})";
	}
	ASSERT_TRUE(Durin::FPaths::SetProjectFile(ProjectFile.generic_string()));
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()), (ProjectDir / "DerivedDataCache").lexically_normal());

	const std::filesystem::path Override = Durin::Testing::GetTestWorkDirectory() / "IsolatedCache";
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
		Writer.WriteBytes(std::array<Durin::uint8, 3>{7, 8, 9});
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
	std::vector<Durin::uint8> Payload;
	EXPECT_TRUE(Reader.ReadString(Path));
	EXPECT_TRUE(Reader.ReadU64(Size));
	EXPECT_TRUE(Reader.ReadI64(Ticks));
	EXPECT_TRUE(Reader.ReadBytes(Payload, 3, 3));
	EXPECT_TRUE(Reader.IsAtEnd());
	EXPECT_EQ(Path, "/Game/Textures/Test");
	EXPECT_EQ(Size, 42);
	EXPECT_EQ(Ticks, -123456789);
	EXPECT_EQ(Payload, (std::vector<Durin::uint8>{7, 8, 9}));

	Durin::DerivedDataCache::FReader WrongVersion(First);
	EXPECT_FALSE(WrongVersion.ReadAndValidateHeader(Magic, 4, 2));

	Durin::DerivedDataCache::FWriter Oversized;
	Oversized.WriteU64(Durin::DerivedDataCache::MaximumCacheStringBytes + 1);
	Durin::DerivedDataCache::FReader Bounded(Oversized.GetBytes());
	EXPECT_FALSE(Bounded.ReadString(Path));
	Durin::DerivedDataCache::FReader BoundedBytes(First);
	EXPECT_FALSE(BoundedBytes.ReadBytes(Payload, First.size() + 1, First.size() + 1));
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
	const std::filesystem::path Path = Durin::Testing::GetTestWorkDirectory() / "Atomic" / "Index.bin";
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

TEST(FDerivedDataCacheTests, SerializedCacheRoundTripsBeyondMaxPath)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LongDerivedDataCache";
	const std::filesystem::path Path = PathLongerThan(Root, "Index.bin", 300);
	ASSERT_GT(Path.native().size(), 260);

	Durin::DerivedDataCache::FWriter Writer;
	Writer.WriteHeader({0x48434143, 7, 3});
	Writer.WriteString("/Game/LongPath/Texture");
	Writer.WriteBytes(std::array<Durin::uint8, 4>{3, 1, 4, 1});
	const std::vector<Durin::uint8> Expected = Writer.TakeBytes();

	std::string Error;
	ASSERT_TRUE(Durin::DerivedDataCache::WriteFileAtomically(Path, Expected, &Error)) << Error;

	std::vector<Durin::uint8> Stored;
	{
		std::ifstream Stream(Path, std::ios::binary);
		ASSERT_TRUE(Stream.is_open());
		Stored.assign(
			std::istreambuf_iterator<char>(Stream),
			std::istreambuf_iterator<char>());
	}
	Durin::DerivedDataCache::FReader Reader(Stored);
	std::string Identity;
	std::vector<Durin::uint8> Payload;
	ASSERT_TRUE(Reader.ReadAndValidateHeader(0x48434143, 7, 3));
	ASSERT_TRUE(Reader.ReadString(Identity));
	ASSERT_TRUE(Reader.ReadBytes(Payload, 4, 4));
	EXPECT_TRUE(Reader.IsAtEnd());
	EXPECT_EQ(Identity, "/Game/LongPath/Texture");
	EXPECT_EQ(Payload, (std::vector<Durin::uint8>{3, 1, 4, 1}));

	std::error_code CleanupError;
	std::filesystem::remove_all(Root, CleanupError);
	EXPECT_FALSE(CleanupError);
}
