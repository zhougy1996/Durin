#include "AssetPackageV5Codec.h"
#include "AssetPackageV6Codec.h"
#include "Asset/PackageObjectStreamWriter.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;
	using namespace Durin::Asset::Private;

	template<typename T>
	auto Read(std::span<const std::byte> Bytes, size_t Offset) -> T
	{
		T Value = 0;
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Value |= static_cast<T>(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
		return Value;
	}

	template<typename T>
	auto Write(std::span<std::byte> Bytes, size_t Offset, T Value) -> void
	{
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>((Value >> (Index * 8)) & 0xff);
	}

	auto RehashEnvelope(std::vector<std::byte>& Bytes) -> void
	{
		const uint64 HeaderBytes = Read<uint64>(Bytes, 32);
		std::vector<std::byte> Header(Bytes.begin(), Bytes.begin() + HeaderBytes);
		std::ranges::fill(std::span(Header).subspan(48, 16), std::byte{});
		const FXxHash128 Hash = FXxHash128::HashBuffer(Header);
		Write<uint64>(Bytes, 48, Hash.HashLow);
		Write<uint64>(Bytes, 56, Hash.HashHigh);
	}

	auto RehashSection(std::vector<std::byte>& Bytes, size_t SectionIndex) -> void
	{
		const size_t Entry = 96 + SectionIndex * 48;
		const uint64 Offset = Read<uint64>(Bytes, Entry + 8);
		const uint64 Size = Read<uint64>(Bytes, Entry + 16);
		const FXxHash128 Hash = FXxHash128::HashBuffer(
			std::span(Bytes).subspan(Offset, Size));
		Write<uint64>(Bytes, Entry + 24, Hash.HashLow);
		Write<uint64>(Bytes, Entry + 32, Hash.HashHigh);
		RehashEnvelope(Bytes);
	}

	auto MakeV5(std::vector<std::byte>& OutBytes) -> bool
	{
		PackageObjectStream::FPackageInput Input{
			.AssetClass = "Example::Asset",
			.Objects = {{"Root", {}, "Example::Asset", "Root"}},
			.ObjectValues = {{"Root", {}}}};
		std::vector<std::byte> ObjectStream;
		PackageObjectStream::FWriterDiagnostic Diagnostic;
		if (!PackageObjectStream::WritePackage(Input, ObjectStream, &Diagnostic))
		{
			ADD_FAILURE() << Diagnostic.Message;
			return false;
		}
		const FAssetResult Result = DastV5::BuildPackageFromObjectStream(ObjectStream, OutBytes);
		if (!Result) ADD_FAILURE() << Result.Message;
		return Result.Succeeded();
	}

	auto ReferenceValidate(std::span<const std::byte> Bytes) -> bool
	{
		if (Bytes.size() < 480 || Bytes[0] != std::byte{0x44}
			|| Bytes[1] != std::byte{0x55} || Bytes[2] != std::byte{0x52}
			|| Bytes[3] != std::byte{0x46} || Read<uint16>(Bytes, 4) != 1
			|| Read<uint16>(Bytes, 6) != 64
			|| Read<uint32>(Bytes, 8) != DastBinaryFormatId.A
			|| Read<uint32>(Bytes, 12) != DastBinaryFormatId.B
			|| Read<uint32>(Bytes, 16) != DastBinaryFormatId.C
			|| Read<uint32>(Bytes, 20) != DastBinaryFormatId.D
			|| Read<uint32>(Bytes, 24) != AssetPackageV6FormatVersion
			|| Read<uint64>(Bytes, 40) != Bytes.size()) return false;
		const uint64 HeaderBytes = Read<uint64>(Bytes, 32);
		if (HeaderBytes > Bytes.size()) return false;
		std::vector<std::byte> Header(Bytes.begin(), Bytes.begin() + HeaderBytes);
		const FXxHash128 StoredHeader{Read<uint64>(Header, 48), Read<uint64>(Header, 56)};
		std::ranges::fill(std::span(Header).subspan(48, 16), std::byte{});
		if (FXxHash128::HashBuffer(Header) != StoredHeader
			|| Read<uint64>(Bytes, 72) != 96 || Read<uint32>(Bytes, 80) != 8
			|| Read<uint32>(Bytes, 84) != 48 || Read<uint64>(Bytes, 88) != 0)
			return false;
		uint64 ExpectedOffset = 480;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			const size_t Entry = 96 + Index * 48;
			const uint64 Offset = Read<uint64>(Bytes, Entry + 8);
			const uint64 Size = Read<uint64>(Bytes, Entry + 16);
			if (Read<uint32>(Bytes, Entry) != Index + 1
				|| Read<uint32>(Bytes, Entry + 4) != 1
				|| Offset != ExpectedOffset || Offset > Bytes.size()
				|| Size > Bytes.size() - Offset || Read<uint64>(Bytes, Entry + 40) != 0)
				return false;
			const FXxHash128 Stored{Read<uint64>(Bytes, Entry + 24), Read<uint64>(Bytes, Entry + 32)};
			if (FXxHash128::HashBuffer(Bytes.subspan(Offset, Size)) != Stored) return false;
			ExpectedOffset += Size;
			if (Index == 1 && ExpectedOffset != HeaderBytes) return false;
		}
		return ExpectedOffset == Bytes.size();
	}

	auto AddUnknownSection(std::span<const std::byte> Bytes, uint32 Flags)
		-> std::vector<std::byte>
	{
		constexpr std::array UnknownBytes{std::byte{0xa5}, std::byte{0x5a}};
		std::vector<std::byte> Result(Bytes.size() + 48 + UnknownBytes.size());
		std::ranges::copy(Bytes.first(480), Result.begin());
		std::ranges::copy(Bytes.subspan(480), Result.begin() + 528);
		for (size_t Index = 0; Index < 8; ++Index)
		{
			const size_t Entry = 96 + Index * 48;
			Write<uint64>(Result, Entry + 8, Read<uint64>(Result, Entry + 8) + 48);
		}
		const size_t UnknownEntry = 96 + 8 * 48;
		Write<uint32>(Result, UnknownEntry, 9);
		Write<uint32>(Result, UnknownEntry + 4, Flags);
		Write<uint64>(Result, UnknownEntry + 8, Bytes.size() + 48);
		Write<uint64>(Result, UnknownEntry + 16, UnknownBytes.size());
		const FXxHash128 Hash = FXxHash128::HashBuffer(UnknownBytes);
		Write<uint64>(Result, UnknownEntry + 24, Hash.HashLow);
		Write<uint64>(Result, UnknownEntry + 32, Hash.HashHigh);
		Write<uint64>(Result, UnknownEntry + 40, uint64{0});
		std::ranges::copy(UnknownBytes, Result.end() - UnknownBytes.size());
		Write<uint32>(Result, 80, 9);
		Write<uint64>(Result, 32, Read<uint64>(Bytes, 32) + 48);
		Write<uint64>(Result, 40, Result.size());
		RehashEnvelope(Result);
		return Result;
	}
}

