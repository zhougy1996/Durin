#include "DObject/PackageFormat.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

#include <gtest/gtest.h>

namespace
{
	namespace Package = Durin::ObjectPackage;

	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto MakeFixture(bool bShuffled = false) -> Package::FLinkerTables
	{
		const Package::FSerializedType I32{.Kind = Package::EValueKind::I32};
		const Package::FSerializedType String{.Kind = Package::EValueKind::String};
		const Package::FSerializedType U32{.Kind = Package::EValueKind::U32};
		const Package::FSerializedType Map{
			.Kind = Package::EValueKind::Map, .Children = {String, U32}};
		const Package::FSerializedType Bulk{.Kind = Package::EValueKind::BulkData};

		Package::FLinkerTables Linker;
		Linker.Summary.PackageName = "/Game/WriterFixture";
		Linker.Summary.AssetClass = "Example::WriterAsset";
		Linker.Summary.HardPackageReferences = {"/Game/HardB", "/Game/HardA"};
		Linker.Summary.SoftPackageReferences = {"/Game/SoftA"};
		Linker.Summary.SearchableNames = {"SearchB", "SearchA"};
		Package::FPackageIndex::TryExport(0, Linker.Summary.MainExport);
		Linker.Names = bShuffled ? std::vector<std::string>{"unused-z", "unused-a"}
			: std::vector<std::string>{"unused-a", "unused-z"};
		Linker.Types = bShuffled ? std::vector{Map, String, I32, Bulk, U32}
			: std::vector{I32, U32, String, Map, Bulk};
		Linker.CustomVersions = {
			{.Guid = Durin::FGuid{4, 3, 2, 1}, .Value = 7, .EmissionValue = 6,
				.MaximumSupported = 9, .bCodecKnown = true, .bRequiredForInterpretation = true}};

		Package::FSerializedSchema Schema{
			.QualifiedName = "Example::WriterAsset",
			.Fields = {
				{.Name = "Count", .Type = I32, .AuthoredFlags = 1},
				{.Name = "External", .Type = Bulk},
				{.Name = "Inline", .Type = Bulk},
				{.Name = "Labels", .Type = Map},
			}};
		if (bShuffled) std::ranges::reverse(Schema.Fields);
		Linker.Schemas = {std::move(Schema)};

		Package::FSerializedValue Labels;
		Labels.Elements = bShuffled
			? std::vector<Package::FSerializedValue>{{.Text = "z"}, {.Unsigned = 9}, {.Text = "a"}, {.Unsigned = 1}}
			: std::vector<Package::FSerializedValue>{{.Text = "a"}, {.Unsigned = 1}, {.Text = "z"}, {.Unsigned = 9}};
		Package::FSerializedValue Inline;
		Inline.Bytes = Bytes({0x10, 0x20, 0x30});
		Inline.BulkElementSize = 1;
		Inline.BulkAlignment = 8;
		Inline.BulkStorage = Package::EBulkStorageKind::Inline;
		Package::FSerializedValue External;
		External.Bytes = Bytes({0xaa, 0xbb, 0xcc, 0xdd});
		External.BulkElementSize = 2;
		External.BulkAlignment = 16;
		External.BulkStorage = Package::EBulkStorageKind::External;

		Package::FPackageExport Export{
			.ObjectName = "WriterFixture",
			.ClassName = "Example::WriterAsset",
			.Properties = {
				{.DeclaringType = "Example::WriterAsset", .FieldName = "Count", .Type = I32,
					.Provenance = Package::EPropertyProvenance::Explicit, .Value = {.Signed = -17}},
				{.DeclaringType = "Example::WriterAsset", .FieldName = "External", .Type = Bulk,
					.Provenance = Package::EPropertyProvenance::Forced, .Value = External},
				{.DeclaringType = "Example::WriterAsset", .FieldName = "Inline", .Type = Bulk,
					.Value = Inline},
				{.DeclaringType = "Example::WriterAsset", .FieldName = "Labels", .Type = Map,
					.Value = Labels},
			}};
		if (bShuffled) std::ranges::reverse(Export.Properties);
		Linker.Exports = {std::move(Export)};
		return Linker;
	}

