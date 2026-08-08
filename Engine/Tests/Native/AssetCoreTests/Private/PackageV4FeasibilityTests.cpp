#include <gtest/gtest.h>

#include "PackageV4Feasibility.h"

#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"

#include <filesystem>
#include <format>
#include <numeric>

namespace
{
	using namespace Durin;
	using namespace Durin::Testing;
	using namespace Durin::Testing::DastV4;

	auto LoadCorpus(std::string_view Relative) -> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		EXPECT_TRUE(FFileHelper::LoadFileToArray(Bytes,
			(std::filesystem::path(DAST_V3_CORPUS_ROOT) / Relative).generic_string()));
		return Bytes;
	}

	auto Breakdown(const FFeasibilityReport& Report) -> std::string
	{
		return std::format(
			"total={} envelope={} name={} type={} schema={} object={} value={} "
			"names={} types={} schemas={} objects={} overrides={} omitted={} depth={} digest={:016X}",
			Report.TotalBytes, Report.EnvelopeAndDirectoryBytes,
			Report.SectionBytes[0], Report.SectionBytes[1], Report.SectionBytes[2],
			Report.SectionBytes[3], Report.SectionBytes[4], Report.NameCount,
			Report.TypeCount, Report.SchemaCount, Report.ObjectCount,
			Report.OverrideCount, Report.OmittedDefaultCount, Report.MaximumNesting,
			Report.Digest);
	}

	template<typename T>
	auto Write(std::vector<uint8>& Bytes, const T& Value) -> void
	{
		const auto* Data = reinterpret_cast<const uint8*>(&Value);
		Bytes.insert(Bytes.end(), Data, Data + sizeof(T));
	}

	auto WriteString(std::vector<uint8>& Bytes, std::string_view Value) -> void
	{
		Write(Bytes, uint64(Value.size()));
		Bytes.insert(Bytes.end(), Value.begin(), Value.end());
	}

	auto WriteField(std::vector<uint8>& Bytes, std::string_view Owner, std::string_view Name,
		uint8 Kind, std::string_view Signature, std::span<const uint8> Payload) -> void
	{
		WriteString(Bytes, Owner);
		WriteString(Bytes, Name);
		Write(Bytes, Kind);
		WriteString(Bytes, Signature);
		Write(Bytes, uint64(Payload.size()));
		Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
	}
}

TEST(FPackageV4FeasibilityTests, DefaultMaterialIsCompleteDeterministicAndWithinBothBudgets)
{
	const std::vector<uint8> V3 = LoadCorpus("Engine/Content/Materials/DefaultMaterial.dasset");
	FV3PackageMeasurement V3Report;
	std::string Error;
	ASSERT_TRUE(MeasureDastV3(V3, V3Report, Error)) << Error;
	ASSERT_EQ(V3Report.Bytes.Total(), 115479);

	FFeasibilityPackage First, Repeated, Reversed;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, false, First, Error)) << Error;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, false, Repeated, Error)) << Error;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, true, Reversed, Error)) << Error;
	EXPECT_EQ(First.Bytes, Repeated.Bytes);
	EXPECT_EQ(First.Bytes, Reversed.Bytes);
	EXPECT_EQ(First.Report, Repeated.Report);
	EXPECT_EQ(First.Report, Reversed.Report);

	const std::string Detail = Breakdown(First.Report);
	EXPECT_EQ(First.Report.TotalBytes, 10869) << Detail;
	EXPECT_EQ(First.Report.EnvelopeAndDirectoryBytes, 79) << Detail;
	EXPECT_EQ(First.Report.SectionBytes,
		(std::array<uint64, SectionCount>{1803, 62, 107, 5, 8813})) << Detail;
	EXPECT_EQ(First.Report.NameCount, 105);
	EXPECT_EQ(First.Report.TypeCount, 21);
	EXPECT_EQ(First.Report.SchemaCount, 6);
	EXPECT_EQ(First.Report.ObjectCount, 1);
	EXPECT_EQ(First.Report.OverrideCount, 3);
	EXPECT_EQ(First.Report.OmittedDefaultCount, 0);
	EXPECT_EQ(First.Report.RetainedDescriptorBytes, 0);
	EXPECT_EQ(First.Report.ParseOperations, 136);
	EXPECT_EQ(First.Report.AllocationInputs, 133);
	EXPECT_EQ(First.Report.MaximumNesting, 5);
	EXPECT_EQ(First.Report.Digest, 0x5955D6A8C777870Cull);
	EXPECT_LE(First.Report.TotalBytes, 16384) << Detail;
	EXPECT_LE(First.Report.TotalBytes, 20659) << Detail;
	EXPECT_EQ(First.Report.TotalBytes,
		First.Report.EnvelopeAndDirectoryBytes + std::accumulate(
			First.Report.SectionBytes.begin(), First.Report.SectionBytes.end(), uint64{0})) << Detail;
	EXPECT_LT(First.Report.ParseOperations, V3Report.ParseOperations) << Detail;
	EXPECT_LT(First.Report.AllocationInputs, V3Report.AllocationInputs) << Detail;
	EXPECT_EQ(First.Report.ObjectCount, V3Report.Objects);
	EXPECT_EQ(First.Report.MaximumNesting, V3Report.MaximumNesting);

	FValidatedHeader Header;
	ASSERT_TRUE(DecodeHeader(First.Bytes, Header, Error)) << Error;
	EXPECT_EQ(Header.Summary.AssetClass, "Durin::DMaterial");
	EXPECT_EQ(Header.Summary.ObjectCount, 1);
	EXPECT_EQ(FXxHash64::HashBuffer(First.Bytes).HashValue, First.Report.Digest);
	std::array<std::vector<uint8>, 4> TableSections;
	for (size_t Index = 0; Index < TableSections.size(); ++Index)
	{
		const FSectionEntry& Entry = Header.Sections[Index];
		TableSections[Index].assign(First.Bytes.begin() + Entry.Offset,
			First.Bytes.begin() + Entry.Offset + Entry.Length);
	}
	FFrozenTables DecodedTables;
	ASSERT_TRUE(DecodeTableSections(TableSections, DecodedTables, Error)) << Error;
	const FSectionEntry& ValueEntry = Header.Sections[4];
	ASSERT_TRUE(ValidateValueSection(std::span(First.Bytes).subspan(
		ValueEntry.Offset, ValueEntry.Length), DecodedTables, Error)) << Error;

	// The fixture consists solely of the five uncompressed canonical sections.
	// A compression flag/block is absent from the frozen v4 envelope vocabulary.
	EXPECT_EQ(Header.Sections.size(), SectionCount);
}

