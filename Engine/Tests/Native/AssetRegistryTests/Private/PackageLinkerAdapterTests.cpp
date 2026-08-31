#include "AssetRegistry/LegacyPackageConversion.h"
#include "PackageLinkerV7Adapter.h"
#include "DastV7Fixture.h"
#include "DObject/PackageFormat.h"
#include "Hash/XxHash.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Serialization/BinaryFormat.h"

#include <gtest/gtest.h>

namespace
{
	namespace Stream = Durin::Asset::PackageObjectStream;
	namespace Linker = Durin::ObjectPackage;
	namespace Adapter = Durin::Asset::Private;

	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto InlineBulkDescriptor(std::span<const std::byte> Payload)
		-> std::vector<std::byte>
	{
		Durin::FBinaryWriter Writer;
		Writer.WriteU64(1); // Stable field index.
		Writer.WriteU8(0); // Inline placement.
		Writer.WriteU8(0); // No legacy flags.
		Writer.WriteU16(1); // Inline alignment.
		Writer.WriteU32(1); // Legacy content-id version.
		Writer.WriteGuid({1, 2, 3, 4});
		Writer.WriteHash128(Durin::FXxHash128::HashBuffer(Payload));
		Writer.WriteU64(Payload.size());
		Writer.WriteU64(Payload.size());
		Writer.WriteU64(0); // Inline payload has no segment offset.
		Writer.WriteBytes(Payload);
		EXPECT_FALSE(Writer.HasError());
		return Writer.TakeBytes();
	}

	struct FV7Fixture
	{
		std::vector<std::byte> Bytes;
		Stream::FDecodedPackage Package;
	};

