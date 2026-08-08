#include <gtest/gtest.h>

#include "PackageV4Feasibility.h"

#include "AssetPackageV4Writer.h"
#include "Hash/XxHash.h"
#include "AssetSystem.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "EngineAssetServices.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Name.h"
#include "Misc/Paths.h"

#include <filesystem>
#include <format>
#include <cstring>
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

	auto LoadDefaultMaterialForPlan() -> Durin::DMaterial*
	{
		static const bool Initialized = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			if (!Durin::IsFNameInitialized()) Durin::FNameInit();
			if (!Durin::FindClassByQualifiedName("Durin::DObject")) Durin::DObjectInit();
			Durin::InitializeEngineAssetServices();
			return true;
		}();
		(void)Initialized;
		(void)Durin::DMaterial::StaticClass();
		static const bool Mounted = [] {
			const std::filesystem::path Content =
				std::filesystem::path(DAST_V3_CORPUS_ROOT) / "Engine" / "Content";
			Durin::PathUtilities::RegisterMountPointForTests(
				"/Engine/", Content.generic_string() + "/");
			return true;
		}();
		(void)Mounted;
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate("/Engine/Materials/DefaultMaterial", Path));
		Durin::DObject* Loaded = nullptr;
		EXPECT_TRUE(Durin::Asset::LoadAsset(Path, Loaded));
		return Durin::Cast<Durin::DMaterial>(Loaded);
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
	Durin::DMaterial* Material = LoadDefaultMaterialForPlan();
	ASSERT_NE(Material, nullptr);
	FDefaultDeltaPlan DeltaPlan;
	FDefaultDeltaDiagnostic DeltaDiagnostic;
	ASSERT_TRUE(BuildDefaultDeltaPlan(
		Material, EDefaultDeltaMode::Enabled, DeltaPlan, &DeltaDiagnostic))
		<< "reason=" << static_cast<int>(DeltaDiagnostic.Reason)
		<< " path=" << DeltaDiagnostic.LogicalPath;
	FDefaultDeltaPlan RepeatedDeltaPlan;
	ASSERT_TRUE(BuildDefaultDeltaPlan(
		Material, EDefaultDeltaMode::Enabled, RepeatedDeltaPlan, &DeltaDiagnostic));
	EXPECT_TRUE(AreDefaultDeltaPlansEquivalent(DeltaPlan, RepeatedDeltaPlan));
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, false, First, Error, &DeltaPlan)) << Error;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, false, Repeated, Error, &DeltaPlan)) << Error;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, true, Reversed, Error, &DeltaPlan)) << Error;
	EXPECT_EQ(First.Bytes, Repeated.Bytes);
	EXPECT_EQ(First.Bytes, Reversed.Bytes);
	EXPECT_EQ(First.Report, Repeated.Report);
	EXPECT_EQ(First.Report, Reversed.Report);
	std::vector<uint8> ProductionBytes = {0xee};
	std::vector<uint8> ProductionRepeated;
	Durin::Asset::DastV4::FWriterDiagnostic WriterDiagnostic;
	ASSERT_TRUE(Durin::Asset::DastV4::WriteAssetPackage(
		Material->GetPackage(), ProductionBytes, {}, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	ASSERT_TRUE(Durin::Asset::DastV4::WriteAssetPackage(
		Material->GetPackage(), ProductionRepeated, {}, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	EXPECT_EQ(ProductionBytes, First.Bytes);
	EXPECT_EQ(ProductionRepeated, First.Bytes);
	FDefaultDeltaPlan NoDeltaPlan;
	ASSERT_TRUE(BuildDefaultDeltaPlan(
		Material, EDefaultDeltaMode::NoDelta, NoDeltaPlan, &DeltaDiagnostic));
	FFeasibilityPackage NoDeltaReference;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(
		V3, false, NoDeltaReference, Error, &NoDeltaPlan)) << Error;
	Durin::Asset::DastV4::FAssetPackageWriteOptions NoDeltaOptions;
	NoDeltaOptions.DeltaMode = EDefaultDeltaMode::NoDelta;
	std::vector<uint8> NoDeltaProduction;
	ASSERT_TRUE(Durin::Asset::DastV4::WriteAssetPackage(
		Material->GetPackage(), NoDeltaProduction, NoDeltaOptions, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	EXPECT_EQ(NoDeltaProduction, NoDeltaReference.Bytes);
	std::vector<uint8> OrdinaryV3;
	ASSERT_TRUE(Durin::Asset::SerializeAssetPackageBytes(Material->GetPackage(), OrdinaryV3));
	ASSERT_GE(OrdinaryV3.size(), 8u);
	uint32 OrdinaryVersion = 0;
	std::memcpy(&OrdinaryVersion, OrdinaryV3.data() + 4, sizeof(OrdinaryVersion));
	EXPECT_EQ(OrdinaryVersion, 3u);

	const std::string Detail = std::format(
		"{} plan_objects={} plan_fields={} plan_emitted={} plan_omitted={} comparisons={} plan_depth={}",
		Breakdown(First.Report), DeltaPlan.Objects.size(), DeltaPlan.FieldCount,
		DeltaPlan.EmittedFieldCount, DeltaPlan.OmittedFieldCount,
		DeltaPlan.ComparisonCount, DeltaPlan.MaximumDepth);
	EXPECT_EQ(First.Report.TotalBytes, 6275) << Detail;
	EXPECT_EQ(First.Report.EnvelopeAndDirectoryBytes, 79) << Detail;
	EXPECT_EQ(First.Report.SectionBytes,
		(std::array<uint64, SectionCount>{1803, 62, 107, 5, 4219})) << Detail;
	EXPECT_EQ(First.Report.NameCount, 105);
	EXPECT_EQ(First.Report.TypeCount, 21);
	EXPECT_EQ(First.Report.SchemaCount, 6);
	EXPECT_EQ(First.Report.ObjectCount, 1);
	EXPECT_EQ(First.Report.OverrideCount, 1);
	EXPECT_EQ(First.Report.OmittedDefaultCount, 231);
	EXPECT_EQ(First.Report.RetainedDescriptorBytes, 0);
	EXPECT_EQ(First.Report.ParseOperations, 134);
	EXPECT_EQ(First.Report.AllocationInputs, 133);
	EXPECT_EQ(First.Report.MaximumNesting, 5);
	EXPECT_EQ(First.Report.Digest, 0xC4111B7609C78D4Full);
	EXPECT_EQ(DeltaPlan.Objects.size(), 1u) << Detail;
	EXPECT_EQ(DeltaPlan.FieldCount, 785u) << Detail;
	EXPECT_EQ(DeltaPlan.EmittedFieldCount, 554u) << Detail;
	EXPECT_EQ(DeltaPlan.OmittedFieldCount, 231u) << Detail;
	EXPECT_EQ(DeltaPlan.ComparisonCount, 1275u) << Detail;
	EXPECT_EQ(DeltaPlan.MaximumDepth, 5u) << Detail;
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

	ASSERT_FALSE(Material->HasAllocatedAuthoredOverrideLedger());
	const auto Omitted = std::ranges::find_if(DeltaPlan.Objects.front().Fields,
		[](const FDefaultDeltaFieldPlan& Field) {
			return Field.Disposition == EDefaultDeltaDisposition::Omitted;
		});
	ASSERT_NE(Omitted, DeltaPlan.Objects.front().Fields.end());
	const FAuthoredOverridePath OverridePath{
		FAuthoredOverridePathToken::Field(
			Omitted->Descriptor.DeclaringType, Omitted->Descriptor.Name)};
	struct FOverrideReset
	{
		DObject* Object;
		~FOverrideReset() { Object->ResetAuthoredOverrides(); }
	} OverrideReset{Material};
	FAuthoredOverrideDiagnostic OverrideDiagnostic;
	ASSERT_TRUE(Material->SetAuthoredOverride(
		OverridePath, EAuthoredOverrideProvenance::LoadedExplicit, &OverrideDiagnostic));
	FDefaultDeltaPlan LoadedExplicitPlan, LoadedExplicitRepeated;
	ASSERT_TRUE(BuildDefaultDeltaPlan(
		Material, EDefaultDeltaMode::Enabled, LoadedExplicitPlan, &DeltaDiagnostic));
	ASSERT_TRUE(BuildDefaultDeltaPlan(
		Material, EDefaultDeltaMode::Enabled, LoadedExplicitRepeated, &DeltaDiagnostic));
	EXPECT_TRUE(AreDefaultDeltaPlansEquivalent(LoadedExplicitPlan, LoadedExplicitRepeated));
	EXPECT_EQ(LoadedExplicitPlan.EmittedFieldCount, DeltaPlan.EmittedFieldCount + 1);
	EXPECT_EQ(LoadedExplicitPlan.OmittedFieldCount + 1, DeltaPlan.OmittedFieldCount);
	FFeasibilityPackage LoadedExplicitPackage, LoadedExplicitPackageRepeated;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(
		V3, false, LoadedExplicitPackage, Error, &LoadedExplicitPlan)) << Error;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(
		V3, false, LoadedExplicitPackageRepeated, Error, &LoadedExplicitRepeated)) << Error;
	EXPECT_EQ(LoadedExplicitPackage.Bytes, LoadedExplicitPackageRepeated.Bytes);
	EXPECT_NE(LoadedExplicitPackage.Bytes, First.Bytes);
	EXPECT_EQ(LoadedExplicitPackage.Report.OverrideCount, First.Report.OverrideCount + 1);
	std::vector<uint8> LoadedExplicitProduction;
	ASSERT_TRUE(Durin::Asset::DastV4::WriteAssetPackage(
		Material->GetPackage(), LoadedExplicitProduction, {}, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	EXPECT_EQ(LoadedExplicitProduction, LoadedExplicitPackage.Bytes);

	ASSERT_TRUE(Material->SetAuthoredOverride(
		OverridePath, EAuthoredOverrideProvenance::Forced, &OverrideDiagnostic));
	FDefaultDeltaPlan ForcedPlan;
	ASSERT_TRUE(BuildDefaultDeltaPlan(Material, EDefaultDeltaMode::Enabled, ForcedPlan, &DeltaDiagnostic));
	FFeasibilityPackage ForcedPackage;
	ASSERT_TRUE(BuildFeasibilityPackageFromV3(V3, false, ForcedPackage, Error, &ForcedPlan)) << Error;
	EXPECT_EQ(ForcedPackage.Bytes.size(), LoadedExplicitPackage.Bytes.size());
	EXPECT_NE(ForcedPackage.Bytes, LoadedExplicitPackage.Bytes);
	EXPECT_EQ(ForcedPlan.EmittedFieldCount, LoadedExplicitPlan.EmittedFieldCount);
	std::vector<uint8> ForcedProduction;
	ASSERT_TRUE(Durin::Asset::DastV4::WriteAssetPackage(
		Material->GetPackage(), ForcedProduction, {}, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	EXPECT_EQ(ForcedProduction, ForcedPackage.Bytes);
	ASSERT_TRUE(Material->ClearAuthoredOverride(OverridePath));
	EXPECT_FALSE(Material->HasAllocatedAuthoredOverrideLedger());
	FDefaultDeltaPlan ClearedPlan;
	ASSERT_TRUE(BuildDefaultDeltaPlan(Material, EDefaultDeltaMode::Enabled, ClearedPlan, &DeltaDiagnostic));
	EXPECT_TRUE(AreDefaultDeltaPlansEquivalent(DeltaPlan, ClearedPlan));

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
