#include "DObject/PackageFormat.h"
#include "Hash/XxHash.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	namespace Package = Durin::ObjectPackage;

	auto EnsurePathMount() -> void
	{
		static const std::array Definitions{Durin::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::EMountOwner::Test,
			.Root = std::filesystem::current_path(),
		}};
		static Durin::Testing::FScopedMountRegistryFixture Fixture(Definitions);
		ASSERT_TRUE(Fixture.IsValid()) << Fixture.GetError();
	}

	auto Bytes(std::initializer_list<uint8> Values) -> Durin::FByteArray
	{
		Durin::FByteArray Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto ObjectPath(std::string_view Value) -> Durin::FObjectPath
	{
		EnsurePathMount();
		Durin::FObjectPath Result;
		EXPECT_TRUE(Durin::FObjectPath::TryCreate(Value, Result));
		return Result;
	}

	auto PackagePath(std::string_view Value) -> Durin::FPackagePath
	{
		EnsurePathMount();
		Durin::FPackagePath Result;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Result));
		return Result;
	}

	auto MakeFixture(bool bShuffled = false) -> Package::FLinkerTables
	{
		EnsurePathMount();
		const Package::FSerializedType I32{.Kind = Package::EValueKind::I32};
		const Package::FSerializedType String{.Kind = Package::EValueKind::String};
		const Package::FSerializedType U32{.Kind = Package::EValueKind::U32};
		const Package::FSerializedType Map{
			.Kind = Package::EValueKind::Map, .Children = {String, U32}};
		const Package::FSerializedType Bulk{.Kind = Package::EValueKind::BulkData};

		Package::FLinkerTables Linker;
		Linker.Summary.SearchableNames = {"SearchB", "SearchA"};
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
		Durin::FPackagePath PackagePath;
		Durin::FTopLevelAssetPath AssetPath;
		Package::FPackageIndex Root;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate("/Game/WriterFixture", PackagePath));
		EXPECT_TRUE(Durin::FTopLevelAssetPath::TryCreate(PackagePath, "WriterFixture", AssetPath));
		EXPECT_TRUE(Package::FPackageIndex::TryExport(0, Root));
		Linker.Summary.PackagePath = PackagePath;
		Linker.Summary.TopLevelAssets = {{Root, AssetPath, "Example::WriterAsset"}};
		for (std::string_view Value : {"/Game/HardB", "/Game/HardA"})
		{
			Durin::FPackagePath Dependency;
			EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Dependency));
			Linker.Summary.HardPackageDependencies.push_back(Dependency);
		}
		Durin::FPackagePath SoftDependency;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate("/Game/SoftA", SoftDependency));
		Linker.Summary.SoftPackageDependencies = {SoftDependency};
		return Linker;
	}

	auto MakeAllKindsFixture(uint32 FloatNanBits = 0x7fc00001u) -> Package::FLinkerTables
	{
		EnsurePathMount();
		Package::FLinkerTables Linker;
		Package::FPackageIndex Self;
		Package::FPackageIndex::TryExport(0, Self);
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
		Durin::FPackagePath PackagePath;
		Durin::FTopLevelAssetPath AssetPath;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate("/Game/AllKinds", PackagePath));
		EXPECT_TRUE(Durin::FTopLevelAssetPath::TryCreate(PackagePath, "AllKinds", AssetPath));
		Linker.Summary.PackagePath = PackagePath;
		Linker.Summary.TopLevelAssets = {{Self, AssetPath, "Example::AllKinds"}};
		return Linker;
	}

	auto MakeReferenceFixture(bool bShuffled) -> Package::FLinkerTables
	{
		EnsurePathMount();
		const Package::FSerializedType Hard{.Kind = Package::EValueKind::HardReference};
		Package::FLinkerTables Linker;
		Durin::FObjectPath DepA;
		Durin::FObjectPath DepB;
		EXPECT_TRUE(Durin::FObjectPath::TryCreate("/Game/DepA.DepA", DepA));
		EXPECT_TRUE(Durin::FObjectPath::TryCreate("/Game/DepB.DepB", DepB));
		Linker.Schemas = {{.QualifiedName = "Example::References", .Fields = {{.Name = "Target", .Type = Hard}}}};
		Linker.Imports = bShuffled
			? std::vector<Package::FPackageImport>{{.ObjectPath = DepB}, {.ObjectPath = DepA}}
			: std::vector<Package::FPackageImport>{{.ObjectPath = DepA}, {.ObjectPath = DepB}};
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
		Durin::FPackagePath PackagePath;
		Durin::FTopLevelAssetPath AssetPath;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate("/Game/References", PackagePath));
		EXPECT_TRUE(Durin::FTopLevelAssetPath::TryCreate(PackagePath, "References", AssetPath));
		Linker.Summary.PackagePath = PackagePath;
		Linker.Summary.TopLevelAssets = {{Root, AssetPath, "Example::References"}};
		Linker.Summary.HardPackageDependencies = {DepB.GetPackagePath(), DepA.GetPackagePath()};
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

	auto RehashSection(Durin::FByteArray& Main, uint32 SectionIndex) -> void
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