	auto MakeAllKindsFixture(uint32 FloatNanBits = 0x7fc00001u) -> Package::FLinkerTables
	{
		Package::FLinkerTables Linker;
		Linker.Summary.PackageName = "/Game/AllKinds";
		Linker.Summary.AssetClass = "Example::AllKinds";
		Package::FPackageIndex Self;
		Package::FPackageIndex::TryExport(0, Self);
		Linker.Summary.MainExport = Self;
		Package::FSerializedSchema Schema{.QualifiedName = "Example::AllKinds"};
		Package::FPackageExport Export{.ObjectName = "AllKinds", .ClassName = "Example::AllKinds"};
		auto Add = [&](std::string Name, Package::FSerializedType Type, Package::FSerializedValue Value)
		{
			Schema.Fields.push_back({.Name = Name, .Type = Type});
			Export.Properties.push_back({.DeclaringType = "Example::AllKinds", .FieldName = std::move(Name),
				.Type = std::move(Type), .Value = std::move(Value)});
		};

		Add("Bool", {.Kind = Package::EValueKind::Bool}, {.Bool = true});
		Add("I8", {.Kind = Package::EValueKind::I8}, {.Signed = -8});
		Add("I16", {.Kind = Package::EValueKind::I16}, {.Signed = -1600});
		Add("I32", {.Kind = Package::EValueKind::I32}, {.Signed = -32000});
		Add("I64", {.Kind = Package::EValueKind::I64}, {.Signed = -64000});
		Add("U8", {.Kind = Package::EValueKind::U8}, {.Unsigned = 8});
		Add("U16", {.Kind = Package::EValueKind::U16}, {.Unsigned = 1600});
		Add("U32", {.Kind = Package::EValueKind::U32}, {.Unsigned = 32000});
		Add("U64", {.Kind = Package::EValueKind::U64}, {.Unsigned = 64000});
		Add("F32", {.Kind = Package::EValueKind::F32}, {.FloatingBits = FloatNanBits});
		Add("F64", {.Kind = Package::EValueKind::F64}, {.FloatingBits = 0x8000000000000000ull});
		Add("String", {.Kind = Package::EValueKind::String}, {.Text = "hello"});
		Add("Name", {.Kind = Package::EValueKind::Name}, {.Text = "PlainName", .NameNumber = 7});
		Add("Guid", {.Kind = Package::EValueKind::Guid}, {.Guid = Durin::FGuid{1, 2, 3, 4}});
		Add("Enum", {.Kind = Package::EValueKind::Enum, .QualifiedName = "Example::Mode",
			.Parameter = uint64(Package::EValueKind::U8)}, {.Unsigned = 2});
		Add("Intrinsic", {.Kind = Package::EValueKind::Intrinsic, .Parameter = 2},
			{.ComponentBits = {0x3ff0000000000000ull, 0x4000000000000000ull, 0x4008000000000000ull}});
		Add("Struct", {.Kind = Package::EValueKind::Struct, .QualifiedName = "Example::Pair",
			.Children = {{.Kind = Package::EValueKind::I32}, {.Kind = Package::EValueKind::String}}},
			{.Elements = {{.Signed = 5}, {.Text = "value"}}, .FieldNames = {"Number", "Text"}});
		Add("Fixed", {.Kind = Package::EValueKind::FixedArray, .Parameter = 2,
			.Children = {{.Kind = Package::EValueKind::U16}}},
			{.Elements = {{.Unsigned = 1}, {.Unsigned = 2}}});
		Add("Array", {.Kind = Package::EValueKind::Array,
			.Children = {{.Kind = Package::EValueKind::String}}},
			{.Elements = {{.Text = "x"}, {.Text = "y"}}});
		Add("Map", {.Kind = Package::EValueKind::Map,
			.Children = {{.Kind = Package::EValueKind::String}, {.Kind = Package::EValueKind::I64}}},
			{.Elements = {{.Text = "b"}, {.Signed = 2}, {.Text = "a"}, {.Signed = 1}}});
		Add("Hard", {.Kind = Package::EValueKind::HardReference}, {.Reference = Self});
		Add("Soft", {.Kind = Package::EValueKind::SoftReference}, {.Text = "/Game/Soft.Target"});
		Add("Byte", {.Kind = Package::EValueKind::Byte}, {.Unsigned = 0xfe});
		Add("Bytes", {.Kind = Package::EValueKind::Bytes}, {.Bytes = Bytes({1, 2, 3, 4})});
		Package::FSerializedValue Bulk;
		Bulk.Bytes = Bytes({9, 8, 7, 6});
		Bulk.BulkElementSize = 1;
		Bulk.BulkAlignment = 4;
		Bulk.BulkStorage = Package::EBulkStorageKind::Inline;
		Add("Bulk", {.Kind = Package::EValueKind::BulkData}, std::move(Bulk));
		Linker.Schemas = {std::move(Schema)};
		Linker.Exports = {std::move(Export)};
		return Linker;
	}

