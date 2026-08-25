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
	constexpr uint32 Magic = 0x48434143;
	auto MakeBytes = [] {
		Durin::FBinaryWriter Writer;
		Writer.WriteHeader({Magic, 3, 2});
		Writer.WriteString("/Game/Textures/Test");
		Writer.WriteU64(42);
		Writer.WriteI64(-123456789);
		Writer.WriteBytes(std::array<std::byte, 3>{std::byte{7}, std::byte{8}, std::byte{9}});
		return Writer.TakeBytes();
	};

	const std::vector<std::byte> First = MakeBytes();
	const std::vector<std::byte> Second = MakeBytes();
	EXPECT_EQ(First, Second);

	Durin::FBinaryReader Reader(First);
	ASSERT_TRUE(Reader.ReadAndValidateHeader(Magic, 3, 2));
	std::string Path;
	uint64 Size = 0;
	int64 Ticks = 0;
	std::vector<std::byte> Payload;
	EXPECT_TRUE(Reader.ReadString(Path));
	EXPECT_TRUE(Reader.ReadU64(Size));
	EXPECT_TRUE(Reader.ReadI64(Ticks));
	EXPECT_TRUE(Reader.ReadBytes(Payload, 3, 3));
	EXPECT_TRUE(Reader.IsAtEnd());
	EXPECT_EQ(Path, "/Game/Textures/Test");
	EXPECT_EQ(Size, 42);
	EXPECT_EQ(Ticks, -123456789);
	EXPECT_EQ(Payload, (std::vector<std::byte>{std::byte{7}, std::byte{8}, std::byte{9}}));

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
	constexpr int64 ExpectedTicks = -1234567890100;
	const auto Time = std::filesystem::file_time_type{
		std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::nanoseconds(ExpectedTicks))};
	const int64 Ticks = Durin::FileTime::ToStableTicks(Time);
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
	Writer.WriteBytes(std::array<std::byte, 4>{
		std::byte{3}, std::byte{1}, std::byte{4}, std::byte{1}});
	const std::vector<std::byte> Expected = Writer.TakeBytes();

	Durin::FFileHelper::FAtomicFileError FileError;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Expected, Path, &FileError))
		<< FileError.ToString();

	std::vector<std::byte> Stored;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Stored, Path));
	Durin::FBinaryReader Reader(Stored);
	std::string Identity;
	std::vector<std::byte> Payload;
	ASSERT_TRUE(Reader.ReadAndValidateHeader(0x48434143, 7, 3));
	ASSERT_TRUE(Reader.ReadString(Identity));
	ASSERT_TRUE(Reader.ReadBytes(Payload, 4, 4));
	EXPECT_TRUE(Reader.IsAtEnd());
	EXPECT_EQ(Identity, "/Game/LongPath/Texture");
	EXPECT_EQ(Payload, (std::vector<std::byte>{
		std::byte{3}, std::byte{1}, std::byte{4}, std::byte{1}}));

	std::error_code CleanupError;
	Durin::Testing::RemoveTestWorkDirectory(Root, CleanupError);
	EXPECT_FALSE(CleanupError);
}

TEST(FBinaryFormatTests, FixedWidthPrimitivesAndRegionsPreserveCanonicalBytes)
{
	Durin::FBinaryWriter Writer;
	Writer.WriteU16(0x1234);
	Writer.WriteI32(-2);
	Writer.WriteFloat(-0.0f);
	EXPECT_EQ(Writer.GetBytes(), (std::vector<std::byte>{
		std::byte{0x34}, std::byte{0x12},
		std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
		std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80}}));

	Durin::FBinaryReader Reader(Writer.GetBytes());
	uint16 Unsigned16 = 0;
	int32 Signed32 = 0;
	float Float32 = 0.0f;
	ASSERT_TRUE(Reader.ReadU16(Unsigned16));
	ASSERT_TRUE(Reader.ReadI32(Signed32));
	ASSERT_TRUE(Reader.ReadFloat(Float32));
	EXPECT_EQ(Unsigned16, 0x1234);
	EXPECT_EQ(Signed32, -2);
	EXPECT_TRUE(std::signbit(Float32));
	EXPECT_TRUE(Reader.IsAtEnd());

	uint32 RandomAccess = 7;
	EXPECT_TRUE(Durin::ReadLittleEndianAt(Writer.GetBytes(), 2, RandomAccess));
	EXPECT_EQ(RandomAccess, 0xfffffffeu);
	EXPECT_FALSE(Durin::ReadLittleEndianAt(Writer.GetBytes(), Writer.GetBytes().size() - 1, RandomAccess));
	EXPECT_EQ(RandomAccess, 0xfffffffeu);

	Durin::FBinaryReader Regions(Writer.GetBytes());
	std::span<const std::byte> Region;
	ASSERT_TRUE(Regions.ReadRegion(Region, 2, 2));
	EXPECT_TRUE(std::ranges::equal(
		Region, std::span<const std::byte>(Writer.GetBytes()).first(2)));
	EXPECT_FALSE(Regions.ReadRegion(Region, 3, 2));
	EXPECT_TRUE(Region.empty());
	EXPECT_EQ(Regions.GetRemainingBytes(), Writer.GetBytes().size() - 2);
}

TEST(FBinaryFormatTests, TakeBytesLeavesWriterReadyForAnIndependentSequence)
{
	Durin::FBinaryWriter Writer;
	Writer.WriteU8(0x11);
	EXPECT_EQ(Writer.TakeBytes(),
		(std::vector<std::byte>{std::byte{0x11}}));

	Writer.WriteU16(0x2233);
	Writer.WriteBytes(std::array<std::byte, 2>{std::byte{0x44}, std::byte{0x55}});
	EXPECT_EQ(Writer.TakeBytes(), (std::vector<std::byte>{
		std::byte{0x33}, std::byte{0x22}, std::byte{0x44}, std::byte{0x55}}));
}