TEST(FPackageV9ContractTests, MultipleTopLevelAssetsRoundTripAndProjectExactRegistry)
{
	EnsurePathMount();
	Package::FLinkerTables Linker = MakeFixture();
	Linker.Exports.push_back({.ObjectName = "Secondary", .ClassName = "Example::Secondary"});
	Package::FPackageIndex SecondaryExport;
	Durin::FTopLevelAssetPath SecondaryPath;
	ASSERT_TRUE(Package::FPackageIndex::TryExport(1, SecondaryExport));
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
		Linker.Summary.PackagePath, "Secondary", SecondaryPath));
	Linker.Summary.TopLevelAssets.push_back(
		{SecondaryExport, SecondaryPath, "Example::Secondary"});
	Durin::FObjectPath Redirect;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		"/Game/Target.Target:Subobject", Redirect));
	Linker.Summary.TopLevelAssets[1].RedirectDestination = Redirect;

	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	Package::FPackageWriterDiagnostic WriterDiagnostic;
	ASSERT_TRUE(Package::WritePackageV9(Linker, Main, Bulk, &WriterDiagnostic))
		<< WriterDiagnostic.Message;
	EXPECT_EQ(Read<uint32>(Main, 24), Package::DastV9FormatVersion);
	const Durin::FXxHash128 FixtureHash = Durin::FXxHash128::HashBuffer(Main);
	EXPECT_EQ(FixtureHash.HashLow, 6770264161647476482ull);
	EXPECT_EQ(FixtureHash.HashHigh, 2895173423952572740ull);
	Package::FPackageV9RegistryData Registry;
	Package::FPackageReaderDiagnostic ReaderDiagnostic;
	ASSERT_TRUE(Package::ReadPackageV9Registry(
		std::span(Main).first(static_cast<size_t>(Read<uint64>(Main, 32))),
		Main.size(), Bulk.size(), Linker.Summary.PackagePath, Registry,
		&ReaderDiagnostic)) << ReaderDiagnostic.Message;
	ASSERT_EQ(Registry.TopLevelAssets.size(), 2u);
	EXPECT_EQ(Registry.TopLevelAssets[0].AssetPath.ToString(),
		"/Game/WriterFixture.Secondary");
	EXPECT_EQ(Registry.TopLevelAssets[0].RedirectDestination, Redirect);
	EXPECT_EQ(Registry.TopLevelAssets[1].AssetPath.ToString(),
		"/Game/WriterFixture.WriterFixture");

	Package::FLinkerTables Decoded;
	ASSERT_TRUE(Package::ReadPackageV9(Main, Bulk, Linker.Summary.PackagePath,
		Decoded, &ReaderDiagnostic)) << ReaderDiagnostic.Message;
	Durin::FByteArray ReemittedMain;
	Durin::FByteArray ReemittedBulk;
	ASSERT_TRUE(Package::WritePackageV9(
		Decoded, ReemittedMain, ReemittedBulk, &WriterDiagnostic));
	EXPECT_EQ(ReemittedMain, Main);
	EXPECT_EQ(ReemittedBulk, Bulk);
}