TEST(FDastV6WireTests, ExactDetachedConversionRoundTripsAndAgreesWithIndependentParser)
{
	std::vector<std::byte> V5;
	ASSERT_TRUE(MakeV5(V5));
	std::vector<std::byte> V6;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, V6));
	EXPECT_TRUE(ReferenceValidate(V6));
	EXPECT_EQ(Read<uint32>(V6, 64), 0);
	EXPECT_EQ(Read<uint32>(V6, 80), 8);
	EXPECT_NE(Read<uint32>(std::span(V6).last(4), 0), PackageTrailer::FooterMagic);
	EXPECT_EQ(FXxHash128::HashBuffer(V6).ToString(),
		"1262496184075ce9227c5b2864562f71");
	EXPECT_EQ(V6.size(), 600u);
	std::vector<std::byte> Repeated;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, Repeated));
	EXPECT_EQ(Repeated, V6);

	DastV6::FParsedPackage Parsed;
	std::string Error;
	ASSERT_TRUE(DastV6::ParsePackage(V6, Parsed, &Error)) << Error;
	EXPECT_EQ(Parsed.AssetClass, "Example::Asset");
	EXPECT_TRUE(Parsed.Imports.empty());
	EXPECT_EQ(Parsed.ExportCount, 1);
	EXPECT_EQ(Parsed.MainExportIndex, 1);
	EXPECT_TRUE(Parsed.PayloadEntries.empty());
	EXPECT_FALSE(Parsed.bHasUnknownSkippableSections);

	std::vector<std::byte> Reconstructed;
	ASSERT_TRUE(DastV6::ConvertV6PackageToV5(V6, Reconstructed));
	EXPECT_EQ(Reconstructed, V5);
}

