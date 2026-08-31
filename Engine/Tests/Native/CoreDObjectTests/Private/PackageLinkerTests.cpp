#include "DObject/CanonicalMapKey.h"
#include "DObject/AssetPath.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/PackageLinker.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"

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

	auto Token(Package::EValueKind Kind, Package::FSerializedValue Value)
		-> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		std::string Error;
		EXPECT_TRUE(Package::BuildCanonicalMapKeyToken({.Kind = Kind}, Value, Result, &Error)) << Error;
		return Result;
	}

	auto PathMountFixture() -> Durin::Testing::FScopedMountRegistryFixture
	{
		const std::array Definitions{
			Durin::FMountPoint{
				.VirtualRoot = "/Game/",
				.Owner = Durin::EMountOwner::Test,
				.Root = std::filesystem::current_path(),
			},
		};
		return Durin::Testing::FScopedMountRegistryFixture(Definitions);
	}
}

TEST(FPathIdentityContractTests, DistinctStructuralKindsRoundTripCanonically)
{
	auto Fixture = PathMountFixture();
	ASSERT_TRUE(Fixture.IsValid()) << Fixture.GetError();
	Durin::FPackagePath PackagePath;
	Durin::FTopLevelAssetPath AssetPath;
	Durin::FObjectPath ObjectPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Objects/Test", PackagePath));
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
		"/Game/Objects/Test.Test", AssetPath));
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(
		"/Game/Objects/Test.Test:Root.Component", ObjectPath));
	EXPECT_EQ(PackagePath.ToString(), "/Game/Objects/Test");
	EXPECT_EQ(PackagePath.GetPackageName(), "Test");
	EXPECT_EQ(AssetPath.GetPackagePath(), PackagePath);
	EXPECT_EQ(AssetPath.GetAssetName(), "Test");
	EXPECT_EQ(ObjectPath.GetPackagePath(), PackagePath);
	EXPECT_EQ(ObjectPath.GetAssetPath(), AssetPath);
	ASSERT_EQ(ObjectPath.GetSubobjectNames().size(), 2u);
	EXPECT_EQ(ObjectPath.GetSubobjectNames()[0], "Root");
	EXPECT_EQ(ObjectPath.GetSubobjectNames()[1], "Component");
	EXPECT_EQ(ObjectPath.ToString(), "/Game/Objects/Test.Test:Root.Component");
}

TEST(FPathIdentityContractTests, RejectsAmbiguousNoncanonicalAndBoundedSpellingsAtomically)
{
	auto Fixture = PathMountFixture();
	ASSERT_TRUE(Fixture.IsValid()) << Fixture.GetError();
	Durin::FPackagePath PackagePath;
	Durin::FTopLevelAssetPath AssetPath;
	Durin::FObjectPath ObjectPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Keep", PackagePath));
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate("/Game/Keep.Asset", AssetPath));
	ASSERT_TRUE(Durin::FObjectPath::TryCreate("/Game/Keep.Asset:Child", ObjectPath));
	for (const std::string_view Invalid : {
		"Game/Package", "/Game/", "/Game/A.B", "/Game/A:B",
		"/Game//A", "/Game/../A", "/Unknown/A"})
	{
		EXPECT_FALSE(Durin::FPackagePath::TryCreate(Invalid, PackagePath)) << Invalid;
		EXPECT_EQ(PackagePath.ToString(), "/Game/Keep");
	}
	for (const std::string_view Invalid : {
		"/Game/A", "/Game/A.", "/Game/A.Asset.Child", "/Game/A.Asset:Child"})
	{
		EXPECT_FALSE(Durin::FTopLevelAssetPath::TryCreate(Invalid, AssetPath)) << Invalid;
		EXPECT_EQ(AssetPath.ToString(), "/Game/Keep.Asset");
	}
	for (const std::string_view Invalid : {
		"/Game/A", "/Game/A.Asset:", "/Game/A.Asset:.Child",
		"/Game/A.Asset:Child.", "/Game/A.Asset:Child..Grandchild",
		"/Game/A.Asset:Child:Grandchild"})
	{
		EXPECT_FALSE(Durin::FObjectPath::TryCreate(Invalid, ObjectPath)) << Invalid;
		EXPECT_EQ(ObjectPath.ToString(), "/Game/Keep.Asset:Child");
	}
	const std::string OversizedComponent(
		Durin::MaximumObjectPathComponentBytes + 1, 'a');
	EXPECT_FALSE(Durin::FObjectPath::TryCreate(
		std::format("/Game/A.Asset:{}", OversizedComponent), ObjectPath));
	EXPECT_EQ(ObjectPath.ToString(), "/Game/Keep.Asset:Child");
}