TEST(FPackageWriterContractTests, FrozenLayoutAndFixtureHashAreExact)
{
	static_assert(Package::DastV8FormatHeaderOffset == 64);
	static_assert(Package::DastV8DirectoryOffset == 96);
	static_assert(Package::DastV8FirstSectionOffset == 528);
	static_assert(Package::DastV8SectionCount == 9);
	static_assert(Package::DastV8SectionEntryBytes == 48);

	const Package::FLinkerTables Linker = MakeFixture();
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	Package::FPackageWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Package::WritePackageV9(Linker, Main, Bulk, &Diagnostic)) << Diagnostic.Message;
	ASSERT_GE(Main.size(), Package::DastV8FirstSectionOffset);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Main.data()), 4), "DURF");
	EXPECT_EQ(Read<uint32>(Main, 24), Package::DastV9FormatVersion);
	EXPECT_EQ(Read<uint64>(Main, 40), Main.size());
	EXPECT_EQ(Main.size(), 1231u);
	EXPECT_EQ(Read<uint64>(Main, 32), 973u);
	EXPECT_EQ(Read<uint64>(Main, 72), Package::DastV8DirectoryOffset);
	EXPECT_EQ(Read<uint32>(Main, 80), Package::DastV8SectionCount);
	EXPECT_EQ(Read<uint32>(Main, 84), Package::DastV8SectionEntryBytes);
	EXPECT_EQ(Read<uint64>(Main, Package::DastV8DirectoryOffset + 8), Package::DastV8FirstSectionOffset);
	constexpr std::array<uint64, Package::DastV8SectionCount> SectionOffsets{
		528, 570, 968, 973, 981, 1013, 1062, 1117, 1228};
	for (uint64 Index = 0; Index < Package::DastV8SectionCount; ++Index)
		EXPECT_EQ(Read<uint64>(Main, Package::DastV8DirectoryOffset + Index * 48 + 8),
			SectionOffsets[Index]) << Index;
	const uint64 ImportOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 2 * 48 + 8);
	const uint64 ImportBytes = Read<uint64>(Main, Package::DastV8DirectoryOffset + 2 * 48 + 16);
	EXPECT_EQ(Read<uint64>(Main, 32), ImportOffset + ImportBytes);
	EXPECT_EQ(Bulk, Bytes({0xaa, 0xbb, 0xcc, 0xdd}));
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "1abb402441db1788483405ef62d652fa");
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bulk).ToString(), "ab65044d6377f7528d403d7d59bb88f3");
}

TEST(FPackageWriterContractTests, EquivalentDiscoveryOrdersProduceIdenticalManifestAndBytes)
{
	const Package::FLinkerTables Canonical = MakeFixture(false);
	const Package::FLinkerTables Shuffled = MakeFixture(true);
	Package::FPackageWriterManifest CanonicalManifest;
	Package::FPackageWriterManifest ShuffledManifest;
	ASSERT_TRUE(Package::FreezePackageV9(Canonical, CanonicalManifest));
	ASSERT_TRUE(Package::FreezePackageV9(Shuffled, ShuffledManifest));
	EXPECT_EQ(CanonicalManifest, ShuffledManifest);
	EXPECT_EQ(CanonicalManifest.Names, (std::vector<std::string>{
		"/Game/HardA", "/Game/HardB", "/Game/SoftA", "/Game/WriterFixture",
		"/Game/WriterFixture.WriterFixture", "Count",
		"Example::WriterAsset", "External", "Inline", "Labels", "SearchA", "SearchB",
		"WriterFixture", "WriterFixture.Example::WriterAsset.External",
		"WriterFixture.Example::WriterAsset.Inline", "unused-a", "unused-z"}));
	EXPECT_EQ(CanonicalManifest.Schemas, (std::vector<std::string>{"Example::WriterAsset"}));
	EXPECT_TRUE(CanonicalManifest.Imports.empty());
	EXPECT_EQ(CanonicalManifest.Exports, (std::vector<std::string>{"WriterFixture"}));
	EXPECT_EQ(CanonicalManifest.BulkValues, (std::vector<std::string>{
		"WriterFixture.Example::WriterAsset.External",
		"WriterFixture.Example::WriterAsset.Inline"}));
	Durin::FByteArray MainA, BulkA, MainB, BulkB;
	ASSERT_TRUE(Package::WritePackageV9(Canonical, MainA, BulkA));
	ASSERT_TRUE(Package::WritePackageV9(Shuffled, MainB, BulkB));
	EXPECT_EQ(MainA, MainB);
	EXPECT_EQ(BulkA, BulkB);
}

