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

	auto Bytes(std::initializer_list<uint8> Values) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Result;
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
			{.Guid = Durin::FGuid{4, 3, 2, 1}, .Version = 7}};

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
		Linker.Schemas = {std::move(Schema), {.QualifiedName = "Example::Pair",
			.Fields = {{.Name = "Number", .Type = {.Kind = Package::EValueKind::I32}},
				{.Name = "Text", .Type = {.Kind = Package::EValueKind::String}}}}};
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
	auto Read(Durin::FByteView Bytes, uint64 Offset) -> T
	{
		T Result = 0;
		EXPECT_TRUE(Durin::ReadLittleEndianAt(Bytes, Offset, Result));
		return Result;
	}

	template<std::unsigned_integral T>
	auto Write(Durin::FMutableByteView Bytes, uint64 Offset, T Value) -> void
	{
		ASSERT_LE(Offset + sizeof(T), Bytes.size());
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[static_cast<size_t>(Offset + Index)] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto RehashSection(Durin::FByteBuffer& Main, uint32 SectionIndex) -> void
	{
		const uint64 DirectoryEntry = Package::DastDirectoryOffset + uint64(SectionIndex) * 48;
		const uint64 Offset = Read<uint64>(Main, DirectoryEntry + 8);
		const uint64 Size = Read<uint64>(Main, DirectoryEntry + 16);
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Main).subspan(static_cast<size_t>(Offset), static_cast<size_t>(Size)));
		Write<uint64>(Main, DirectoryEntry + 24, Hash.HashLow);
		Write<uint64>(Main, DirectoryEntry + 32, Hash.HashHigh);
		const uint64 HeaderBytes = Read<uint64>(Main, 32);
		ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(
			std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(),
			{Package::DastMaximumHeaderBytes, Package::DastMaximumPackageBytes}));
	}
}

TEST(FPackageFormatContractTests, MultipleTopLevelAssetsRoundTripAndProjectExactRegistry)
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

	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	Package::FPackageWriterResult WriterDiagnostic;
	ASSERT_TRUE((WriterDiagnostic = Package::WritePackage(Linker, Main, Bulk)))
		<< Durin::ObjectPackage::FormatPackageError(WriterDiagnostic);
	EXPECT_EQ(Read<uint32>(Main, 24), Package::DastV10FormatVersion);
	const Durin::FXxHash128 FixtureHash = Durin::FXxHash128::HashBuffer(Main);
	EXPECT_EQ(FixtureHash.HashLow, 6855489284108035300ull);
	EXPECT_EQ(FixtureHash.HashHigh, 13282341067721308400ull);
	Package::FPackageRegistryData Registry;
	Package::FPackageReaderResult ReaderDiagnostic;
	ASSERT_TRUE((ReaderDiagnostic = Package::ReadPackageRegistry(
		std::span(Main).first(static_cast<size_t>(Read<uint64>(Main, 32))),
		Main.size(), Bulk.size(), Linker.Summary.PackagePath, Registry))) << Durin::ObjectPackage::FormatPackageError(ReaderDiagnostic);
	ASSERT_EQ(Registry.TopLevelAssets.size(), 2u);
	EXPECT_EQ(Registry.TopLevelAssets[0].AssetPath.ToString(),
		"/Game/WriterFixture.Secondary");
	EXPECT_EQ(Registry.TopLevelAssets[0].RedirectDestination, Redirect);
	EXPECT_EQ(Registry.TopLevelAssets[1].AssetPath.ToString(),
		"/Game/WriterFixture.WriterFixture");

	Package::FLinkerTables Decoded;
	ASSERT_TRUE((ReaderDiagnostic = Package::ReadPackage(Main, Bulk, Linker.Summary.PackagePath,
		Decoded))) << Durin::ObjectPackage::FormatPackageError(ReaderDiagnostic);
	Durin::FByteBuffer ReemittedMain;
	Durin::FByteBuffer ReemittedBulk;
	ASSERT_TRUE((WriterDiagnostic = Package::WritePackage(
		Decoded, ReemittedMain, ReemittedBulk)));
	EXPECT_EQ(ReemittedMain, Main);
	EXPECT_EQ(ReemittedBulk, Bulk);
}