	auto BuildFixture() -> FV7Fixture
	{
		auto I8 = Stream::MakeType(Stream::ETypeOpcode::I8);
		auto I16 = Stream::MakeType(Stream::ETypeOpcode::I16);
		auto I32 = Stream::MakeType(Stream::ETypeOpcode::I32);
		auto I64 = Stream::MakeType(Stream::ETypeOpcode::I64);
		auto U8 = Stream::MakeType(Stream::ETypeOpcode::U8);
		auto U16 = Stream::MakeType(Stream::ETypeOpcode::U16);
		auto U32 = Stream::MakeType(Stream::ETypeOpcode::U32);
		auto U64 = Stream::MakeType(Stream::ETypeOpcode::U64);
		auto F32 = Stream::MakeType(Stream::ETypeOpcode::F32);
		auto F64 = Stream::MakeType(Stream::ETypeOpcode::F64);
		auto String = Stream::MakeType(Stream::ETypeOpcode::String);
		auto Name = Stream::MakeType(Stream::ETypeOpcode::Name);
		auto Guid = Stream::MakeType(Stream::ETypeOpcode::Guid);
		auto Enum = Stream::MakeType(Stream::ETypeOpcode::Enum, "Fixture::Mode",
			static_cast<uint64>(Stream::ETypeOpcode::I16));
		auto Transform = Stream::MakeType(Stream::ETypeOpcode::Intrinsic, {}, 5);
		auto Nested = Stream::MakeType(Stream::ETypeOpcode::Struct, "Fixture::Nested");
		auto Fixed = Stream::MakeType(Stream::ETypeOpcode::FixedArray, {}, 2, {I32});
		auto Array = Stream::MakeType(Stream::ETypeOpcode::Array, {}, 0, {Nested});
		auto Hard = Stream::MakeType(Stream::ETypeOpcode::HardRef, "Fixture::Object");
		auto Soft = Stream::MakeType(Stream::ETypeOpcode::SoftRef, "Fixture::Object");
		auto Map = Stream::MakeType(Stream::ETypeOpcode::Map, {}, 0, {String, Hard});
		auto Blob = Stream::MakeType(Stream::ETypeOpcode::Bytes);
		auto Bulk = Stream::MakeType(Stream::ETypeOpcode::BulkData);

		Stream::FValue NestedValue;
		NestedValue.FieldNames = {"Value"};
		NestedValue.Provenances = {Durin::EDefaultDeltaProvenance::Explicit};
		NestedValue.Elements = {{.Signed = 42}};
		Stream::FValue NestedForced = NestedValue;
		NestedForced.Provenances = {Durin::EDefaultDeltaProvenance::Forced};
		Stream::FValue TransformValue;
		TransformValue.ComponentBits = {
			std::bit_cast<uint64>(1.0), std::bit_cast<uint64>(0.0),
			std::bit_cast<uint64>(0.0), std::bit_cast<uint64>(0.0),
			std::bit_cast<uint64>(10.0), std::bit_cast<uint64>(20.0), std::bit_cast<uint64>(30.0),
			std::bit_cast<uint64>(1.0), std::bit_cast<uint64>(1.0), std::bit_cast<uint64>(1.0),
		};

		Stream::FPackageInput Input{
			.AssetClass = "Fixture::Asset",
			.Dependencies = {"/Game/External"},
			.AdditionalNames = {"FixtureName", "/Game/Soft"},
			.Types = {I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, String,
				Name, Guid, Enum, Transform, Nested, Fixed, Array, Hard, Soft, Map, Blob, Bulk},
			.Schemas = {
				{"Fixture::Nested", {{"Value", I32, 0}}},
				{"Fixture::Asset", {
					{"I8Min", I8, 0}, {"I16Min", I16, 0}, {"I32Min", I32, 0}, {"I64Min", I64, 0},
					{"U8Max", U8, 0}, {"U16Max", U16, 0}, {"U32Max", U32, 0}, {"U64Max", U64, 0},
					{"Float", F32, 0}, {"Double", F64, 0}, {"Text", String, 0}, {"Named", Name, 0},
					{"Id", Guid, 0}, {"Mode", Enum, 0}, {"Transform", Transform, 0},
					{"Nested", Nested, 0}, {"Fixed", Fixed, 0}, {"Array", Array, 0}, {"Lookup", Map, 0},
					{"External", Hard, 0}, {"Internal", Hard, 0}, {"Soft", Soft, 0},
					{"Blob", Blob, 0}, {"Bulk", Bulk, 0},
				}},
			},
			.CustomVersions = {{{1, 2, 3, 4}, 9, 9, 9, true, true}},
			.Objects = {
				{"Root", {}, "Fixture::Asset", "Root"},
				{"Root/Child", "Root", "Fixture::Object", "Child"},
			},
			.ObjectValues = {
				{"Root", {
					{"Fixture::Asset", "I8Min", Durin::EDefaultDeltaProvenance::Explicit, {.Signed = -128}},
					{"Fixture::Asset", "I16Min", Durin::EDefaultDeltaProvenance::Explicit, {.Signed = -32768}},
					{"Fixture::Asset", "I32Min", Durin::EDefaultDeltaProvenance::Explicit, {.Signed = std::numeric_limits<int32>::min()}},
					{"Fixture::Asset", "I64Min", Durin::EDefaultDeltaProvenance::Explicit, {.Signed = std::numeric_limits<int64>::min()}},
					{"Fixture::Asset", "U8Max", Durin::EDefaultDeltaProvenance::Explicit, {.Unsigned = std::numeric_limits<uint8>::max()}},
					{"Fixture::Asset", "U16Max", Durin::EDefaultDeltaProvenance::Explicit, {.Unsigned = std::numeric_limits<uint16>::max()}},
					{"Fixture::Asset", "U32Max", Durin::EDefaultDeltaProvenance::Explicit, {.Unsigned = std::numeric_limits<uint32>::max()}},
					{"Fixture::Asset", "U64Max", Durin::EDefaultDeltaProvenance::Explicit, {.Unsigned = std::numeric_limits<uint64>::max()}},
					{"Fixture::Asset", "Float", Durin::EDefaultDeltaProvenance::Explicit, {.FloatingBits = 0x7fc00000}},
					{"Fixture::Asset", "Double", Durin::EDefaultDeltaProvenance::Explicit, {.FloatingBits = 0x8000000000000000ull}},
					{"Fixture::Asset", "Text", Durin::EDefaultDeltaProvenance::Explicit, {.Text = "fixture"}},
					{"Fixture::Asset", "Named", Durin::EDefaultDeltaProvenance::Explicit, {.Text = "FixtureName"}},
					{"Fixture::Asset", "Id", Durin::EDefaultDeltaProvenance::Explicit, {.Guid = {5, 6, 7, 8}}},
					{"Fixture::Asset", "Mode", Durin::EDefaultDeltaProvenance::Explicit, {.Signed = -1}},
					{"Fixture::Asset", "Transform", Durin::EDefaultDeltaProvenance::Explicit, TransformValue},
					{"Fixture::Asset", "Nested", Durin::EDefaultDeltaProvenance::Explicit, NestedValue},
					{"Fixture::Asset", "Fixed", Durin::EDefaultDeltaProvenance::Explicit, {.Elements = {{.Signed = -1}, {.Signed = 2}}}},
					{"Fixture::Asset", "Array", Durin::EDefaultDeltaProvenance::Explicit, {.Elements = {NestedForced}}},
					{"Fixture::Asset", "Lookup", Durin::EDefaultDeltaProvenance::Explicit, {.Elements = {
						{.Text = "external"}, {.ReferenceTag = 2, .ReferenceId = 1},
						{.Text = "internal"}, {.ReferenceTag = 1, .ReferenceId = 2}}}},
					{"Fixture::Asset", "External", Durin::EDefaultDeltaProvenance::Explicit, {.ReferenceTag = 2, .ReferenceId = 1}},
					{"Fixture::Asset", "Internal", Durin::EDefaultDeltaProvenance::Explicit, {.ReferenceTag = 1, .ReferenceId = 2}},
					{"Fixture::Asset", "Soft", Durin::EDefaultDeltaProvenance::Explicit, {.Text = "/Game/Soft", .ReferenceTag = 1}},
					{"Fixture::Asset", "Blob", Durin::EDefaultDeltaProvenance::Explicit, {.Bytes = Bytes({0xde, 0xad})}},
					{"Fixture::Asset", "Bulk", Durin::EDefaultDeltaProvenance::Forced,
						{.Bytes = InlineBulkDescriptor(Bytes({0xbe, 0xef}))}},
				}},
				{"Root/Child", {}},
			},
		};

		FV7Fixture Fixture;
		Stream::FWriterDiagnostic WriterDiagnostic;
		EXPECT_TRUE(Stream::WritePackage(Input, Fixture.Bytes, &WriterDiagnostic)) << WriterDiagnostic.Message;
		Stream::FReaderDiagnostic ReaderDiagnostic;
		EXPECT_TRUE(Stream::DecodePackage(Fixture.Bytes, Fixture.Package, {}, &ReaderDiagnostic))
			<< ReaderDiagnostic.Message;
		return Fixture;
	}

