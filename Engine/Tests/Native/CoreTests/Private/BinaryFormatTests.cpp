#include "Serialization/BinaryFormat.h"
#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
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

TEST(FBinaryFormatTests, SerializesDeterministicallyAndRejectsIncompatibleOrUnboundedData)
{
	constexpr Durin::uint32 Magic = 0x48434143;
	auto MakeBytes = [] {
		Durin::FBinaryWriter Writer;
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

	Durin::FBinaryReader Reader(First);
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

	Durin::FBinaryReader WrongVersion(First);
	EXPECT_FALSE(WrongVersion.ReadAndValidateHeader(Magic, 4, 2));

	Durin::FBinaryWriter Oversized;
	Oversized.WriteU64(Durin::MaximumBinaryStringBytes + 1);
	Durin::FBinaryReader Bounded(Oversized.GetBytes());
	EXPECT_FALSE(Bounded.ReadString(Path));
	Durin::FBinaryReader BoundedBytes(First);
	EXPECT_FALSE(BoundedBytes.ReadBytes(Payload, First.size() + 1, First.size() + 1));
}

TEST(FFileTimeTests, StableFileTicksRoundTripAtNanosecondPrecision)
{
	constexpr Durin::int64 ExpectedTicks = -1234567890100;
	const auto Time = std::filesystem::file_time_type{
		std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(ExpectedTicks))};
	const Durin::int64 Ticks = Durin::FileTime::ToStableTicks(Time);
	EXPECT_EQ(Ticks, ExpectedTicks);
	EXPECT_EQ(Durin::FileTime::ToStableTicks(Durin::FileTime::FromStableTicks(Ticks)), Ticks);
}

TEST(FBinaryFormatTests, SerializedDataRoundTripsBeyondMaxPath)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "LongDerivedDataCache";
	const std::filesystem::path Path = PathLongerThan(Root, "Index.bin", 300);
	ASSERT_GT(Path.native().size(), 260);

	Durin::FBinaryWriter Writer;
	Writer.WriteHeader({0x48434143, 7, 3});
	Writer.WriteString("/Game/LongPath/Texture");
	Writer.WriteBytes(std::array<Durin::uint8, 4>{3, 1, 4, 1});
	const std::vector<Durin::uint8> Expected = Writer.TakeBytes();

	Durin::FFileHelper::FAtomicFileError FileError;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Expected, Path, &FileError))
		<< FileError.ToString();

	std::vector<Durin::uint8> Stored;
	{
		std::ifstream Stream(Path, std::ios::binary);
		ASSERT_TRUE(Stream.is_open());
		Stored.assign(
			std::istreambuf_iterator<char>(Stream),
			std::istreambuf_iterator<char>());
	}
	Durin::FBinaryReader Reader(Stored);
	std::string Identity;
	std::vector<Durin::uint8> Payload;
	ASSERT_TRUE(Reader.ReadAndValidateHeader(0x48434143, 7, 3));
	ASSERT_TRUE(Reader.ReadString(Identity));
	ASSERT_TRUE(Reader.ReadBytes(Payload, 4, 4));
	EXPECT_TRUE(Reader.IsAtEnd());
	EXPECT_EQ(Identity, "/Game/LongPath/Texture");
	EXPECT_EQ(Payload, (std::vector<Durin::uint8>{3, 1, 4, 1}));

	std::error_code CleanupError;
	Durin::Testing::RemoveTestWorkDirectory(Root, CleanupError);
	EXPECT_FALSE(CleanupError);
}