TEST(FPackageWriterContractTests, FrozenLayoutAndFixtureHashAreExact)
{
	static_assert(Package::DastFormatHeaderOffset == 64);
	static_assert(Package::DastDirectoryOffset == 96);
	static_assert(Package::DastFirstSectionOffset == 528);
	static_assert(Package::DastSectionCount == 9);
	static_assert(Package::DastSectionEntryBytes == 48);

	const Package::FLinkerTables Linker = MakeFixture();
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	Package::FPackageWriterResult Diagnostic;
	ASSERT_TRUE((Diagnostic = Package::WritePackage(Linker, Main, Bulk))) << Durin::ObjectPackage::FormatPackageError(Diagnostic);
	ASSERT_GE(Main.size(), Package::DastFirstSectionOffset);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(Main.data()), 4), "DURF");
	EXPECT_EQ(Read<uint32>(Main, 24), Package::DastV10FormatVersion);
	EXPECT_EQ(Read<uint64>(Main, 40), Main.size());
	EXPECT_EQ(Main.size(), 1224u);
	EXPECT_EQ(Read<uint64>(Main, 32), 973u);
	EXPECT_EQ(Read<uint64>(Main, 72), Package::DastDirectoryOffset);
	EXPECT_EQ(Read<uint32>(Main, 80), Package::DastSectionCount);
	EXPECT_EQ(Read<uint32>(Main, 84), Package::DastSectionEntryBytes);
	EXPECT_EQ(Read<uint64>(Main, Package::DastDirectoryOffset + 8), Package::DastFirstSectionOffset);
	constexpr std::array<uint64, Package::DastSectionCount> SectionOffsets{
		528, 570, 968, 973, 981, 1013, 1054, 1110, 1221};
	for (uint64 Index = 0; Index < Package::DastSectionCount; ++Index)
		EXPECT_EQ(Read<uint64>(Main, Package::DastDirectoryOffset + Index * 48 + 8),
			SectionOffsets[Index]) << Index;
	const uint64 ImportOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 2 * 48 + 8);
	const uint64 ImportBytes = Read<uint64>(Main, Package::DastDirectoryOffset + 2 * 48 + 16);
	EXPECT_EQ(Read<uint64>(Main, 32), ImportOffset + ImportBytes);
	EXPECT_EQ(Bulk, Bytes({0xaa, 0xbb, 0xcc, 0xdd}));
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "9e520a5e489ec8527b730cf0b96b1163");
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Bulk).ToString(), "ab65044d6377f7528d403d7d59bb88f3");
}

TEST(FPackageWriterContractTests, EquivalentDiscoveryOrdersProduceIdenticalManifestAndBytes)
{
	const Package::FLinkerTables Canonical = MakeFixture(false);
	const Package::FLinkerTables Shuffled = MakeFixture(true);
	Package::FPackageWriterManifest CanonicalManifest;
	Package::FPackageWriterManifest ShuffledManifest;
	ASSERT_TRUE(Package::FreezePackage(Canonical, CanonicalManifest));
	ASSERT_TRUE(Package::FreezePackage(Shuffled, ShuffledManifest));
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
	Durin::FByteBuffer MainA, BulkA, MainB, BulkB;
	ASSERT_TRUE(Package::WritePackage(Canonical, MainA, BulkA));
	ASSERT_TRUE(Package::WritePackage(Shuffled, MainB, BulkB));
	EXPECT_EQ(MainA, MainB);
	EXPECT_EQ(BulkA, BulkB);
}

TEST(FPackageWriterContractTests, FailuresAreTypedAndAtomic)
{
	Package::FLinkerTables Invalid = MakeFixture();
	Invalid.Exports.front().Properties[1].Value.BulkAlignment = 3;
	Durin::FByteBuffer Main = Bytes({1, 2, 3});
	Durin::FByteBuffer Bulk = Bytes({4, 5});
	const auto OriginalMain = Main;
	const auto OriginalBulk = Bulk;
	Package::FPackageWriterResult Diagnostic;
	EXPECT_FALSE((Diagnostic = Package::WritePackage(Invalid, Main, Bulk)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::InvalidBulkData);
	EXPECT_EQ(Main, OriginalMain);
	EXPECT_EQ(Bulk, OriginalBulk);
	EXPECT_FALSE((Diagnostic = Package::WritePackage(MakeFixture(), Main, Main)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::AliasedOutput);
	EXPECT_EQ(Main, OriginalMain);
}

TEST(FPackageWriterContractTests, EmptyExternalSegmentUsesZeroExtentAndDigest)
{
	Package::FLinkerTables Linker = MakeFixture();
	Linker.Exports.front().Properties[1].Value.BulkStorage = Package::EBulkStorageKind::Inline;
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Package::WritePackage(Linker, Main, Bulk));
	EXPECT_TRUE(Bulk.empty());
	const uint64 RegistryOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 8);
	const uint64 RegistryBytes = Read<uint64>(Main, Package::DastDirectoryOffset + 16);
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
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Package::WritePackage(Linker, Main, Bulk));
	EXPECT_EQ(Read<uint32>(Main, Package::DastFormatHeaderOffset), 0u);
	EXPECT_TRUE(Bulk.empty());
	EXPECT_EQ(Read<uint64>(Main, Package::DastDirectoryOffset + 8 * 48 + 16), 0u);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "bb87b876d085a81fdd6ee423c255e2c0");
}