	auto FindType(const Stream::FDecodedPackage& Package, Stream::ETypeOpcode Opcode)
		-> const Stream::FDecodedType&
	{
		const auto It = std::ranges::find(Package.Types, Opcode, &Stream::FDecodedType::Opcode);
		EXPECT_NE(It, Package.Types.end());
		return *It;
	}
}

TEST(FPackageLinkerAdapterTests, FrozenV7CorpusDecodesAndReencodesByteIdentically)
{
	const FV7Fixture Fixture = BuildFixture();
	ASSERT_FALSE(Fixture.Bytes.empty());
	std::vector<std::byte> Reencoded;
	Stream::FReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Stream::ReencodePackage(Fixture.Package, Reencoded, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Reencoded, Fixture.Bytes);

	Stream::FDecodedPackage Sentinel;
	Sentinel.Header.AssetClass = "sentinel";
	for (size_t Size : {size_t(0), size_t(12), Fixture.Bytes.size() - 1})
	{
		EXPECT_FALSE(Stream::DecodePackage(std::span(Fixture.Bytes).first(Size), Sentinel, {}, &Diagnostic));
		EXPECT_EQ(Sentinel.Header.AssetClass, "sentinel");
	}
}

TEST(FPackageLinkerAdapterTests, DecodedCanonicalTokensMatchFrozenCoreBytes)
{
	const FV7Fixture Fixture = BuildFixture();
	Stream::FReaderDiagnostic Diagnostic;
	std::vector<std::byte> Token;
	const auto& I8 = FindType(Fixture.Package, Stream::ETypeOpcode::I8);
	ASSERT_TRUE(Stream::BuildCanonicalMapKeyToken(Fixture.Package, I8, {.Signed = -128}, Token, &Diagnostic));
	EXPECT_EQ(Token, Bytes({2, 0}));
	const auto& F32 = FindType(Fixture.Package, Stream::ETypeOpcode::F32);
	ASSERT_TRUE(Stream::BuildCanonicalMapKeyToken(Fixture.Package, F32, {.FloatingBits = 0x80000000}, Token, &Diagnostic));
	EXPECT_EQ(Token, Bytes({10, 0x80, 0, 0, 0}));
	const auto& Enum = FindType(Fixture.Package, Stream::ETypeOpcode::Enum);
	ASSERT_TRUE(Stream::BuildCanonicalMapKeyToken(Fixture.Package, Enum, {.Signed = -1}, Token, &Diagnostic));
	EXPECT_EQ(Token, Bytes({13, 0x7f, 0xff}));
	const auto& Name = FindType(Fixture.Package, Stream::ETypeOpcode::Name);
	ASSERT_TRUE(Stream::BuildCanonicalMapKeyToken(Fixture.Package, Name, {.Text = "FixtureName"}, Token, &Diagnostic));
	EXPECT_EQ(Token, Bytes({18, 0, 0, 0, 0, 0, 0, 0, 11,
		'F', 'i', 'x', 't', 'u', 'r', 'e', 'N', 'a', 'm', 'e', 0, 0, 0, 0}));

	std::vector<std::byte> Sentinel = Bytes({0xaa});
	const auto& Map = FindType(Fixture.Package, Stream::ETypeOpcode::Map);
	EXPECT_FALSE(Stream::BuildCanonicalMapKeyToken(Fixture.Package, Map, {}, Sentinel, &Diagnostic));
	EXPECT_EQ(Sentinel, Bytes({0xaa}));
}