TEST(FPackageWriterContractTests, FailuresAreTypedAndAtomic)
{
	Package::FLinkerTables Invalid = MakeFixture();
	Invalid.Exports.front().Properties[1].Value.BulkAlignment = 3;
	Durin::FByteArray Main = Bytes({1, 2, 3});
	Durin::FByteArray Bulk = Bytes({4, 5});
	const auto OriginalMain = Main;
	const auto OriginalBulk = Bulk;
	Package::FPackageWriterDiagnostic Diagnostic;
	EXPECT_FALSE(Package::WritePackageV9(Invalid, Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::InvalidBulkData);
	EXPECT_EQ(Main, OriginalMain);
	EXPECT_EQ(Bulk, OriginalBulk);
	EXPECT_FALSE(Package::WritePackageV9(MakeFixture(), Main, Main, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::AliasedOutput);
	EXPECT_EQ(Main, OriginalMain);
}

TEST(FPackageWriterContractTests, EmptyExternalSegmentUsesZeroExtentAndDigest)
{
	Package::FLinkerTables Linker = MakeFixture();
	Linker.Exports.front().Properties[1].Value.BulkStorage = Package::EBulkStorageKind::Inline;
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	ASSERT_TRUE(Package::WritePackageV9(Linker, Main, Bulk));
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
	Linker.Summary.TopLevelAssets.front().RedirectDestination =
		ObjectPath("/Game/RedirectTarget.RedirectTarget");
	std::erase_if(Linker.Schemas.front().Fields, [](const Package::FSerializedField& Field)
	{ return Field.Type.Kind == Package::EValueKind::BulkData; });
	std::erase_if(Linker.Exports.front().Properties, [](const Package::FPropertyTag& Property)
	{ return Property.Type.Kind == Package::EValueKind::BulkData; });
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	ASSERT_TRUE(Package::WritePackageV9(Linker, Main, Bulk));
	EXPECT_EQ(Read<uint32>(Main, Package::DastV8FormatHeaderOffset), 0u);
	EXPECT_TRUE(Bulk.empty());
	EXPECT_EQ(Read<uint64>(Main, Package::DastV8DirectoryOffset + 8 * 48 + 16), 0u);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "83afce7c14dd6218a79a7e85aa117106");
}

TEST(FPackageWriterContractTests, EveryNativeValueKindHasOneFrozenFixture)
{
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	Package::FPackageWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Package::WritePackageV9(MakeAllKindsFixture(), Main, Bulk, &Diagnostic))
		<< Diagnostic.LogicalPath << ": " << Diagnostic.Message;
	EXPECT_TRUE(Bulk.empty());
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "bc8b14a7c868e13a6f1e28c816cf6d2d");

	Durin::FByteArray AlternateNanMain;
	ASSERT_TRUE(Package::WritePackageV9(MakeAllKindsFixture(0x7fffffffu), AlternateNanMain, Bulk));
	EXPECT_EQ(Main, AlternateNanMain);
}

TEST(FPackageWriterContractTests, MapCollisionsAndInvalidTopologyFailAtomically)
{
	Package::FLinkerTables InvalidMap = MakeFixture();
	auto& Elements = InvalidMap.Exports.front().Properties.back().Value.Elements;
	Elements[2].Text = Elements[0].Text;
	Durin::FByteArray Main = Bytes({7});
	Durin::FByteArray Bulk = Bytes({8});
	Package::FPackageWriterDiagnostic Diagnostic;
	EXPECT_FALSE(Package::WritePackageV9(InvalidMap, Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::DuplicateIdentity);
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));

	Package::FLinkerTables Cyclic = MakeFixture();
	Cyclic.Exports.front().Outer = Cyclic.Summary.TopLevelAssets.front().Export;
	EXPECT_FALSE(Package::WritePackageV9(Cyclic, Main, Bulk, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::InvalidTopology);
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));
}