TEST(FPackageWriterContractTests, EveryNativeValueKindHasOneFrozenFixture)
{
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	Package::FPackageWriterResult Diagnostic;
	ASSERT_TRUE((Diagnostic = Package::WritePackage(MakeAllKindsFixture(), Main, Bulk)))
		<< Diagnostic.LogicalPath << ": " << Durin::ObjectPackage::FormatPackageError(Diagnostic);
	EXPECT_TRUE(Bulk.empty());
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(Main).ToString(), "9cf8e6fd6b584069aceed4e76726c63c");

	Durin::FByteBuffer AlternateNanMain;
	ASSERT_TRUE(Package::WritePackage(MakeAllKindsFixture(0x7fffffffu), AlternateNanMain, Bulk));
	EXPECT_EQ(Main, AlternateNanMain);
}

TEST(FPackageWriterContractTests, MapCollisionsAndInvalidTopologyFailAtomically)
{
	Package::FLinkerTables InvalidMap = MakeFixture();
	auto& Elements = InvalidMap.Exports.front().Properties.back().Value.Elements;
	Elements[2].Text = Elements[0].Text;
	Durin::FByteBuffer Main = Bytes({7});
	Durin::FByteBuffer Bulk = Bytes({8});
	Package::FPackageWriterResult Diagnostic;
	EXPECT_FALSE((Diagnostic = Package::WritePackage(InvalidMap, Main, Bulk)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::DuplicateIdentity);
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));

	Package::FLinkerTables Cyclic = MakeFixture();
	Cyclic.Exports.front().Outer = Cyclic.Summary.TopLevelAssets.front().Export;
	EXPECT_FALSE((Diagnostic = Package::WritePackage(Cyclic, Main, Bulk)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageWriterFailure::InvalidTopology);
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));
}

TEST(FPackageWriterContractTests, ImportExportAndReferenceIdsRemapAcrossShuffledTables)
{
	const Package::FLinkerTables A = MakeReferenceFixture(false);
	const Package::FLinkerTables B = MakeReferenceFixture(true);
	Durin::FByteBuffer MainA, BulkA, MainB, BulkB;
	Package::FPackageWriterResult Diagnostic;
	ASSERT_TRUE((Diagnostic = Package::WritePackage(A, MainA, BulkA))) << Durin::ObjectPackage::FormatPackageError(Diagnostic);
	ASSERT_TRUE((Diagnostic = Package::WritePackage(B, MainB, BulkB))) << Durin::ObjectPackage::FormatPackageError(Diagnostic);
	EXPECT_EQ(MainA, MainB);
	EXPECT_EQ(BulkA, BulkB);
	Package::FPackageWriterManifest Manifest;
	ASSERT_TRUE(Package::FreezePackage(B, Manifest));
	EXPECT_EQ(Manifest.Imports, (std::vector<std::string>{
		"/Game/DepA.DepA", "/Game/DepB.DepB"}));
	EXPECT_EQ(Manifest.Exports, (std::vector<std::string>{"References", "References/Child"}));
}