TEST(FPackageLinkerAdapterTests, SupportedV7PackageAdaptsDeterministicallyWithoutObjects)
{
	FV7Fixture Fixture = BuildFixture();
	Fixture.Package.CustomVersions.front().EmissionValue = 9;
	Fixture.Package.CustomVersions.front().MaximumSupported = 9;
	Fixture.Package.CustomVersions.front().bCodecKnown = true;
	Fixture.Package.CustomVersions.front().bRequiredForInterpretation = true;
	Linker::FLinkerTables First;
	Linker::FLinkerTables Second;
	Adapter::FV7LinkerAdapterDiagnostic Diagnostic;
	ASSERT_TRUE(Adapter::AdaptDecodedPackageV7(Fixture.Package, "/Game/Fixture", First, &Diagnostic))
		<< Diagnostic.Message;
	ASSERT_TRUE(Adapter::AdaptDecodedPackageV7(Fixture.Package, "/Game/Fixture", Second, &Diagnostic))
		<< Diagnostic.Message;
	EXPECT_EQ(First, Second);
	EXPECT_EQ(First.Summary.PackageName, "/Game/Fixture");
	EXPECT_EQ(First.Summary.AssetClass, "Fixture::Asset");
	EXPECT_EQ(First.Summary.HardPackageReferences, std::vector<std::string>{"/Game/External"});
	EXPECT_EQ(First.Summary.SoftPackageReferences, std::vector<std::string>{"/Game/Soft"});
	ASSERT_EQ(First.Imports.size(), 1);
	EXPECT_EQ(First.Imports.front().PackageName, "/Game/External");
	ASSERT_EQ(First.Exports.size(), 2);
	EXPECT_EQ(First.Exports[0].ObjectName, "Root");
	EXPECT_EQ(First.Exports[1].Outer.ToRaw(), 1);
	ASSERT_EQ(First.Exports[0].Properties.size(), 24);
	const auto Internal = std::ranges::find(First.Exports[0].Properties, "Internal", &Linker::FPropertyTag::FieldName);
	ASSERT_NE(Internal, First.Exports[0].Properties.end());
	EXPECT_TRUE(Internal->Value.Reference.IsExport());
	EXPECT_EQ(Internal->Value.Reference.GetTableIndex(), 1u);
	const auto External = std::ranges::find(First.Exports[0].Properties, "External", &Linker::FPropertyTag::FieldName);
	ASSERT_NE(External, First.Exports[0].Properties.end());
	EXPECT_TRUE(External->Value.Reference.IsImport());
	const std::vector<Linker::FCustomVersion> ExpectedVersions{
		{{1, 2, 3, 4}, 9, 9, 9, true, true}};
	EXPECT_EQ(First.CustomVersions, ExpectedVersions);
}