	auto MakeReferenceFixture(bool bShuffled) -> Package::FLinkerTables
	{
		const Package::FSerializedType Hard{.Kind = Package::EValueKind::HardReference};
		Package::FLinkerTables Linker;
		Linker.Summary.PackageName = "/Game/References";
		Linker.Summary.AssetClass = "Example::References";
		Linker.Summary.HardPackageReferences = {"/Game/DepB", "/Game/DepA"};
		Linker.Schemas = {{.QualifiedName = "Example::References", .Fields = {{.Name = "Target", .Type = Hard}}}};
		Linker.Imports = bShuffled
			? std::vector<Package::FPackageImport>{{.PackageName = "/Game/DepB"}, {.PackageName = "/Game/DepA"}}
			: std::vector<Package::FPackageImport>{{.PackageName = "/Game/DepA"}, {.PackageName = "/Game/DepB"}};
		Package::FPackageIndex Root;
		Package::FPackageIndex Target;
		Package::FPackageIndex::TryImport(bShuffled ? 1 : 0, Target);
		Package::FPackageExport RootExport{
			.ObjectName = "References", .ClassName = "Example::References",
			.Properties = {{.DeclaringType = "Example::References", .FieldName = "Target",
				.Type = Hard, .Value = {.Reference = Target}}}};
		Package::FPackageExport ChildExport{.ObjectName = "Child", .ClassName = "Example::Child"};
		if (bShuffled)
		{
			Package::FPackageIndex::TryExport(1, Root);
			ChildExport.Outer = Root;
			Linker.Exports = {std::move(ChildExport), std::move(RootExport)};
		}
		else
		{
			Package::FPackageIndex::TryExport(0, Root);
			ChildExport.Outer = Root;
			Linker.Exports = {std::move(RootExport), std::move(ChildExport)};
		}
		Linker.Summary.MainExport = Root;
		return Linker;
	}

	template<std::unsigned_integral T>
	auto Read(std::span<const std::byte> Bytes, uint64 Offset) -> T
	{
		T Result = 0;
		EXPECT_TRUE(Durin::ReadLittleEndianAt(Bytes, Offset, Result));
		return Result;
	}

	template<std::unsigned_integral T>
	auto Write(std::span<std::byte> Bytes, uint64 Offset, T Value) -> void
	{
		ASSERT_LE(Offset + sizeof(T), Bytes.size());
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[static_cast<size_t>(Offset + Index)] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto RehashSection(std::vector<std::byte>& Main, uint32 SectionIndex) -> void
	{
		const uint64 DirectoryEntry = Package::DastV8DirectoryOffset + uint64(SectionIndex) * 48;
		const uint64 Offset = Read<uint64>(Main, DirectoryEntry + 8);
		const uint64 Size = Read<uint64>(Main, DirectoryEntry + 16);
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Main).subspan(static_cast<size_t>(Offset), static_cast<size_t>(Size)));
		Write<uint64>(Main, DirectoryEntry + 24, Hash.HashLow);
		Write<uint64>(Main, DirectoryEntry + 32, Hash.HashHigh);
		const uint64 HeaderBytes = Read<uint64>(Main, 32);
		ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(
			std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(),
			{Package::DastV8MaximumHeaderBytes, Package::DastV8MaximumPackageBytes}));
	}
}