TEST(FPackageReaderContractTests, RegistryProjectionUsesOnlyDeclaredFrontMatter)
{
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	const uint64 HeaderBytes = Read<uint64>(Main, 32);
	Package::FPackageRegistryData Registry;
	Package::FPackageReaderResult Diagnostic;
	ASSERT_TRUE((Diagnostic = Package::ReadPackageRegistry(std::span(Main).first(static_cast<size_t>(HeaderBytes)),
		Main.size(), Bulk.size(), Fixture.Summary.PackagePath, Registry))) << Durin::ObjectPackage::FormatPackageError(Diagnostic);
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
		Durin::FByteBuffer Main;
		Durin::FByteBuffer Bulk;
		ASSERT_TRUE(Package::WritePackage(Source, Main, Bulk));
		Package::FLinkerTables Decoded;
		Package::FPackageReaderResult Diagnostic;
		ASSERT_TRUE((Diagnostic = Package::ReadPackage(Main, Bulk, Source.Summary.PackagePath, Decoded)))
			<< Diagnostic.LogicalPath << ": " << Durin::ObjectPackage::FormatPackageError(Diagnostic);
		Durin::FByteBuffer RoundTripMain;
		Durin::FByteBuffer RoundTripBulk;
		ASSERT_TRUE(Package::WritePackage(Decoded, RoundTripMain, RoundTripBulk));
		EXPECT_EQ(RoundTripMain, Main);
		EXPECT_EQ(RoundTripBulk, Bulk);
	}

	Package::FLinkerTables Redirect = MakeFixture();
	Redirect.Summary.TopLevelAssets.front().RedirectDestination =
		ObjectPath("/Game/RedirectTarget.RedirectTarget");
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Package::WritePackage(Redirect, Main, Bulk));
	Package::FLinkerTables Decoded;
	ASSERT_TRUE(Package::ReadPackage(Main, Bulk, Redirect.Summary.PackagePath, Decoded));
	EXPECT_EQ(Decoded.Summary.TopLevelAssets.front().RedirectDestination,
		ObjectPath("/Game/RedirectTarget.RedirectTarget"));
}

TEST(FPackageReaderContractTests, EnvelopeSectionAndBulkFailuresAreAtomic)
{
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	Package::FLinkerTables Sentinel;
	Sentinel.Summary.PackagePath = PackagePath("/Game/Sentinel");
	Package::FPackageReaderResult Diagnostic;

	Durin::FByteBuffer CorruptHeader = Main;
	CorruptHeader[48] ^= std::byte{1};
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(CorruptHeader, Bulk, Fixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidEnvelope);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	Durin::FByteBuffer CorruptSection = Main;
	CorruptSection.back() ^= std::byte{1};
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(CorruptSection, Bulk, Fixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::HashMismatch);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	Durin::FByteBuffer CorruptDirectory = Main;
	CorruptDirectory[Package::DastDirectoryOffset] = std::byte{2};
	const uint64 HeaderBytes = Read<uint64>(CorruptDirectory, 32);
	ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(
		std::span(CorruptDirectory).first(static_cast<size_t>(HeaderBytes)), CorruptDirectory.size(),
		{Package::DastMaximumHeaderBytes, Package::DastMaximumPackageBytes}));
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(CorruptDirectory, Bulk, Fixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidDirectory);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	Durin::FByteBuffer CorruptBulk = Bulk;
	CorruptBulk.front() ^= std::byte{1};
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(Main, CorruptBulk, Fixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::HashMismatch);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");
}

TEST(FPackageReaderContractTests, WrongIdentityAndTruncatedFrontMatterDoNotPublishRegistry)
{
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	const uint64 HeaderBytes = Read<uint64>(Main, 32);
	Package::FPackageRegistryData Registry{.PackagePath = PackagePath("/Game/Sentinel")};
	Package::FPackageReaderResult Diagnostic;
	EXPECT_FALSE((Diagnostic = Package::ReadPackageRegistry(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), Bulk.size(),
		PackagePath("/Game/Missing"), Registry)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidRegistry);
	EXPECT_EQ(Registry.PackagePath.ToString(), "/Game/Sentinel");
	EXPECT_FALSE((Diagnostic = Package::ReadPackageRegistry(
		std::span(Main).first(static_cast<size_t>(HeaderBytes - 1)), Main.size(), Bulk.size(),
		Fixture.Summary.PackagePath, Registry)));
	EXPECT_EQ(Registry.PackagePath.ToString(), "/Game/Sentinel");
}