TEST(FPackageLinkerAdapterTests, UnsupportedAndMalformedV7InputsFailAtomicallyAtLogicalLocation)
{
	FV7Fixture Fixture = BuildFixture();
	Linker::FLinkerTables Output;
	Output.Summary.PackageName = "sentinel";
	Adapter::FV7LinkerAdapterDiagnostic Diagnostic;
	Fixture.Package.ObjectValues.front().Overrides.front().Provenance = 2;
	Fixture.Package.ObjectValues.front().Overrides.front().DescriptorClosure = Bytes({1});
	EXPECT_FALSE(Adapter::AdaptDecodedPackageV7(Fixture.Package, "/Game/Fixture", Output, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Adapter::EV7LinkerAdapterFailure::UnsupportedRetainedValue);
	EXPECT_EQ(Diagnostic.LogicalPath, "Root");
	EXPECT_EQ(Output.Summary.PackageName, "sentinel");

	Fixture = BuildFixture();
	Fixture.Package.Objects[1].OuterId = 99;
	EXPECT_FALSE(Adapter::AdaptDecodedPackageV7(Fixture.Package, "/Game/Fixture", Output, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Adapter::EV7LinkerAdapterFailure::InvalidTopology);
	EXPECT_EQ(Diagnostic.LogicalPath, "Root/Child");
	EXPECT_EQ(Output.Summary.PackageName, "sentinel");
}

TEST(FPackageLinkerAdapterTests, UnmaterializedStructTypeWithoutSchemaRemainsRepresentable)
{
	FV7Fixture Fixture = BuildFixture();
	Fixture.Package.Types.push_back({
		.Opcode = Stream::ETypeOpcode::Struct,
		.QualifiedName = "Fixture::Unmaterialized"});
	Linker::FLinkerTables Output;
	Adapter::FV7LinkerAdapterDiagnostic Diagnostic;
	ASSERT_TRUE(Adapter::AdaptDecodedPackageV7(
		Fixture.Package, "/Game/Fixture", Output, &Diagnostic))
		<< Diagnostic.Message;
	const auto It = std::ranges::find(
		Output.Types, "Fixture::Unmaterialized", &Linker::FSerializedType::QualifiedName);
	ASSERT_NE(It, Output.Types.end());
	EXPECT_TRUE(It->Children.empty());
}

TEST(FPackageLinkerAdapterTests, CompleteV7PackageConvertsToCanonicalV8WithoutObjects)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	const FV7Fixture Fixture = BuildFixture();
	std::vector<std::byte> V7Package;
	ASSERT_TRUE(Durin::Testing::DastV7Fixture::BuildPackageFromObjectStream(
		Fixture.Bytes, V7Package));

	std::vector<std::byte> FirstMain;
	std::vector<std::byte> FirstBulk;
	Durin::Asset::FLegacyPackageConversionDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::Asset::ConvertDastV7PackageToV8(
		V7Package, {}, "/Game/Fixture", FirstMain, FirstBulk, &Diagnostic))
		<< Diagnostic.Message;
	EXPECT_FALSE(FirstMain.empty());
	EXPECT_TRUE(FirstBulk.empty());

	Linker::FLinkerTables Converted;
	Linker::FPackageReaderDiagnostic ReaderDiagnostic;
	ASSERT_TRUE(Linker::ReadPackageV8(
		FirstMain, FirstBulk, "/Game/Fixture", Converted, &ReaderDiagnostic))
		<< ReaderDiagnostic.Message;
	ASSERT_EQ(Converted.Exports.size(), 2);
	const auto Bulk = std::ranges::find(
		Converted.Exports.front().Properties, "Bulk", &Linker::FPropertyTag::FieldName);
	ASSERT_NE(Bulk, Converted.Exports.front().Properties.end());
	EXPECT_EQ(Bulk->Value.Bytes, Bytes({0xbe, 0xef}));
	EXPECT_EQ(Bulk->Value.BulkElementSize, 1u);
	EXPECT_EQ(Bulk->Value.BulkAlignment, 1u);
	EXPECT_EQ(Bulk->Value.BulkStorage, Linker::EBulkStorageKind::Inline);

	std::vector<std::byte> SecondMain;
	std::vector<std::byte> SecondBulk;
	ASSERT_TRUE(Durin::Asset::ConvertDastV7PackageToV8(
		V7Package, {}, "/Game/Fixture", SecondMain, SecondBulk, &Diagnostic));
	EXPECT_EQ(SecondMain, FirstMain);
	EXPECT_EQ(SecondBulk, FirstBulk);
}