TEST(FPathIdentityContractTests, EqualityHashingAndOrderingAreCaseSensitiveAndCanonical)
{
	auto Fixture = PathMountFixture();
	ASSERT_TRUE(Fixture.IsValid()) << Fixture.GetError();
	Durin::FObjectPath Upper;
	Durin::FObjectPath Lower;
	Durin::FObjectPath Nested;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate("/Game/A.Asset", Upper));
	ASSERT_TRUE(Durin::FObjectPath::TryCreate("/Game/a.Asset", Lower));
	ASSERT_TRUE(Durin::FObjectPath::TryCreate("/Game/A.Asset:Child", Nested));
	EXPECT_NE(Upper, Lower);
	EXPECT_EQ((std::unordered_set<Durin::FObjectPath>{Upper, Lower}).size(), 2u);
	std::vector Paths{Lower, Nested, Upper};
	std::ranges::sort(Paths);
	EXPECT_EQ(Paths, (std::vector{Upper, Nested, Lower}));
}

TEST(FPackageLinkerContractTests, PackageIndicesValidateBoundariesAndRoundTrip)
{
	Package::FPackageIndex Index;
	EXPECT_TRUE(Package::FPackageIndex::TryFromRaw(0, Index));
	EXPECT_TRUE(Index.IsNull());
	EXPECT_TRUE(Package::FPackageIndex::TryFromRaw(1, Index));
	EXPECT_TRUE(Index.IsExport());
	EXPECT_EQ(Index.GetTableIndex(), 0u);
	EXPECT_EQ(Index.ToRaw(), 1);
	EXPECT_TRUE(Package::FPackageIndex::TryFromRaw(-1, Index));
	EXPECT_TRUE(Index.IsImport());
	EXPECT_EQ(Index.GetTableIndex(), 0u);
	EXPECT_EQ(Index.ToRaw(), -1);
	EXPECT_TRUE(Package::FPackageIndex::TryFromRaw(std::numeric_limits<int32>::max(), Index));
	EXPECT_EQ(Index.ToRaw(), std::numeric_limits<int32>::max());
	EXPECT_TRUE(Package::FPackageIndex::TryFromRaw(-int64(std::numeric_limits<int32>::max()), Index));
	EXPECT_EQ(Index.ToRaw(), -int64(std::numeric_limits<int32>::max()));
	EXPECT_FALSE(Package::FPackageIndex::TryFromRaw(int64(std::numeric_limits<int32>::max()) + 1, Index));
	EXPECT_FALSE(Package::FPackageIndex::TryFromRaw(-int64(std::numeric_limits<int32>::max()) - 1, Index));
	EXPECT_FALSE(Package::FPackageIndex::TryImport(std::numeric_limits<int32>::max(), Index));
	EXPECT_FALSE(Package::FPackageIndex::TryExport(std::numeric_limits<int32>::max(), Index));
}

TEST(FPackageLinkerContractTests, StructuralTypesCompareDeterministically)
{
	const Package::FSerializedType I32{.Kind = Package::EValueKind::I32};
	const Package::FSerializedType String{.Kind = Package::EValueKind::String};
	const Package::FSerializedType Array{.Kind = Package::EValueKind::Array, .Children = {I32}};
	const Package::FSerializedType Map{.Kind = Package::EValueKind::Map, .Children = {String, Array}};
	EXPECT_EQ(Map, Map);
	EXPECT_NE(Map, Array);
	std::vector Values{Map, I32, String, Array};
	std::ranges::sort(Values);
	EXPECT_TRUE(std::ranges::is_sorted(Values));
}

TEST(FPackageLinkerContractTests, CheckedTablesResolvePathsAndRejectInvalidTopology)
{
	Package::FPackageIndex Root;
	Package::FPackageIndex Child;
	ASSERT_TRUE(Package::FPackageIndex::TryExport(0, Root));
	ASSERT_TRUE(Package::FPackageIndex::TryExport(1, Child));
	Package::FLinkerTables Tables;
	Tables.Names = {"Root", "Child"};
	Tables.Exports = {
		{.ObjectName = "Root", .ClassName = "Example::Root"},
		{.ObjectName = "Child", .ClassName = "Example::Child", .Outer = Root},
	};
	std::string_view Name;
	EXPECT_TRUE(Tables.TryGetName(1, Name));
	EXPECT_EQ(Name, "Root");
	EXPECT_FALSE(Tables.TryGetName(0, Name));
	std::string Path = "sentinel";
	Package::FLinkerDiagnostic Diagnostic;
	EXPECT_TRUE(Tables.TryResolvePath(Child, Path, &Diagnostic));
	EXPECT_EQ(Path, "Root/Child");
	Package::FPackageIndex Import;
	ASSERT_TRUE(Package::FPackageIndex::TryImport(0, Import));
	Tables.Imports = {{.PackageName = "/Game/External"}};
	EXPECT_TRUE(Tables.TryResolvePath(Import, Path, &Diagnostic));
	EXPECT_EQ(Path, "/Game/External");
	Tables.Exports.front().Outer = Child;
	EXPECT_FALSE(Tables.TryResolvePath(Child, Path, &Diagnostic));
	EXPECT_EQ(Diagnostic.Failure, Package::ELinkerFailure::InvalidTopology);
	EXPECT_EQ(Path, "/Game/External");
}