TEST(FPackageWriterContractTests, FrozenLayoutAndFixtureHashAreExact)
{
	static_assert(Package::DastV8FormatHeaderOffset == 64);
	static_assert(Package::DastV8DirectoryOffset == 96);
	static_assert(Package::DastV8FirstSectionOffset == 528);
	static_assert(Package::DastV8SectionCount == 9);
	static_assert(Package::DastV8SectionEntryBytes == 48);

	const Package::FLinkerTables Linker = MakeFixture();
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	Package::FPackageWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Package::WritePackageV8(Linker, Main, Bulk, &Diagnostic)) << Diagnostic.Message;
	ASSERT_GE(Main.size(), Package::DastV8FirstSectionOffset);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Main.data()), 4), "DURF");
	EXPECT_EQ(Read<uint32>(Main, 24), Package::DastV8FormatVersion);
	EXPECT_EQ(Read<uint64>(Main, 40), Main.size());
	EXPECT_EQ(Main.size(), 1188u);
	EXPECT_EQ(Read<uint64>(Main, 32), 930u);
	EXPECT_EQ(Read<uint64>(Main, 72), Package::DastV8DirectoryOffset);
	EXPECT_EQ(Read<uint32>(Main, 80), Package::DastV8SectionCount);
	EXPECT_EQ(Read<uint32>(Main, 84), Package::DastV8SectionEntryBytes);
	EXPECT_EQ(Read<uint64>(Main, Package::DastV8DirectoryOffset + 8), Package::DastV8FirstSectionOffset);
	constexpr std::array<uint64, Package::DastV8SectionCount> SectionOffsets{
		528, 568, 925, 930, 938, 970, 1019, 1074, 1185};
	for (uint64 Index = 0; Index < Package::DastV8SectionCount; ++Index)
		EXPECT_EQ(Read<uint64>(Main, Package::DastV8DirectoryOffset + Index * 48 + 8),
			SectionOffsets[Index]) << Index;
	const uint64 ImportOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 2 * 48 + 8);
	const uint64 ImportBytes = Read<uint64>(Main, Package::DastV8DirectoryOffset + 2 * 48 + 16);
	EXPECT_EQ(Read<uint64>(Main, 32), ImportOffset + ImportBytes);
	EXPECT_EQ(Bulk, Bytes({0xaa, 0xbb, 0xcc, 0xdd}));
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "04c1cd13fa47b749aab2d9e18c0074bc");
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bulk).ToString(), "ab65044d6377f7528d403d7d59bb88f3");
}

TEST(FPackageWriterContractTests, EquivalentDiscoveryOrdersProduceIdenticalManifestAndBytes)
{
	const Package::FLinkerTables Canonical = MakeFixture(false);
	const Package::FLinkerTables Shuffled = MakeFixture(true);
	Package::FPackageWriterManifest CanonicalManifest;
	Package::FPackageWriterManifest ShuffledManifest;
	ASSERT_TRUE(Package::FreezePackageV8(Canonical, CanonicalManifest));
	ASSERT_TRUE(Package::FreezePackageV8(Shuffled, ShuffledManifest));
	EXPECT_EQ(CanonicalManifest, ShuffledManifest);
	EXPECT_EQ(CanonicalManifest.Names, (std::vector<std::string>{
		"/Game/HardA", "/Game/HardB", "/Game/SoftA", "/Game/WriterFixture", "Count",
		"Example::WriterAsset", "External", "Inline", "Labels", "SearchA", "SearchB",
		"WriterFixture", "WriterFixture.Example::WriterAsset.External",
		"WriterFixture.Example::WriterAsset.Inline", "unused-a", "unused-z"}));
	EXPECT_EQ(CanonicalManifest.Schemas, (std::vector<std::string>{"Example::WriterAsset"}));
	EXPECT_TRUE(CanonicalManifest.Imports.empty());
	EXPECT_EQ(CanonicalManifest.Exports, (std::vector<std::string>{"WriterFixture"}));
	EXPECT_EQ(CanonicalManifest.BulkValues, (std::vector<std::string>{
		"WriterFixture.Example::WriterAsset.External",
		"WriterFixture.Example::WriterAsset.Inline"}));
	std::vector<std::byte> MainA, BulkA, MainB, BulkB;
	ASSERT_TRUE(Package::WritePackageV8(Canonical, MainA, BulkA));
	ASSERT_TRUE(Package::WritePackageV8(Shuffled, MainB, BulkB));
	EXPECT_EQ(MainA, MainB);
	EXPECT_EQ(BulkA, BulkB);
}