TEST(FPackageLinkerAdapterTests, ConversionFailureDoesNotPublishPartialOutputs)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	FV7Fixture Fixture = BuildFixture();
	const auto Override = std::ranges::find_if(
		Fixture.Package.ObjectValues.front().Overrides,
		[&](const Stream::FDecodedOverride& Candidate) {
			if (Candidate.SchemaId == 0
				|| Candidate.SchemaId > Fixture.Package.Schemas.size()) return false;
			const auto& Schema = Fixture.Package.Schemas[Candidate.SchemaId - 1];
			return Candidate.FieldId != 0 && Candidate.FieldId <= Schema.Fields.size()
				&& Schema.Fields[Candidate.FieldId - 1].Name == "Bulk";
		});
	ASSERT_NE(Override, Fixture.Package.ObjectValues.front().Overrides.end());
	ASSERT_GT(Override->Value.Bytes.size(), 32u);
	Override->Value.Bytes[32] ^= std::byte{0x1};
	Stream::FReaderDiagnostic ReencodeDiagnostic;
	ASSERT_TRUE(Stream::ReencodePackage(
		Fixture.Package, Fixture.Bytes, &ReencodeDiagnostic));
	std::vector<std::byte> V7Package;
	ASSERT_TRUE(Durin::Testing::DastV7Fixture::BuildPackageFromObjectStream(
		Fixture.Bytes, V7Package));
	std::vector<std::byte> Main = Bytes({0xaa});
	std::vector<std::byte> Bulk = Bytes({0xbb});
	Durin::Asset::FLegacyPackageConversionDiagnostic Diagnostic;
	EXPECT_FALSE(Durin::Asset::ConvertDastV7PackageToV8(
		V7Package, {}, "/Game/Fixture", Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure,
		Durin::Asset::ELegacyPackageConversionFailure::InvalidBulkData);
	EXPECT_EQ(Diagnostic.LogicalPath, "Root::Fixture::Asset::Bulk");
	EXPECT_EQ(Main, Bytes({0xaa}));
	EXPECT_EQ(Bulk, Bytes({0xbb}));
}

TEST(FPackageLinkerAdapterTests, RedirectAndTruncatedV7PackagesRespectConversionBoundary)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	Durin::FAssetPath Source;
	Durin::FAssetPath Target;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Redirect", Source));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Target", Target));
	auto DestinationType = Stream::MakeType(Stream::ETypeOpcode::HardRef, "Durin::DObject");
	Stream::FPackageInput RedirectInput{
		.AssetClass = "Durin::Asset::DAssetRedirector",
		.EntryKind = Durin::Asset::EAssetRegistryEntryKind::Redirector,
		.RedirectDestination = Target.ToString(),
		.Dependencies = {Target.ToString()},
		.Types = {DestinationType},
		.Schemas = {{"Durin::Asset::DAssetRedirector",
			{{"DestinationObject", DestinationType, 0}}}},
		.Objects = {{std::string(Source.GetAssetName()), {},
			"Durin::Asset::DAssetRedirector", std::string(Source.GetAssetName())}},
		.ObjectValues = {{std::string(Source.GetAssetName()), {{
			.SchemaName = "Durin::Asset::DAssetRedirector",
			.FieldName = "DestinationObject",
			.Value = {.ReferenceTag = 2, .ReferenceId = 1}}}}}};
	std::vector<std::byte> ObjectStream;
	Stream::FWriterDiagnostic WriterDiagnostic;
	ASSERT_TRUE(Stream::WritePackage(RedirectInput, ObjectStream, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	std::vector<std::byte> V7Package;
	ASSERT_TRUE(Durin::Testing::DastV7Fixture::BuildPackageFromObjectStream(
		ObjectStream, V7Package));
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	Durin::Asset::FLegacyPackageConversionDiagnostic Diagnostic;
	ASSERT_TRUE(Durin::Asset::ConvertDastV7PackageToV8(
		V7Package, {}, Source.GetView(), Main, Bulk, &Diagnostic))
		<< Diagnostic.Message;
	Linker::FLinkerTables Converted;
	ASSERT_TRUE(Linker::ReadPackageV8(Main, Bulk, Source.GetView(), Converted));
	EXPECT_TRUE(Converted.Summary.bRedirect);
	EXPECT_EQ(Converted.Summary.RedirectDestination, Target.GetView());

	const std::vector<std::byte> SentinelMain = Bytes({0xaa});
	const std::vector<std::byte> SentinelBulk = Bytes({0xbb});
	for (const size_t Size : {size_t(0), size_t(31), V7Package.size() - 1})
	{
		Main = SentinelMain;
		Bulk = SentinelBulk;
		EXPECT_FALSE(Durin::Asset::ConvertDastV7PackageToV8(
			std::span(V7Package).first(Size), {}, Source.GetView(),
			Main, Bulk, &Diagnostic));
		EXPECT_EQ(Main, SentinelMain);
		EXPECT_EQ(Bulk, SentinelBulk);
	}
}