TEST(FDastV6WireTests, DirectorySectionAndEnvelopeCorruptionFailDeterministically)
{
	std::vector<std::byte> V5;
	ASSERT_TRUE(MakeV5(V5));
	std::vector<std::byte> V6;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, V6));
	DastV6::FParsedPackage Output;
	std::string Error;

	auto ExpectFailure = [&](std::vector<std::byte> Bytes, std::string_view Fragment) {
		const DastV6::FParsedPackage Sentinel{.MainExportIndex = 77};
		Output = Sentinel;
		EXPECT_FALSE(DastV6::ParsePackage(Bytes, Output, &Error));
		EXPECT_NE(Error.find(Fragment), std::string::npos) << Error;
		EXPECT_EQ(Output.MainExportIndex, 77);
	};

	auto BadMagic = V6;
	BadMagic[0] ^= std::byte{1};
	ExpectFailure(std::move(BadMagic), "Magic");

	auto BadDirectoryReserved = V6;
	BadDirectoryReserved[96 + 40] = std::byte{1};
	RehashEnvelope(BadDirectoryReserved);
	ExpectFailure(std::move(BadDirectoryReserved), "section entry");

	auto Gap = V6;
	Write<uint64>(Gap, 96 + 8, Read<uint64>(Gap, 96 + 8) + 1);
	RehashEnvelope(Gap);
	ExpectFailure(std::move(Gap), "section entry");

	auto BadValueHash = V6;
	const size_t ValueEntry = 96 + 6 * 48;
	const uint64 ValueOffset = Read<uint64>(BadValueHash, ValueEntry + 8);
	BadValueHash[ValueOffset] ^= std::byte{1};
	ExpectFailure(std::move(BadValueHash), "section hash");

	auto Trailing = V6;
	Trailing.push_back(std::byte{});
	Write<uint64>(Trailing, 40, Trailing.size());
	RehashEnvelope(Trailing);
	ExpectFailure(std::move(Trailing), "gaps, trailing bytes");
}

TEST(FDastV6WireTests, DetachedCodecIsCompleteButOrdinaryPolicyStillRejectsV6)
{
	const FAssetPackageCodec& Codec = DastV6::GetCodec();
	EXPECT_EQ(Codec.FormatId, DastBinaryFormatId);
	EXPECT_EQ(Codec.FormatVersion, AssetPackageV6FormatVersion);
	EXPECT_TRUE(Codec.bCanRead);
	EXPECT_TRUE(Codec.bCanWrite);
	EXPECT_TRUE(Codec.bCanMutate);
	std::string PolicyError;
	EXPECT_TRUE(ValidateAssetPackageVersionPolicy(PolicyError)) << PolicyError;

	std::vector<std::byte> V5;
	ASSERT_TRUE(MakeV5(V5));
	std::vector<std::byte> V6;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, V6));
	const FAssetPackageCodec* Resolved = reinterpret_cast<const FAssetPackageCodec*>(1);
	const FAssetResult Result = ResolveAssetPackageReader(V6, Resolved);
	EXPECT_EQ(Result.Error, EAssetError::UnsupportedVersion);
	EXPECT_EQ(Resolved, nullptr);
	EXPECT_EQ(FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion)->FormatVersion,
		AssetPackageV5FormatVersion);
}

TEST(FDastV6WireTests, UnknownSkippableDataValidatesButMutationRejectsWithoutLoss)
{
	std::vector<std::byte> V5;
	ASSERT_TRUE(MakeV5(V5));
	std::vector<std::byte> V6;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, V6));
	const std::vector<std::byte> Extended = AddUnknownSection(V6, 0);
	DastV6::FParsedPackage Parsed;
	std::string Error;
	ASSERT_TRUE(DastV6::ParsePackage(Extended, Parsed, &Error)) << Error;
	EXPECT_TRUE(Parsed.bHasUnknownSkippableSections);
	EXPECT_TRUE(DastV6::GetCodec().Validate(Extended));
	std::vector<std::byte> Destination{std::byte{0x7b}};
	FAssetPath Path;
	EXPECT_FALSE(DastV6::GetCodec().Relocate(Extended, Path, Destination));
	EXPECT_EQ(Destination, (std::vector<std::byte>{std::byte{0x7b}}));

	const std::vector<std::byte> Required = AddUnknownSection(V6, 1);
	EXPECT_FALSE(DastV6::ParsePackage(Required, Parsed, &Error));
	EXPECT_NE(Error.find("unknown required"), std::string::npos) << Error;
}