TEST(FPackageReaderContractTests, RetiredAndFutureVersionsFailWithoutPublishing)
{
	for (uint32 Version : {9u, 11u})
	{
		auto Fixture = MakeFixture();
		Durin::FByteBuffer Main, Bulk;
		ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
		Write<uint32>(Main, 24, Version);
		const auto HeaderBytes = Read<uint64>(Main, 32);
		ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(
			std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(),
			{Package::DastMaximumHeaderBytes, Package::DastMaximumPackageBytes}));
		Package::FLinkerTables Sentinel;
		Sentinel.Summary.PackagePath = PackagePath("/Game/Sentinel");
		Package::FPackageReaderResult Diagnostic;
		EXPECT_FALSE((Diagnostic = Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Sentinel)));
		EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidEnvelope);
		EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");
		Package::FPackageRegistryData Registry;
		EXPECT_FALSE(Package::ReadPackageRegistry(std::span(Main).first(static_cast<size_t>(HeaderBytes)),
			Main.size(), Bulk.size(), Fixture.Summary.PackagePath, Registry));
		Fixture.FormatVersion = Version;
		const auto Original = Main;
		EXPECT_FALSE(Package::WritePackage(Fixture, Main, Bulk));
		EXPECT_EQ(Main, Original);
	}
}

TEST(FPackageReaderContractTests, LateValueTopologyAndBulkFailuresAreTypedAndAtomic)
{
	Package::FLinkerTables Sentinel;
	Sentinel.Summary.PackagePath = PackagePath("/Game/Sentinel");
	Package::FPackageReaderResult Diagnostic;

	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	const Package::FLinkerTables Fixture = MakeFixture();
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	const uint64 ValuesOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 6 * 48 + 8);
	// The export baseline flag follows the table version, export count and id.
	Main[static_cast<size_t>(ValuesOffset + 6)] = std::byte{2};
	RehashSection(Main, 6);
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidValue);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	const uint64 BulkDirectoryOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 7 * 48 + 8);
	Main[static_cast<size_t>(BulkDirectoryOffset + 5)] = std::byte{2};
	RehashSection(Main, 7);
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidBulkData);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");

	const Package::FLinkerTables ReferenceFixture = MakeReferenceFixture(false);
	ASSERT_TRUE(Package::WritePackage(ReferenceFixture, Main, Bulk));
	const uint64 ExportsOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 3 * 48 + 8);
	Main[static_cast<size_t>(ExportsOffset + 10)] = std::byte{4};
	RehashSection(Main, 3);
	EXPECT_FALSE((Diagnostic = Package::ReadPackage(Main, Bulk, ReferenceFixture.Summary.PackagePath, Sentinel)));
	EXPECT_EQ(Diagnostic.Failure, Package::EPackageReaderFailure::InvalidTopology);
	EXPECT_EQ(Sentinel.Summary.PackagePath.ToString(), "/Game/Sentinel");
}

TEST(FPackageReaderContractTests, CustomVersionRecordsRejectInvalidFactsAndRetiredCapabilityFlags)
{
	auto Fixture = MakeFixture();
	Durin::FByteBuffer Main, Bulk;
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	const uint64 SchemaOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 5 * 48 + 8);
	// One record follows the table version and one-byte count: GUID, version, flags.
	EXPECT_EQ(Main[SchemaOffset + 25], std::byte{0});
	Main[SchemaOffset + 25] = std::byte{12};
	RehashSection(Main, 5);
	Package::FLinkerTables Loaded;
	EXPECT_FALSE(Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Loaded));
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	ASSERT_TRUE(Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Loaded));
	EXPECT_EQ(Loaded.CustomVersions, Fixture.CustomVersions);
	Write<uint32>(Main, SchemaOffset + 21, 0xffffffffu);
	RehashSection(Main, 5);
	EXPECT_FALSE(Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Loaded));
	Fixture.CustomVersions.front().Version = -1;
	EXPECT_FALSE(Package::WritePackage(Fixture, Main, Bulk));
	Fixture.CustomVersions.front().Version = 7;
	Fixture.CustomVersions.push_back(Fixture.CustomVersions.front());
	EXPECT_FALSE(Package::WritePackage(Fixture, Main, Bulk));
}

