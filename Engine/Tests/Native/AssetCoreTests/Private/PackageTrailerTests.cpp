#include "Asset/PackageTrailer.h"

#include "Asset/PackageInspection.h"
#include "Asset/PackageV4Reader.h"
#include "Asset/PackageV4Writer.h"
#include "AssetPackageCodec.h"
#include "AssetPackageV5Codec.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;
	using namespace Durin::Asset::PackageTrailer;

	auto MakeEntry(uint32 Index) -> FEntry
	{
		return {
			.PayloadId = FGuid{Index, Index + 1, Index + 2, Index + 3},
			.Placement = EPlacement::ExternalDabkV1,
			.LogicalByteCount = uint64(Index) * 16,
			.StoredByteCount = uint64(Index) * 16,
			.ContentHash = {0x1000ull + Index, 0x2000ull + Index},
			.ContainerHash = {0x3000ull + Index, 0x4000ull + Index}};
	}

	auto MakePrefix() -> std::vector<std::byte>
	{
		return {
			std::byte{0x44}, std::byte{0x41}, std::byte{0x53}, std::byte{0x54},
			std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
	}

	auto BuildPackage(std::span<const FEntry> Entries) -> std::vector<std::byte>
	{
		std::vector<std::byte> Package = MakePrefix();
		std::vector<std::byte> Detached;
		std::string Error;
		EXPECT_TRUE(Build(Entries, Package.size(), Detached, &Error)) << Error;
		Package.insert(Package.end(), Detached.begin(), Detached.end());
		return Package;
	}

	auto ReadU32(std::span<const std::byte> Bytes, size_t Offset) -> uint32
	{
		uint32 Value = 0;
		for (size_t Index = 0; Index < 4; ++Index)
			Value |= static_cast<uint32>(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
		return Value;
	}

	auto ReadU64(std::span<const std::byte> Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (size_t Index = 0; Index < 8; ++Index)
			Value |= static_cast<uint64>(std::to_integer<uint8>(Bytes[Offset + Index])) << (Index * 8);
		return Value;
	}

	auto WriteU32(std::span<std::byte> Bytes, size_t Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (size_t Index = 0; Index < 4; ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto WriteU64(std::span<std::byte> Bytes, size_t Offset, uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (size_t Index = 0; Index < 8; ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto RefreshHashes(std::vector<std::byte>& Package) -> void
	{
		const size_t FooterOffset = Package.size() - FooterBytes;
		const size_t TrailerOffset = static_cast<size_t>(ReadU64(Package, FooterOffset + 16));
		const size_t TrailerSize = static_cast<size_t>(ReadU64(Package, FooterOffset + 24));
		const size_t EntryCount = static_cast<size_t>(ReadU64(Package, TrailerOffset + 16));
		const size_t DirectoryOffset = TrailerOffset + TrailerHeaderBytes;
		const FXxHash128 DirectoryHash = FXxHash128::HashBuffer(std::span(Package).subspan(
			DirectoryOffset, EntryCount * TrailerEntryBytes));
		WriteU64(Package, TrailerOffset + 40, DirectoryHash.HashLow);
		WriteU64(Package, TrailerOffset + 48, DirectoryHash.HashHigh);
		const FXxHash128 TrailerHash = FXxHash128::HashBuffer(
			std::span(Package).subspan(TrailerOffset, TrailerSize));
		WriteU64(Package, FooterOffset + 40, TrailerHash.HashLow);
		WriteU64(Package, FooterOffset + 48, TrailerHash.HashHigh);
	}

	auto RefreshTrailerHash(std::vector<std::byte>& Package) -> void
	{
		const size_t FooterOffset = Package.size() - FooterBytes;
		const size_t TrailerOffset = static_cast<size_t>(ReadU64(Package, FooterOffset + 16));
		const size_t TrailerSize = static_cast<size_t>(ReadU64(Package, FooterOffset + 24));
		const FXxHash128 TrailerHash = FXxHash128::HashBuffer(
			std::span(Package).subspan(TrailerOffset, TrailerSize));
		WriteU64(Package, FooterOffset + 40, TrailerHash.HashLow);
		WriteU64(Package, FooterOffset + 48, TrailerHash.HashHigh);
	}

	auto ExpectRejected(std::vector<std::byte> Package, std::string_view Category) -> void
	{
		FInspection Inspection{
			.ObjectStreamEnd = 77,
			.Entries = {MakeEntry(9)}};
		std::string Error;
		EXPECT_FALSE(Inspect(Package, Inspection, &Error));
		EXPECT_TRUE(Inspection.Entries.empty());
		EXPECT_EQ(Inspection.ObjectStreamEnd, 0u);
		EXPECT_NE(Error.find(Category), std::string::npos) << Error;
	}
}

TEST(FPackageTrailerWireTests, BuildsCanonicalGoldenAndInspectsWithoutObjects)
{
	const FEntry Entry = MakeEntry(1);
	const std::vector<std::byte> Package = BuildPackage(std::span(&Entry, 1));
	ASSERT_EQ(Package.size(), 8u + TrailerHeaderBytes + TrailerEntryBytes + FooterBytes);
	const size_t TrailerOffset = 8;
	const size_t EntryOffset = TrailerOffset + TrailerHeaderBytes;
	const size_t FooterOffset = EntryOffset + TrailerEntryBytes;
	EXPECT_EQ(ReadU32(Package, TrailerOffset), TrailerMagic);
	EXPECT_EQ(ReadU32(Package, TrailerOffset + 4), TrailerVersion);
	EXPECT_EQ(ReadU32(Package, TrailerOffset + 8), TrailerHeaderBytes);
	EXPECT_EQ(ReadU32(Package, TrailerOffset + 12), TrailerEntryBytes);
	EXPECT_EQ(ReadU64(Package, TrailerOffset + 16), 1u);
	EXPECT_EQ(ReadU64(Package, TrailerOffset + 24), TrailerHeaderBytes);
	EXPECT_EQ(ReadU64(Package, TrailerOffset + 32), 8u);
	EXPECT_EQ(ReadU32(Package, EntryOffset + 16), uint32(EPlacement::ExternalDabkV1));
	EXPECT_EQ(ReadU64(Package, EntryOffset + 24), 16u);
	EXPECT_EQ(ReadU64(Package, EntryOffset + 32), 16u);
	EXPECT_EQ(ReadU32(Package, FooterOffset), FooterMagic);
	EXPECT_EQ(ReadU32(Package, FooterOffset + 4), FooterVersion);
	EXPECT_EQ(ReadU64(Package, FooterOffset + 16), 8u);
	EXPECT_EQ(ReadU64(Package, FooterOffset + 24), TrailerHeaderBytes + TrailerEntryBytes);
	EXPECT_EQ(ReadU64(Package, FooterOffset + 32), 8u);
	EXPECT_EQ(FXxHash128::HashBuffer(Package).ToString(),
		"151d6607e1f921fc683988df02e9ee61");

	FInspection Inspection;
	std::string Error;
	ASSERT_TRUE(Inspect(Package, Inspection, &Error)) << Error;
	ASSERT_EQ(Inspection.Entries, std::vector{Entry});
	EXPECT_EQ(Inspection.ObjectStreamEnd, 8u);
	EXPECT_EQ(Inspection.TrailerOffset, 8u);
	EXPECT_EQ(Inspection.TrailerSize, TrailerHeaderBytes + TrailerEntryBytes);
}

TEST(FPackageTrailerWireTests, EmptyAndPermutedInputsRoundTripCanonically)
{
	const std::vector<std::byte> Empty = BuildPackage(std::span<const FEntry>{});
	ASSERT_EQ(Empty.size(), 8u + TrailerHeaderBytes + FooterBytes);
	EXPECT_EQ(FXxHash128::HashBuffer(Empty).ToString(),
		"2e4faaccbaa9225e41176cf5eaffd62e");
	FInspection EmptyInspection;
	std::string Error;
	ASSERT_TRUE(Inspect(Empty, EmptyInspection, &Error)) << Error;
	EXPECT_TRUE(EmptyInspection.Entries.empty());

	const std::array Entries{MakeEntry(3), MakeEntry(1), MakeEntry(2)};
	const std::array Permuted{Entries[1], Entries[2], Entries[0]};
	const std::vector<std::byte> Multiple = BuildPackage(Entries);
	EXPECT_EQ(Multiple, BuildPackage(Permuted));
	EXPECT_EQ(FXxHash128::HashBuffer(Multiple).ToString(),
		"75f61f4730a2440256598234ad9feb52");
	FInspection Inspection;
	ASSERT_TRUE(Inspect(Multiple, Inspection, &Error)) << Error;
	EXPECT_EQ(Inspection.Entries, (std::vector{MakeEntry(1), MakeEntry(2), MakeEntry(3)}));

	std::vector<std::byte> Output{std::byte{0xff}};
	const std::array Duplicate{MakeEntry(1), MakeEntry(1)};
	EXPECT_FALSE(Build(Duplicate, 8, Output, &Error));
	EXPECT_TRUE(Output.empty());
	EXPECT_NE(Error.find("duplicate"), std::string::npos);
}

TEST(FPackageTrailerWireTests, RejectsHeaderFooterAndIntegrityCorruption)
{
	const FEntry Entry = MakeEntry(1);
	const std::vector<std::byte> Valid = BuildPackage(std::span(&Entry, 1));
	const size_t TrailerOffset = 8;
	const size_t FooterOffset = Valid.size() - FooterBytes;
	for (const size_t Offset : {
		FooterOffset + 0, FooterOffset + 4, FooterOffset + 8,
		FooterOffset + 12, FooterOffset + 16, FooterOffset + 24,
		FooterOffset + 32, FooterOffset + 56})
	{
		auto Corrupt = Valid;
		Corrupt[Offset] ^= std::byte{1};
		ExpectRejected(std::move(Corrupt), "footer");
	}
	{
		auto Corrupt = Valid;
		Corrupt[FooterOffset + 40] ^= std::byte{1};
		ExpectRejected(std::move(Corrupt), "hash");
	}
	for (const size_t Offset : {
		TrailerOffset + 0, TrailerOffset + 4, TrailerOffset + 8,
		TrailerOffset + 12, TrailerOffset + 16, TrailerOffset + 24,
		TrailerOffset + 32, TrailerOffset + 40, TrailerOffset + 56})
	{
		auto Corrupt = Valid;
		Corrupt[Offset] ^= std::byte{1};
		ExpectRejected(std::move(Corrupt), "hash");
	}
}

TEST(FPackageTrailerWireTests, RejectsUnsupportedEntryStatesAfterValidRehash)
{
	const FEntry Entry = MakeEntry(1);
	const std::vector<std::byte> Valid = BuildPackage(std::span(&Entry, 1));
	const size_t EntryOffset = 8 + TrailerHeaderBytes;
	for (const size_t Offset : {
		EntryOffset + 16, EntryOffset + 20, EntryOffset + 24,
		EntryOffset + 72})
	{
		auto Corrupt = Valid;
		Corrupt[Offset] ^= std::byte{1};
		RefreshHashes(Corrupt);
		ExpectRejected(std::move(Corrupt), "entry");
	}
	for (const size_t Offset : {EntryOffset + 40, EntryOffset + 56})
	{
		auto Corrupt = Valid;
		std::fill_n(Corrupt.begin() + Offset, 16, std::byte{0});
		RefreshHashes(Corrupt);
		ExpectRejected(std::move(Corrupt), "entry");
	}
	{
		auto Corrupt = Valid;
		Corrupt[EntryOffset + 32] ^= std::byte{1};
		RefreshHashes(Corrupt);
		ExpectRejected(std::move(Corrupt), "entry");
	}
}

TEST(FPackageTrailerWireTests, RejectsSemanticHeaderAndDirectoryCorruptionAfterOuterRehash)
{
	const FEntry Entry = MakeEntry(1);
	const std::vector<std::byte> Valid = BuildPackage(std::span(&Entry, 1));
	const size_t TrailerOffset = 8;
	{
		auto InvalidMagic = Valid;
		InvalidMagic[TrailerOffset] ^= std::byte{1};
		RefreshTrailerHash(InvalidMagic);
		ExpectRejected(std::move(InvalidMagic), "header");
	}
	{
		auto InvalidEntrySize = Valid;
		InvalidEntrySize[TrailerOffset + 12] ^= std::byte{1};
		RefreshTrailerHash(InvalidEntrySize);
		ExpectRejected(std::move(InvalidEntrySize), "header");
	}
	{
		auto InvalidCount = Valid;
		WriteU64(InvalidCount, TrailerOffset + 16, 2);
		RefreshTrailerHash(InvalidCount);
		ExpectRejected(std::move(InvalidCount), "extent");
	}
	{
		auto StaleDirectoryHash = Valid;
		StaleDirectoryHash[TrailerOffset + 40] ^= std::byte{1};
		RefreshTrailerHash(StaleDirectoryHash);
		ExpectRejected(std::move(StaleDirectoryHash), "directory hash");
	}
}

TEST(FPackageTrailerWireTests, RejectsDuplicateOrderTruncationAndTrailingBytes)
{
	const std::array Entries{MakeEntry(1), MakeEntry(2)};
	const std::vector<std::byte> Valid = BuildPackage(Entries);
	{
		auto Duplicate = Valid;
		const size_t FirstEntry = 8 + TrailerHeaderBytes;
		const size_t SecondEntry = FirstEntry + TrailerEntryBytes;
		std::copy_n(Duplicate.begin() + FirstEntry, 16, Duplicate.begin() + SecondEntry);
		RefreshHashes(Duplicate);
		ExpectRejected(std::move(Duplicate), "duplicate");
	}
	{
		auto Truncated = Valid;
		Truncated.pop_back();
		ExpectRejected(std::move(Truncated), "footer");
	}
	{
		auto Trailing = Valid;
		Trailing.push_back(std::byte{0});
		ExpectRejected(std::move(Trailing), "footer");
	}
}

TEST(FPackageTrailerWireTests, RejectsInvalidBuildInputsAndPreservesV4Policy)
{
	std::string Error;
	std::vector<std::byte> Output{std::byte{0xff}};
	EXPECT_FALSE(Build(std::span<const FEntry>{}, 7, Output, &Error));
	EXPECT_TRUE(Output.empty());
	EXPECT_FALSE(Build(
		std::span<const FEntry>{}, MaximumObjectStreamBytes + 1, Output, &Error));
	const std::vector<FEntry> TooManyEntries(MaximumEntryCount + 1);
	EXPECT_FALSE(Build(TooManyEntries, 8, Output, &Error));
	EXPECT_NE(Error.find("count"), std::string::npos);

	FEntry Invalid = MakeEntry(1);
	Invalid.ContainerHash = {};
	EXPECT_FALSE(Build(std::span(&Invalid, 1), 8, Output, &Error));
	EXPECT_TRUE(Output.empty());

	const std::vector<std::byte> V5Preamble = MakePrefix();
	const FAssetResult Result = ValidateAssetPackageBytes(V5Preamble);
	EXPECT_EQ(Result.Error, EAssetError::CorruptFile);
}

TEST(FPackageTrailerWireTests, V5CodecReadsMutatesAndRejectsTrailerDisagreement)
{
	DastV4::FPackageInput Input{
		.AssetClass = "Example::Asset",
		.Objects = {{"Root", {}, "Example::Asset", "Root"}},
		.ObjectValues = {{"Root", {}}}};
	std::vector<std::byte> V4;
	DastV4::FWriterDiagnostic WriterDiagnostic;
	ASSERT_TRUE(DastV4::WritePackage(Input, V4, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	std::vector<std::byte> V5;
	ASSERT_TRUE(Durin::Asset::Private::DastV5::ConvertV4Package(V4, V5));
	EXPECT_EQ(ReadU32(V5, 4), AssetPackageV5FormatVersion);
	EXPECT_EQ(FXxHash128::HashBuffer(V5).ToString(),
		"527c1a520e7122bec12f8141d6eb638c");
	EXPECT_TRUE(ValidateAssetPackageBytes(V5));

	const Durin::Asset::Private::FAssetPackageCodec* Reader =
		Durin::Asset::Private::FindAssetPackageReader(AssetPackageV5FormatVersion);
	const Durin::Asset::Private::FAssetPackageCodec* Writer =
		Durin::Asset::Private::FindAssetPackageWriter(AssetPackageV5FormatVersion);
	ASSERT_NE(Reader, nullptr);
	ASSERT_EQ(Reader, Writer);
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(Reader->Inspect(V5, Inspection));
	EXPECT_EQ(Inspection.Header.FormatVersion, AssetPackageV5FormatVersion);
	EXPECT_EQ(Inspection.Header.EntryKind, EAssetRegistryEntryKind::Asset);
	DastV4::FDecodedPackage OldReaderOutput;
	EXPECT_FALSE(DastV4::DecodePackage(V5, OldReaderOutput));

	FInspection Trailer;
	ASSERT_TRUE(Inspect(V5, Trailer));
	std::vector<std::byte> WrongDetached;
	const FEntry WrongEntry = MakeEntry(7);
	ASSERT_TRUE(Build(std::span(&WrongEntry, 1), Trailer.ObjectStreamEnd, WrongDetached));
	std::vector<std::byte> Disagrees(
		V5.begin(), V5.begin() + static_cast<size_t>(Trailer.ObjectStreamEnd));
	Disagrees.insert(Disagrees.end(), WrongDetached.begin(), WrongDetached.end());
	const FAssetResult Disagreement = ValidateAssetPackageBytes(Disagrees);
	EXPECT_EQ(Disagreement.Error, EAssetError::CorruptFile);
	EXPECT_NE(Disagreement.Message.find("disagree"), std::string::npos);
}