TEST(FDastV6WireTests, CountsIndexesAndEveryHeaderDirectoryByteFailClosed)
{
	std::vector<std::byte> V5;
	ASSERT_TRUE(MakeV5(V5));
	std::vector<std::byte> V6;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, V6));
	DastV6::FParsedPackage Output;
	std::string FirstError;
	std::string SecondError;

	for (size_t Offset = 64; Offset < 480; ++Offset)
	{
		auto Mutated = V6;
		Mutated[Offset] ^= std::byte{0x5a};
		DastV6::FParsedPackage First{.MainExportIndex = 91};
		DastV6::FParsedPackage Second{.MainExportIndex = 91};
		const bool bFirst = DastV6::ParsePackage(Mutated, First, &FirstError);
		const bool bSecond = DastV6::ParsePackage(Mutated, Second, &SecondError);
		EXPECT_FALSE(bFirst) << Offset;
		EXPECT_EQ(bFirst, bSecond) << Offset;
		EXPECT_EQ(FirstError, SecondError) << Offset;
		EXPECT_EQ(First.MainExportIndex, 91) << Offset;
		EXPECT_EQ(Second.MainExportIndex, 91) << Offset;
	}

	auto InvalidMain = V6;
	const uint64 SummaryOffset = Read<uint64>(InvalidMain, 96 + 8);
	Write<uint32>(InvalidMain, SummaryOffset + 4, 2);
	RehashSection(InvalidMain, 0);
	EXPECT_FALSE(DastV6::ParsePackage(InvalidMain, Output, &FirstError));

	auto TooManyExports = V6;
	Write<uint64>(TooManyExports, SummaryOffset + 16, DastV6::MaximumExportCount + 1);
	RehashSection(TooManyExports, 0);
	EXPECT_FALSE(DastV6::ParsePackage(TooManyExports, Output, &FirstError));

	auto ImportMismatch = V6;
	const uint64 ImportOffset = Read<uint64>(ImportMismatch, 96 + 48 + 8);
	Write<uint64>(ImportMismatch, ImportOffset + 8, 1);
	RehashSection(ImportMismatch, 1);
	EXPECT_FALSE(DastV6::ParsePackage(ImportMismatch, Output, &FirstError));

	auto PayloadMismatch = V6;
	Write<uint64>(PayloadMismatch, SummaryOffset + 24, 1);
	RehashSection(PayloadMismatch, 0);
	EXPECT_FALSE(DastV6::ParsePackage(PayloadMismatch, Output, &FirstError));
}

TEST(FDastV6WireTests, HeaderOnlyAndFullValidationCostsStayWithinMeasuredBudgets)
{
	std::vector<std::byte> V5;
	ASSERT_TRUE(MakeV5(V5));
	std::vector<std::byte> V6;
	ASSERT_TRUE(DastV6::ConvertV5Package(V5, V6));
	const uint64 HeaderBytes = Read<uint64>(V6, 32);
	const auto Header = std::span(V6).first(static_cast<size_t>(HeaderBytes));
	constexpr size_t Iterations = 1000;

	const auto HeaderBegin = std::chrono::steady_clock::now();
	for (size_t Index = 0; Index < Iterations; ++Index)
	{
		FAssetPackageHeader Parsed;
		require(DastV6::GetCodec().ReadHeader(Header, V6.size(), Parsed));
	}
	const double HeaderMicroseconds = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - HeaderBegin).count() / Iterations;
	const auto FullBegin = std::chrono::steady_clock::now();
	for (size_t Index = 0; Index < Iterations; ++Index)
	{
		DastV6::FParsedPackage Parsed;
		require(DastV6::ParsePackage(V6, Parsed));
	}
	const double FullMicroseconds = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - FullBegin).count() / Iterations;
	testing::Test::RecordProperty("v6_file_bytes", V6.size());
	testing::Test::RecordProperty("v6_header_bytes", HeaderBytes);
	testing::Test::RecordProperty("v6_header_parse_us", HeaderMicroseconds);
	testing::Test::RecordProperty("v6_full_parse_us", FullMicroseconds);
	EXPECT_LT(HeaderMicroseconds, 500.0);
	EXPECT_LT(FullMicroseconds, 2000.0);
}