TEST(FPackageV4FeasibilityTests, UnifiedArchiveTypesDriveGenericNonMaterialCorpus)
{
	std::string Error;
	const FArchiveLogicalTypeDescriptor I32 = FArchiveLogicalTypeDescriptor::Scalar(true, 32);
	const FArchiveLogicalTypeDescriptor Entry = FArchiveLogicalTypeDescriptor::Struct(FName("Tests::FEntry"));
	const std::vector<FArchiveLogicalTypeDescriptor> ArchiveTypes = {
		I32,
		FArchiveLogicalTypeDescriptor::String(),
		FArchiveLogicalTypeDescriptor::Array(Entry),
		FArchiveLogicalTypeDescriptor::Map(FArchiveLogicalTypeDescriptor::Name(), I32),
		FArchiveLogicalTypeDescriptor::FixedArray(I32, 3),
		FArchiveLogicalTypeDescriptor::Object(FName("Durin::DObject")),
		FArchiveLogicalTypeDescriptor::SoftObject(FName("Durin::DObject")),
		FArchiveLogicalTypeDescriptor::Bytes(),
	};
	FTableInput Input;
	uint32 FieldIndex = 0;
	for (const FArchiveLogicalTypeDescriptor& ArchiveType : ArchiveTypes)
	{
		ASSERT_TRUE(AddArchiveDiscoveredField({FName("Tests::FArchiveCorpus"),
			FName(std::format("Field{}", FieldIndex++)), ArchiveType}, Input, Error)) << Error;
	}
	FTypePtr I32Type;
	ASSERT_TRUE(AdaptArchiveLogicalType(I32, I32Type, Error)) << Error;
	Input.Schemas.push_back({"Tests::FEntry", {{"Value", I32Type, 0}}});
	FFrozenTables Tables;
	ASSERT_TRUE(FreezeTables(Input, Tables, Error)) << Error;
	std::array<std::vector<uint8>, 4> Sections;
	ASSERT_TRUE(EncodeTableSections(Tables, Sections, Error)) << Error;
	EXPECT_EQ(Tables.Types.size(), 10);
	EXPECT_FALSE(Sections[0].empty());
	EXPECT_FALSE(Sections[1].empty());
	EXPECT_FALSE(Sections[2].empty());
}

TEST(FPackageV4FeasibilityTests, RepeatedNonMaterialStructMetadataAlsoShrinksGenerically)
{
	constexpr std::string_view I32Signature = "4:4";
	std::vector<uint8> ArrayPayload;
	Write(ArrayPayload, uint64{16});
	for (int32 Index = 0; Index < 16; ++Index)
	{
		std::vector<uint8> StructPayload;
		WriteString(StructPayload, "Tests::FEntry");
		Write(StructPayload, uint64{1});
		std::vector<uint8> ScalarPayload;
		Write(ScalarPayload, Index + 1);
		WriteField(StructPayload, "Tests::FEntry", "Value", 4, I32Signature, ScalarPayload);
		ArrayPayload.insert(ArrayPayload.end(), StructPayload.begin(), StructPayload.end());
	}
	std::vector<uint8> V3;
	Write(V3, uint32{Magic});
	Write(V3, uint32{3});
	WriteString(V3, "Tests::DArchiveCorpus");
	Write(V3, uint8{0});
	WriteString(V3, "");
	Write(V3, uint64{0});
	Write(V3, uint64{1});
	Write(V3, uint64{1});
	Write(V3, uint64{0});
	WriteString(V3, "Tests::DArchiveCorpus");
	WriteString(V3, "Synthetic");
	Write(V3, uint64{1});
	WriteField(V3, "Tests::DArchiveCorpus", "Entries", 15,
		"Array<Struct<Tests::FEntry>>", ArrayPayload);

	FV3PackageMeasurement V3Report;
	FFeasibilityPackage V4;
	std::string Error;
	ASSERT_TRUE(MeasureDastV3(V3, V3Report, Error)) << Error;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, false, V4, Error)) << Error;
	EXPECT_LT(V4.Report.TotalBytes, V3Report.Bytes.Total());
	EXPECT_EQ(V4.Report.ObjectCount, 1);
	EXPECT_EQ(V4.Report.OverrideCount, 1);
	EXPECT_EQ(V4.Report.SchemaCount, 2);
}