TEST(FPackageWriterContractTests, WriterFailuresPreserveContextAndOutputs)
{
	Durin::FByteBuffer Main = Bytes({7}), Bulk = Bytes({8});
	auto Fixture = MakeFixture();
	Package::FPackageIndex Child;
	ASSERT_TRUE(Package::FPackageIndex::TryExport(1, Child));
	Fixture.Exports.push_back({.ObjectName = "Child", .ClassName = "Example::Child", .Outer = Child});
	const auto Topology = Package::WritePackage(Fixture, Main, Bulk);
	ASSERT_FALSE(Topology);
	EXPECT_EQ(Topology.Reason, Package::EPackageWriterReason::UnresolvedTablePath);
	EXPECT_EQ(Topology.LogicalPath, "Exports[1]");
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));

	Fixture = MakeFixture();
	auto& Labels = Fixture.Exports.front().Properties.back();
	Labels.Type.Children[0].Kind = Package::EValueKind::I8;
	Fixture.Schemas.front().Fields.back().Type = Labels.Type;
	Labels.Value.Elements[0].Signed = 128;
	const auto Key = Package::WritePackage(Fixture, Main, Bulk);
	ASSERT_FALSE(Key);
	EXPECT_EQ(Key.Reason, Package::EPackageWriterReason::CanonicalKeyRejected);
	EXPECT_EQ(Key.LogicalPath, "WriterFixture.Example::WriterAsset.Labels");
	Fixture = {};
	EXPECT_EQ(Main, Bytes({7}));
	EXPECT_EQ(Bulk, Bytes({8}));
}

TEST(FPackageReaderContractTests, CanonicalValidationPreservesOutput)
{
	auto Fixture = MakeFixture();
	Fixture.Exports.front().Properties.resize(1);
	Fixture.Schemas.front().Fields.resize(1);
	Fixture.Exports.front().Properties.front().Type.Kind = Package::EValueKind::I8;
	Fixture.Schemas.front().Fields.front().Type.Kind = Package::EValueKind::I8;
	Fixture.Exports.front().Properties.front().Value.Signed = 64;
	Durin::FByteBuffer Main, Bulk;
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	const auto ValuesOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 6 * 48 + 8);
	// One export and one scalar field: header, identities, provenance, tag, zigzag value.
	ASSERT_EQ(Main[ValuesOffset + 12], std::byte(uint8(Package::EValueKind::I8) + 1));
	ASSERT_EQ(Main[ValuesOffset + 13], std::byte{0x80});
	ASSERT_EQ(Main[ValuesOffset + 14], std::byte{1});
	Main[ValuesOffset + 14] = std::byte{2}; // 128 is decodable but outside I8 storage.
	RehashSection(Main, 6);
	Package::FLinkerTables Output;
	Output.Summary.PackagePath = PackagePath("/Game/Sentinel");
	const auto Result = Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Output);
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.Reason, Package::EPackageReaderReason::CanonicalWriterRejected);
	EXPECT_EQ(Output.Summary.PackagePath.ToString(), "/Game/Sentinel");
	Main.clear();
}

TEST(FPackageReaderContractTests, EnvelopeAndNestedValueReasonsSurvivePropagation)
{
	auto Fixture = MakeFixture();
	Durin::FByteBuffer Main, Bulk;
	ASSERT_TRUE(Package::WritePackage(Fixture, Main, Bulk));
	Package::FLinkerTables Output;
	Output.Summary.PackagePath = PackagePath("/Game/Sentinel");
	auto Corrupt = Main;
	Corrupt[48] ^= std::byte{1};
	const auto Envelope = Package::ReadPackage(Corrupt, Bulk, Fixture.Summary.PackagePath, Output);
	ASSERT_FALSE(Envelope);
	EXPECT_EQ(Envelope.Reason, Package::EPackageReaderReason::EnvelopeRejected);
	EXPECT_EQ(Output.Summary.PackagePath.ToString(), "/Game/Sentinel");

	const auto ValuesOffset = Read<uint64>(Main, Package::DastDirectoryOffset + 6 * 48 + 8);
	Main[ValuesOffset + 12] = std::byte{0};
	RehashSection(Main, 6);
	const auto Value = Package::ReadPackage(Main, Bulk, Fixture.Summary.PackagePath, Output);
	ASSERT_FALSE(Value);
	EXPECT_EQ(Value.Reason, Package::EPackageReaderReason::ValueTag);
	EXPECT_EQ(Value.LogicalPath, "WriterFixture.Example::WriterAsset.Count");
	EXPECT_EQ(Output.Summary.PackagePath.ToString(), "/Game/Sentinel");
}

TEST(FPackageFormatContractTests, ResultFormattingAndSuccessHaveNoRetainedError)
{
	Durin::FByteBuffer Main, Bulk;
	auto Result = Package::WritePackage(MakeFixture(), Main, Main);
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.Reason, Package::EPackageWriterReason::AliasedOutput);
	Result = Package::WritePackage(MakeFixture(), Main, Bulk);
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Reason, Package::EPackageWriterReason::None);
	EXPECT_TRUE(Package::FormatPackageError(Result).empty());
}