TEST(FPackageWriterContractTests, FailuresAreTypedAndAtomic)
{
	Package::FLinkerTables Invalid = MakeFixture();
	Invalid.Exports.front().Properties[1].Value.BulkAlignment = 3;
	std::vector<std::byte> Main = Bytes({1, 2, 3});
	std::vector<std::byte> Bulk = Bytes({4, 5});
	const auto OriginalMain = Main;
	const auto OriginalBulk = Bulk;
	Package::FPackageWriterDiagnostic Diagnostic;
	EXPECT_FALSE(Package::WritePackageV8(Invalid, Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::InvalidBulkData);
	EXPECT_EQ(Main, OriginalMain);
	EXPECT_EQ(Bulk, OriginalBulk);
	EXPECT_FALSE(Package::WritePackageV8(MakeFixture(), Main, Main, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::AliasedOutput);
	EXPECT_EQ(Main, OriginalMain);
}

TEST(FPackageWriterContractTests, EmptyExternalSegmentUsesZeroExtentAndDigest)
{
	Package::FLinkerTables Linker = MakeFixture();
	Linker.Exports.front().Properties[1].Value.BulkStorage = Package::EBulkStorageKind::Inline;
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(Linker, Main, Bulk));
	EXPECT_TRUE(Bulk.empty());
	const uint64 RegistryOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 8);
	const uint64 RegistryBytes = Read<uint64>(Main, Package::DastV8DirectoryOffset + 16);
	ASSERT_GE(RegistryBytes, 24u);
	EXPECT_TRUE(std::ranges::all_of(std::span(Main).subspan(
		static_cast<size_t>(RegistryOffset + RegistryBytes - 24), 24),
		[](std::byte Value) { return Value == std::byte{0}; }));
}

TEST(FPackageWriterContractTests, RedirectWithoutBulkHasFrozenBytes)
{
	Package::FLinkerTables Linker = MakeFixture();
	Linker.Summary.bRedirect = true;
	Linker.Summary.RedirectDestination = "/Game/RedirectTarget";
	std::erase_if(Linker.Schemas.front().Fields, [](const Package::FSerializedField& Field)
	{ return Field.Type.Kind == Package::EValueKind::BulkData; });
	std::erase_if(Linker.Exports.front().Properties, [](const Package::FPropertyTag& Property)
	{ return Property.Type.Kind == Package::EValueKind::BulkData; });
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(Linker, Main, Bulk));
	EXPECT_EQ(Read<uint32>(Main, Package::DastV8FormatHeaderOffset), 1u);
	EXPECT_TRUE(Bulk.empty());
	EXPECT_EQ(Read<uint64>(Main, Package::DastV8DirectoryOffset + 8 * 48 + 16), 0u);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "7555df0322a9379ac95c7d3676acf9eb");
}

TEST(FPackageWriterContractTests, EveryNativeValueKindHasOneFrozenFixture)
{
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	Package::FPackageWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Package::WritePackageV8(MakeAllKindsFixture(), Main, Bulk, &Diagnostic))
		<< Diagnostic.LogicalPath << ": " << Diagnostic.Message;
	EXPECT_TRUE(Bulk.empty());
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "3314cfdf1a99980aab0ba0a275c76cd8");

	std::vector<std::byte> AlternateNanMain;
	ASSERT_TRUE(Package::WritePackageV8(MakeAllKindsFixture(0x7fffffffu), AlternateNanMain, Bulk));
	EXPECT_EQ(Main, AlternateNanMain);
}

TEST(FPackageWriterContractTests, MapCollisionsAndInvalidTopologyFailAtomically)
{
	Package::FLinkerTables InvalidMap = MakeFixture();
	auto& Elements = InvalidMap.Exports.front().Properties.back().Value.Elements;
	Elements[2].Text = Elements[0].Text;
	std::vector<std::byte> Main = Bytes({7});
	std::vector<std::byte> Bulk = Bytes({8});
	Package::FPackageWriterDiagnostic Diagnostic;
	EXPECT_FALSE(Package::WritePackageV8(InvalidMap, Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::DuplicateIdentity);
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));

	Package::FLinkerTables Cyclic = MakeFixture();
	Cyclic.Exports.front().Outer = Cyclic.Summary.MainExport;
	EXPECT_FALSE(Package::WritePackageV8(Cyclic, Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::InvalidTopology);
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));
}