TEST(FPackageWriterContractTests, ImportExportAndReferenceIdsRemapAcrossShuffledTables)
{
	const Package::FLinkerTables A = MakeReferenceFixture(false);
	const Package::FLinkerTables B = MakeReferenceFixture(true);
	Durin::FByteArray MainA, BulkA, MainB, BulkB;
	Package::FPackageWriterDiagnostic Diagnostic;
	ASSERT_TRUE(Package::WritePackageV9(A, MainA, BulkA, &Diagnostic)) << Diagnostic.Message;
	ASSERT_TRUE(Package::WritePackageV9(B, MainB, BulkB, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(MainA, MainB);
	EXPECT_EQ(BulkA, BulkB);
	Package::FPackageWriterManifest Manifest;
	ASSERT_TRUE(Package::FreezePackageV9(B, Manifest));
	EXPECT_EQ(Manifest.Imports, (std::vector<std::string>{
		"/Game/DepA.DepA", "/Game/DepB.DepB"}));
	EXPECT_EQ(Manifest.Exports, (std::vector<std::string>{"References", "References/Child"}));
}

TEST(FPackageReaderContractTests, RegistryProjectionUsesOnlyDeclaredFrontMatter)
{
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackageV9(Fixture, Main, Bulk));
	const uint64 HeaderBytes = Read<uint64>(Main, 32);
	Package::FPackageV9RegistryData Registry;
	Package::FPackageReaderDiagnostic Diagnostic;
	ASSERT_TRUE(Package::ReadPackageV9Registry(std::span(Main).first(static_cast<size_t>(HeaderBytes)),
		Main.size(), Bulk.size(), Fixture.Summary.PackagePath, Registry, &Diagnostic)) << Diagnostic.Message;
	EXPECT_EQ(Registry.PackagePath, Fixture.Summary.PackagePath);
	ASSERT_EQ(Registry.TopLevelAssets.size(), 1u);
	EXPECT_EQ(Registry.TopLevelAssets.front().ClassName, "Example::WriterAsset");
	EXPECT_EQ(Registry.TopLevelAssets.front().ExportId, 1u);
	EXPECT_EQ(Registry.ExportCount, 1u);
	ASSERT_EQ(Registry.HardPackageReferences.size(), 2u);
	EXPECT_EQ(Registry.HardPackageReferences[0].ToString(), "/Game/HardA");
	EXPECT_EQ(Registry.HardPackageReferences[1].ToString(), "/Game/HardB");
	EXPECT_EQ(Registry.SoftPackageReferences, Fixture.Summary.SoftPackageDependencies);
	EXPECT_EQ(Registry.SearchableNames, (std::vector<std::string>{"SearchA", "SearchB"}));
	EXPECT_EQ(Registry.ExternalBulkBytes, Bulk.size());
}

TEST(FPackageReaderContractTests, CanonicalFixturesReadAndWriteByteIdentically)
{
	for (const Package::FLinkerTables& Source : {MakeFixture(), MakeFixture(true),
		MakeAllKindsFixture(), MakeReferenceFixture(false), MakeReferenceFixture(true)})
	{
		Durin::FByteArray Main;
		Durin::FByteArray Bulk;
		ASSERT_TRUE(Package::WritePackageV9(Source, Main, Bulk));
		Package::FLinkerTables Decoded;
		Package::FPackageReaderDiagnostic Diagnostic;
		ASSERT_TRUE(Package::ReadPackageV9(Main, Bulk, Source.Summary.PackagePath, Decoded, &Diagnostic))
			<< Diagnostic.LogicalPath << ": " << Diagnostic.Message;
		Durin::FByteArray RoundTripMain;
		Durin::FByteArray RoundTripBulk;
		ASSERT_TRUE(Package::WritePackageV9(Decoded, RoundTripMain, RoundTripBulk));
		EXPECT_EQ(RoundTripMain, Main);
		EXPECT_EQ(RoundTripBulk, Bulk);
	}

	Package::FLinkerTables Redirect = MakeFixture();
	Redirect.Summary.TopLevelAssets.front().RedirectDestination =
		ObjectPath("/Game/RedirectTarget.RedirectTarget");
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	ASSERT_TRUE(Package::WritePackageV9(Redirect, Main, Bulk));
	Package::FLinkerTables Decoded;
	ASSERT_TRUE(Package::ReadPackageV9(Main, Bulk, Redirect.Summary.PackagePath, Decoded));
	EXPECT_EQ(Decoded.Summary.TopLevelAssets.front().RedirectDestination,
		ObjectPath("/Game/RedirectTarget.RedirectTarget"));
}

TEST(FPackageReaderContractTests, EnvelopeSectionAndBulkFailuresAreAtomic)
{
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackageV9(Fixture, Main, Bulk));
	Package::FLinkerTables Sentinel;
	Sentinel.Summary.PackagePath = PackagePath("/Game/Sentinel");
	Package::FPackageReaderDiagnostic Diagnostic;

	Durin::FByteArray CorruptHeader = Main;
	CorruptHeader[48] ^= std::byte{1};
	EXPECT_FALSE(Package::ReadPackageV9(CorruptHeader, Bulk, Fixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidEnvelope);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	Durin::FByteArray CorruptSection = Main;
	CorruptSection.back() ^= std::byte{1};
	EXPECT_FALSE(Package::ReadPackageV9(CorruptSection, Bulk, Fixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::HashMismatch);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	Durin::FByteArray CorruptDirectory = Main;
	CorruptDirectory[Package::DastV8DirectoryOffset] = std::byte{2};
	const uint64 HeaderBytes = Read<uint64>(CorruptDirectory, 32);
	ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(
		std::span(CorruptDirectory).first(static_cast<size_t>(HeaderBytes)), CorruptDirectory.size(),
		{Package::DastV8MaximumHeaderBytes, Package::DastV8MaximumPackageBytes}));
	EXPECT_FALSE(Package::ReadPackageV9(CorruptDirectory, Bulk, Fixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidDirectory);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	Durin::FByteArray CorruptBulk = Bulk;
	CorruptBulk.front() ^= std::byte{1};
	EXPECT_FALSE(Package::ReadPackageV9(Main, CorruptBulk, Fixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::HashMismatch);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");
}

TEST(FPackageReaderContractTests, WrongIdentityAndTruncatedFrontMatterDoNotPublishRegistry)
{
	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackageV9(Fixture, Main, Bulk));
	const uint64 HeaderBytes = Read<uint64>(Main, 32);
	Package::FPackageV9RegistryData Registry{.PackagePath = PackagePath("/Game/Sentinel")};
	Package::FPackageReaderDiagnostic Diagnostic;
	EXPECT_FALSE(Package::ReadPackageV9Registry(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), Bulk.size(),
		PackagePath("/Game/Missing"), Registry, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidRegistry);
	EXPECT_EQ(Registry.PackagePath.ToString(), "/Game/Sentinel");
	EXPECT_FALSE(Package::ReadPackageV9Registry(
		std::span(Main).first(static_cast<size_t>(HeaderBytes - 1)), Main.size(), Bulk.size(),
		Fixture.Summary.PackagePath, Registry, &Diagnostic));
	EXPECT_EQ(Registry.PackagePath.ToString(), "/Game/Sentinel");
}

TEST(FPackageReaderContractTests, LateValueTopologyAndBulkFailuresAreTypedAndAtomic)
{
	Package::FLinkerTables Sentinel;
	Sentinel.Summary.PackagePath = PackagePath("/Game/Sentinel");
	Package::FPackageReaderDiagnostic Diagnostic;

	Durin::FByteArray Main;
	Durin::FByteArray Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackageV9(Fixture, Main, Bulk));
	const uint64 ValuesOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 6 * 48 + 8);
	Main[static_cast<size_t>(ValuesOffset + 11)] = std::byte{1};
	RehashSection(Main, 6);
	EXPECT_FALSE(Package::ReadPackageV9(Main, Bulk, Fixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidValue);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	ASSERT_TRUE(Package::WritePackageV9(Fixture, Main, Bulk));
	const uint64 BulkDirectoryOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 7 * 48 + 8);
	Main[static_cast<size_t>(BulkDirectoryOffset + 5)] = std::byte{2};
	RehashSection(Main, 7);
	EXPECT_FALSE(Package::ReadPackageV9(Main, Bulk, Fixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidBulkData);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	const Package::FLinkerTables ReferenceFixture = MakeReferenceFixture(false);
	ASSERT_TRUE(Package::WritePackageV9(ReferenceFixture, Main, Bulk));
	const uint64 ExportsOffset = Read<uint64>(Main, Package::DastV8DirectoryOffset + 3 * 48 + 8);
	Main[static_cast<size_t>(ExportsOffset + 10)] = std::byte{4};
	RehashSection(Main, 3);
	EXPECT_FALSE(Package::ReadPackageV9(Main, Bulk, ReferenceFixture.Summary.PackagePath, Sentinel, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidTopology);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");
}