TEST(FPackageLinkerContractTests, CanonicalScalarTokensFreezeOrderingAndFloatNormalization)
{
	EXPECT_EQ(Token(Package::EValueKind::I8, {.Signed = -128}), Bytes({2, 0x00}));
	EXPECT_EQ(Token(Package::EValueKind::I8, {.Signed = 127}), Bytes({2, 0xff}));
	EXPECT_EQ(Token(Package::EValueKind::I16, {.Signed = -32768}), Bytes({3, 0x00, 0x00}));
	EXPECT_EQ(Token(Package::EValueKind::I16, {.Signed = 32767}), Bytes({3, 0xff, 0xff}));
	EXPECT_EQ(Token(Package::EValueKind::I32, {.Signed = std::numeric_limits<int32>::min()}),
		Bytes({4, 0x00, 0x00, 0x00, 0x00}));
	EXPECT_EQ(Token(Package::EValueKind::I32, {.Signed = std::numeric_limits<int32>::max()}),
		Bytes({4, 0xff, 0xff, 0xff, 0xff}));
	EXPECT_EQ(Token(Package::EValueKind::I64, {.Signed = std::numeric_limits<int64>::min()}),
		Bytes({5, 0, 0, 0, 0, 0, 0, 0, 0}));
	EXPECT_EQ(Token(Package::EValueKind::I64, {.Signed = std::numeric_limits<int64>::max()}),
		Bytes({5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
	EXPECT_EQ(Token(Package::EValueKind::U16, {.Unsigned = 0x1234}), Bytes({7, 0x12, 0x34}));
	EXPECT_EQ(Token(Package::EValueKind::F32, {.FloatingBits = 0x00000000}),
		Bytes({10, 0x80, 0x00, 0x00, 0x00}));
	EXPECT_EQ(Token(Package::EValueKind::F32, {.FloatingBits = 0x80000000}),
		Bytes({10, 0x80, 0x00, 0x00, 0x00}));
	EXPECT_EQ(Token(Package::EValueKind::F32, {.FloatingBits = 0x7fc01234}),
		Bytes({10, 0xff, 0xc0, 0x12, 0x34}));
	EXPECT_EQ(Token(Package::EValueKind::F32, {.FloatingBits = 0xffc01234}),
		Bytes({10, 0x00, 0x3f, 0xed, 0xcb}));
	EXPECT_EQ(Token(Package::EValueKind::F64, {.FloatingBits = 0x8000000000000000ull}),
		Bytes({11, 0x80, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(FPackageLinkerContractTests, CanonicalNamesGuidsEnumsAndStructsHaveExactFraming)
{
	EXPECT_EQ(Token(Package::EValueKind::Name, {.Text = "A", .NameNumber = 2}),
		Bytes({18, 0, 0, 0, 0, 0, 0, 0, 1, 'A', 0, 0, 0, 2}));
	EXPECT_EQ(Token(Package::EValueKind::Guid, {.Guid = {1, 2, 3, 4}}),
		Bytes({19, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4}));

	Package::FSerializedType Enum{
		.Kind = Package::EValueKind::Enum,
		.QualifiedName = "Example::Mode",
		.Parameter = static_cast<uint64>(Package::EValueKind::I16),
	};
	std::vector<std::byte> EnumToken;
	ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(Enum, {.Signed = -1}, EnumToken));
	EXPECT_EQ(EnumToken, Bytes({13, 0x7f, 0xff}));

	Package::FSerializedType Struct{
		.Kind = Package::EValueKind::Struct,
		.QualifiedName = "Example::Key",
		.Children = {{.Kind = Package::EValueKind::I8}, {.Kind = Package::EValueKind::Bool}},
	};
	Package::FSerializedValue StructValue;
	StructValue.Elements = {{.Signed = 0}, {.Bool = true}};
	std::vector<std::byte> StructToken;
	ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(Struct, StructValue, StructToken));
	EXPECT_EQ(StructToken, Bytes({
		17,
		0, 0, 0, 0, 0, 0, 0, 0, 2, 0x80,
		0, 0, 0, 1, 0, 0, 0, 0, 1, 1,
	}));

	Package::FSerializedType Intrinsic{
		.Kind = Package::EValueKind::Intrinsic,
		.Parameter = 1,
	};
	Package::FSerializedValue IntrinsicValue;
	IntrinsicValue.ComponentBits = {std::bit_cast<uint64>(1.0), std::bit_cast<uint64>(-0.0)};
	std::vector<std::byte> IntrinsicToken;
	ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(Intrinsic, IntrinsicValue, IntrinsicToken));
	EXPECT_EQ(IntrinsicToken, Bytes({
		17,
		0, 0, 0, 0, 0, 0, 0, 0, 11, 0xbf, 0xf0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 1, 0, 0, 0, 0, 11, 0x80, 0, 0, 0, 0, 0, 0, 0,
	}));
}

TEST(FPackageLinkerContractTests, CanonicalEnumStorageWidthsRemainExact)
{
	using EKind = Package::EValueKind;
	for (const auto [Storage, Value, Expected] : {
		std::tuple{EKind::I8, int64(-1), Bytes({13, 0x7f})},
		std::tuple{EKind::I16, int64(-1), Bytes({13, 0x7f, 0xff})},
		std::tuple{EKind::I32, int64(-1), Bytes({13, 0x7f, 0xff, 0xff, 0xff})},
		std::tuple{EKind::I64, int64(-1), Bytes({13, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
	})
	{
		Package::FSerializedType Type{.Kind = EKind::Enum, .Parameter = static_cast<uint64>(Storage)};
		std::vector<std::byte> Actual;
		ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(Type, {.Signed = Value}, Actual));
		EXPECT_EQ(Actual, Expected);
	}
	for (const auto [Storage, Value, Expected] : {
		std::tuple{EKind::U8, uint64(0xff), Bytes({13, 0xff})},
		std::tuple{EKind::U16, uint64(0xffff), Bytes({13, 0xff, 0xff})},
		std::tuple{EKind::U32, uint64(0xffffffffu), Bytes({13, 0xff, 0xff, 0xff, 0xff})},
		std::tuple{EKind::U64, std::numeric_limits<uint64>::max(), Bytes({13, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff})},
	})
	{
		Package::FSerializedType Type{.Kind = EKind::Enum, .Parameter = static_cast<uint64>(Storage)};
		std::vector<std::byte> Actual;
		ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(Type, {.Unsigned = Value}, Actual));
		EXPECT_EQ(Actual, Expected);
	}
}

TEST(FPackageLinkerContractTests, CanonicalTokenFailureIsAtomic)
{
	std::vector<std::byte> TokenBytes = Bytes({0xaa});
	std::string Error;
	EXPECT_FALSE(Package::BuildCanonicalMapKeyToken(
		{.Kind = Package::EValueKind::Map}, {}, TokenBytes, &Error));
	EXPECT_EQ(TokenBytes, Bytes({0xaa}));
	EXPECT_EQ(Error, "CanonicalMapKeyUnsupported: value type is not canonicalizable.");
}

TEST(FPackageLinkerContractTests, LiveReflectedAndDetachedValuesShareCanonicalBytes)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	int32 Integer = -7;
	Durin::FNumericProperty IntegerProperty(
		Durin::FFieldVariant(), Durin::FName("Integer"), Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::None, 1, 0, sizeof(Integer),
		Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
	std::vector<std::byte> Live;
	std::vector<std::byte> Detached;
	std::string Error;
	ASSERT_TRUE(Durin::BuildCanonicalMapKeyToken(
		&IntegerProperty, &Integer, 0, Live, &Error)) << Error;
	ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(
		{.Kind = Package::EValueKind::I32}, {.Signed = Integer}, Detached, &Error)) << Error;
	EXPECT_EQ(Live, Detached);

	std::string Text = "shared";
	Durin::FStringProperty StringProperty(
		Durin::FFieldVariant(), Durin::FName("Text"), Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::None, 1, 0, sizeof(Text),
		Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr);
	ASSERT_TRUE(Durin::BuildCanonicalMapKeyToken(
		&StringProperty, &Text, 0, Live, &Error)) << Error;
	ASSERT_TRUE(Package::BuildCanonicalMapKeyToken(
		{.Kind = Package::EValueKind::String}, {.Text = Text}, Detached, &Error)) << Error;
	EXPECT_EQ(Live, Detached);
}