TEST(FPackageWriterContractTests, ImportExportAndReferenceIdsRemapAcrossShuffledTables)
{
	const Package::FLinkerTables A = MakeReferenceFixture(false);
	const Package::FLinkerTables B = MakeReferenceFixture(true);
	std::vector<std::byte> MainA, BulkA, MainB, BulkB;
	Package::FPackageWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Package::WritePackageV8(A, MainA, BulkA, &Diagnostic)) << Diagnostic.Message;
	ASSERT_TRUE(Package::WritePackageV8(B, MainB, BulkB, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(MainA, MainB);
	EXPECT_EQ(BulkA, BulkB);
	Package::FPackageWriterManifest Manifest;
	ASSERT_TRUE(Package::FreezePackageV8(B, Manifest));
	EXPECT_EQ(Manifest.Imports, (std::vector<std::string>{"/Game/DepA", "/Game/DepB"}));
	EXPECT_EQ(Manifest.Exports, (std::vector<std::string>{"References", "References/Child"}));
}

TEST(FPackageReaderContractTests, RegistryProjectionUsesOnlyDeclaredFrontMatter)
{
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(MakeFixture(), Main, Bulk));
	const uint64 HeaderBytes = Read<uint64>(Main, 32);
	Package::FPackageV8RegistryData Registry;
	Package::FPackageReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Package::ReadPackageV8Registry(std::span(Main).first(static_cast<size_t>(HeaderBytes)),
		Main.size(), Bulk.size(), "/Game/WriterFixture", Registry, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Registry.PackageName, "/Game/WriterFixture");
	EXPECT_EQ(Registry.AssetClass, "Example::WriterAsset");
	EXPECT_FALSE(Registry.bRedirect);
	EXPECT_EQ(Registry.MainExportId, 1u);
	EXPECT_EQ(Registry.ExportCount, 1u);
	EXPECT_EQ(Registry.HardPackageReferences,
		(std::vector<std::string>{"/Game/HardA", "/Game/HardB"}));
	EXPECT_EQ(Registry.SoftPackageReferences, (std::vector<std::string>{"/Game/SoftA"}));
	EXPECT_EQ(Registry.SearchableNames, (std::vector<std::string>{"SearchA", "SearchB"}));
	EXPECT_EQ(Registry.ExternalBulkBytes, Bulk.size());
}

TEST(FPackageReaderContractTests, CanonicalFixturesReadAndWriteByteIdentically)
{
	for (const Package::FLinkerTables& Source : {MakeFixture(), MakeFixture(true),
		MakeAllKindsFixture(), MakeReferenceFixture(false), MakeReferenceFixture(true)})
	{
		std::vector<std::byte> Main;
		std::vector<std::byte> Bulk;
		ASSERT_TRUE(Package::WritePackageV8(Source, Main, Bulk));
		Package::FLinkerTables Decoded;
		Package::FPackageReaderDiagnostic Diagnostic;
		ASSERT_TRUE(Package::ReadPackageV8(Main, Bulk, Source.Summary.PackageName, Decoded, &Diagnostic))
			<< Diagnostic.LogicalPath << ": " << Diagnostic.Message;
		std::vector<std::byte> RoundTripMain;
		std::vector<std::byte> RoundTripBulk;
		ASSERT_TRUE(Package::WritePackageV8(Decoded, RoundTripMain, RoundTripBulk));
		EXPECT_EQ(RoundTripMain, Main);
		EXPECT_EQ(RoundTripBulk, Bulk);
	}

	Package::FLinkerTables Redirect = MakeFixture();
	Redirect.Summary.bRedirect = true;
	Redirect.Summary.RedirectDestination = "/Game/RedirectTarget";
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(Redirect, Main, Bulk));
	Package::FLinkerTables Decoded;
	ASSERT_TRUE(Package::ReadPackageV8(Main, Bulk, Redirect.Summary.PackageName, Decoded));
	EXPECT_TRUE(Decoded.Summary.bRedirect);
	EXPECT_EQ(Decoded.Summary.RedirectDestination, "/Game/RedirectTarget");
}

TEST(FPackageReaderContractTests, EnvelopeSectionAndBulkFailuresAreAtomic)
{
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(MakeFixture(), Main, Bulk));
	Package::FLinkerTables Sentinel;
	Sentinel.Summary.PackageName = "sentinel";
	Package::FPackageReaderDiagnostic Diagnostic;

	std::vector<std::byte> CorruptHeader = Main;
	CorruptHeader[48] ^= std::byte{1};
	EXPECT_FALSE(Package::ReadPackageV8(CorruptHeader, Bulk, "/Game/WriterFixture", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidEnvelope);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");

	std::vector<std::byte> CorruptSection = Main;
	CorruptSection.back() ^= std::byte{1};
	EXPECT_FALSE(Package::ReadPackageV8(CorruptSection, Bulk, "/Game/WriterFixture", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::HashMismatch);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");

	std::vector<std::byte> CorruptDirectory = Main;
	CorruptDirectory[Package::DastV8DirectoryOffset] = std::byte{2};
	const uint64 HeaderBytes = Read<uint64>(CorruptDirectory, 32);
	ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(
		std::span(CorruptDirectory).first(static_cast<size_t>(HeaderBytes)), CorruptDirectory.size(),
		{Package::DastV8MaximumHeaderBytes, Package::DastV8MaximumPackageBytes}));
	EXPECT_FALSE(Package::ReadPackageV8(CorruptDirectory, Bulk, "/Game/WriterFixture", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidDirectory);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");

	std::vector<std::byte> CorruptBulk = Bulk;
	CorruptBulk.front() ^= std::byte{1};
	EXPECT_FALSE(Package::ReadPackageV8(Main, CorruptBulk, "/Game/WriterFixture", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::HashMismatch);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");
}

TEST(FPackageReaderContractTests, WrongIdentityAndTruncatedFrontMatterDoNotPublishRegistry)
{
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(MakeFixture(), Main, Bulk));
	const uint64 HeaderBytes = Read<uint64>(Main, 32);
	Package::FPackageV8RegistryData Registry{.PackageName = "sentinel"};
	Package::FPackageReaderDiagnostic Diagnostic;
	EXPECT_FALSE(Package::ReadPackageV8Registry(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), Bulk.size(),
		"/Game/Missing", Registry, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidRegistry);
	EXPECT_EQ(Registry.PackageName, "sentinel");
	EXPECT_FALSE(Package::ReadPackageV8Registry(
		std::span(Main).first(static_cast<size_t>(HeaderBytes - 1)), Main.size(), Bulk.size(),
		"/Game/WriterFixture", Registry, &Diagnostic));
	EXPECT_EQ(Registry.PackageName, "sentinel");
}

TEST(FPackageReaderContractTests, LateValueTopologyAndBulkFailuresAreTypedAndAtomic)
{
	Package::FLinkerTables Sentinel;
	Sentinel.Summary.PackageName = "sentinel";
	Package::FPackageReaderDiagnostic Diagnostic;

	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(MakeFixture(), Main, Bulk));
	const uint64 ValuesOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 6 * 48 + 8);
	Main[static_cast<size_t>(ValuesOffset + 11)] = std::byte{1};
	RehashSection(Main, 6);
	EXPECT_FALSE(Package::ReadPackageV8(Main, Bulk, "/Game/WriterFixture", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidValue);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");

	ASSERT_TRUE(Package::WritePackageV8(MakeFixture(), Main, Bulk));
	const uint64 BulkDirectoryOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 7 * 48 + 8);
	Main[static_cast<size_t>(BulkDirectoryOffset + 5)] = std::byte{2};
	RehashSection(Main, 7);
	EXPECT_FALSE(Package::ReadPackageV8(Main, Bulk, "/Game/WriterFixture", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidBulkData);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");

	ASSERT_TRUE(Package::WritePackageV8(MakeReferenceFixture(false), Main, Bulk));
	const uint64 ExportsOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 3 * 48 + 8);
	Main[static_cast<size_t>(ExportsOffset + 10)] = std::byte{4};
	RehashSection(Main, 3);
	EXPECT_FALSE(Package::ReadPackageV8(Main, Bulk, "/Game/References", Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidTopology);
	EXPECT_EQ(Sentinel.Summary.PackageName, "sentinel");
}
