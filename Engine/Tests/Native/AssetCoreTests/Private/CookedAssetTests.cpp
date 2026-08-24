#include <gtest/gtest.h>

#include "AssetTools.h"
#include "AssetPackageV5Codec.h"
#include "Asset/PackageObjectStreamWriter.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	auto Payload(FGuid Id, std::initializer_list<uint8> Bytes, uint32 Alignment = 16) -> FCookedBulkPayload
	{
		std::vector<std::byte> PayloadBytes;
		PayloadBytes.reserve(Bytes.size());
		for (uint8 Byte : Bytes) PayloadBytes.push_back(static_cast<std::byte>(Byte));
		return {Id, 1, 7, ECookedPayloadCompression::None, Alignment, std::move(PayloadBytes)};
	}

	auto MakeBulk(std::vector<FCookedPayloadDescriptor>* OutDescriptors = nullptr) -> std::vector<std::byte>
	{
		const std::array Payloads = {
			Payload(FGuid(2, 0, 0, 0), {9, 8, 7}, 64),
			Payload(FGuid(1, 0, 0, 0), {1, 2, 3, 4}, 16)};
		std::vector<std::byte> Bytes;
		EXPECT_TRUE(EncodeCookedBulk(Payloads, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes, OutDescriptors));
		return Bytes;
	}

	auto MakePackageBytes() -> std::vector<std::byte>
	{
		static const bool bInitialized = [] {
			Testing::InitializeDObjectSystemForTests();
			return true;
		}();
		(void)bInitialized;
		const std::string ClassName =
			DObject::StaticClass()->GetQualifiedName().ToString();
		Asset::PackageObjectStream::FPackageInput Input{
			.AssetClass = ClassName,
			.Objects = {{"Root", {}, ClassName, "Root"}},
			.ObjectValues = {{"Root", {}}},
		};
		std::vector<std::byte> ObjectStream;
		Asset::PackageObjectStream::FWriterDiagnostic Diagnostic;
		EXPECT_TRUE(Asset::PackageObjectStream::WritePackage(Input, ObjectStream, &Diagnostic))
			<< Diagnostic.Message;
		std::vector<std::byte> Bytes;
		EXPECT_TRUE(Asset::Private::DastV5::BuildPackageFromObjectStream(
			ObjectStream, Bytes));
		return Bytes;
	}

	template<typename T>
	auto WriteLittle(std::vector<std::byte>& Bytes, size_t Offset, T Value) -> void
	{
		for (size_t Index = 0; Index < sizeof(T); ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto RefreshTableHash(std::vector<std::byte>& Bytes) -> void
	{
		const uint32 Count = std::to_integer<uint32>(Bytes[24]);
		const uint64 Hash = FXxHash64::HashBuffer(std::span(Bytes).subspan(64, Count * 80)).HashValue;
		WriteLittle(Bytes, 48, Hash);
	}
}

TEST(FCookedBulkTests, ProducesDeterministicSortedMultiPayloadContainer)
{
	std::vector<FCookedPayloadDescriptor> Descriptors;
	const std::vector<std::byte> First = MakeBulk(&Descriptors);
	const std::vector<std::byte> Second = MakeBulk();
	EXPECT_EQ(First, Second);
	EXPECT_EQ(First.size(), 259u);
	const FXxHash128 GoldenHash = FXxHash128::HashBuffer(First);
	EXPECT_EQ(GoldenHash.HashLow, 12417320302211656157ull);
	EXPECT_EQ(GoldenHash.HashHigh, 3049470508272984121ull);
	ASSERT_EQ(Descriptors.size(), 2u);
	EXPECT_EQ(Descriptors[0].PayloadId, FGuid(1, 0, 0, 0));
	EXPECT_EQ(Descriptors[1].PayloadId, FGuid(2, 0, 0, 0));

	FCookedBulkContainer Container;
	ASSERT_TRUE(DecodeCookedBulk(First, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));
	std::span<const std::byte> Resolved;
	ASSERT_TRUE(ResolveCookedPayload(Container, Descriptors[1], Resolved));
	EXPECT_TRUE(std::ranges::equal(Resolved, Container.Payloads[1]));
}

TEST(FCookedBulkTests, RejectsWrongTargetCorruptionOverlapTruncationAndUnknownCompression)
{
	FCookedBulkContainer Container;
	std::vector<std::byte> Bytes = MakeBulk();
	EXPECT_FALSE(DecodeCookedBulk(Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::EditorValidation, Container));

	auto Corrupt = Bytes;
	Corrupt.back() ^= std::byte{1};
	EXPECT_FALSE(DecodeCookedBulk(Corrupt, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));

	auto Overlap = Bytes;
	const uint64 FirstOffset = [] (const std::vector<std::byte>& Value) {
		uint64 Result = 0;
		for (size_t Index = 0; Index < 8; ++Index)
			Result |= std::to_integer<uint64>(Value[64 + 40 + Index]) << (Index * 8);
		return Result;
	}(Overlap);
	WriteLittle(Overlap, 64 + 80 + 40, FirstOffset);
	RefreshTableHash(Overlap);
	EXPECT_FALSE(DecodeCookedBulk(Overlap, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));

	auto UnknownCompression = Bytes;
	WriteLittle(UnknownCompression, 64 + 32, uint32{99});
	RefreshTableHash(UnknownCompression);
	EXPECT_FALSE(DecodeCookedBulk(UnknownCompression, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));

	Bytes.pop_back();
	EXPECT_FALSE(DecodeCookedBulk(Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));
}

TEST(FCookedBulkTests, DescriptorMustExactlyMatchEntry)
{
	std::vector<FCookedPayloadDescriptor> Descriptors;
	const std::vector<std::byte> Bytes = MakeBulk(&Descriptors);
	FCookedBulkContainer Container;
	ASSERT_TRUE(DecodeCookedBulk(Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));
	Descriptors[0].StoredSize++;
	std::span<const std::byte> Resolved;
	EXPECT_FALSE(ResolveCookedPayload(Container, Descriptors[0], Resolved));
	Descriptors[0] = Container.Entries[0];
	Descriptors[0].LocationKind = 99;
	EXPECT_FALSE(ResolveCookedPayload(Container, Descriptors[0], Resolved));
}

TEST(FCookedBulkTests, RejectsDeclaredExcessivePayloadSizeBeforeAllocation)
{
	std::vector<std::byte> Bytes = MakeBulk();
	WriteLittle(Bytes, 64 + 48, uint64{8ull * 1024 * 1024 * 1024 + 1});
	WriteLittle(Bytes, 64 + 56, uint64{8ull * 1024 * 1024 * 1024 + 1});
	RefreshTableHash(Bytes);
	FCookedBulkContainer Container;
	EXPECT_FALSE(DecodeCookedBulk(Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));
}

TEST(FCookedPathTests, ResolvesRelocatableCompanionAndRejectsTraversalAndWrongMount)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "CookedPath";
	std::filesystem::path Package, Companion;
	ASSERT_TRUE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/Textures/T", Package));
	ASSERT_TRUE(ResolveCookedCompanionPath(std::filesystem::absolute(Root), Package, Companion));
	EXPECT_EQ(Package.filename(), "T.dasset");
	EXPECT_EQ(Companion.filename(), "T.dbulk");
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/../Escape", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/DDC/Object", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/CON", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "G:/Source/T", Package));
}

TEST(FCookedPathTests, LoadsDescriptorSelectedPayloadWithExplicitContainerLifetime)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookedPayloadLoad");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::vector<FCookedPayloadDescriptor> Descriptors;
	const std::vector<std::byte> Bulk = MakeBulk(&Descriptors);
	const std::filesystem::path Companion = Root / "Game/Textures/T.dbulk";
	std::filesystem::create_directories(Companion.parent_path());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(Bulk)), Companion));

	FCookedPackagePayload Loaded;
	std::string Error;
	FAssetRuntimeConfiguration Runtime = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Root, Runtime));
	ASSERT_TRUE(LoadCookedPackagePayload(
		Runtime,
		"/Game/Textures/T",
		Descriptors[1],
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		Loaded,
		&Error)) << Error;
	ASSERT_EQ(Loaded.Container.Payloads.size(), 2u);
	EXPECT_TRUE(std::ranges::equal(Loaded.Payload, Loaded.Container.Payloads[1]));
	EXPECT_EQ(Loaded.Payload.data(), Loaded.Container.Payloads[1].data());
	EXPECT_EQ(Loaded.Payload.size(), Loaded.Container.Payloads[1].size());

	const std::span<const std::byte> Previous = Loaded.Payload;
	EXPECT_FALSE(LoadCookedPackagePayload(
		Runtime,
		"/Game/Textures/Missing",
		Descriptors[0],
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		Loaded,
		&Error));
	EXPECT_EQ(Loaded.Payload.data(), Previous.data());

	FCookedPayloadDescriptor WrongTarget = Descriptors[1];
	WrongTarget.TargetProfile = static_cast<uint32>(ECookTargetProfile::EditorValidation);
	FCookedPackagePayload Rejected;
	EXPECT_FALSE(LoadCookedPackagePayload(Runtime, "/Game/Textures/T",
		WrongTarget, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Rejected, &Error));
}

TEST(FCookedPathTests, ImmutableRuntimeConfigurationRejectsReplacementAndPackageMutation)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookedMode");
	FAssetRuntimeConfiguration Runtime = FAssetRuntimeConfiguration::Authored();
	FAssetRuntimeConfiguration Invalid = Runtime;
	EXPECT_FALSE(FAssetRuntimeConfiguration::Cooked("relative/cook", Invalid));
	EXPECT_EQ(Invalid, Runtime);
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Root, Runtime));
	EXPECT_FALSE(Runtime.AllowsSourceFallback());
	EXPECT_FALSE(Runtime.AllowsDerivedDataFallback());
	ShutdownAssetManager();
	ASSERT_TRUE(InitializeAssetManager(Runtime));
	EXPECT_TRUE(GetAssetRuntimeConfiguration().RequiresCookedPayload());
	EXPECT_FALSE(InitializeAssetManager(FAssetRuntimeConfiguration::Authored()));
	EXPECT_EQ(SavePackage(nullptr).Error, EAssetError::ReadOnlyMode);
	FCookedBulkContainer Missing;
	EXPECT_FALSE(LoadCookedBulkFile(
		Root / "Game/DefinitelyMissing.dbulk",
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		Missing));
	ShutdownAssetManager();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST(FCookManifestTests, IsDeterministicAndRejectsCorruptRecords)
{
	FCookManifest Manifest{
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		{{ECookManifestEntryKind::CookedBulk, 1, "Game/B.dbulk", 2, 3, 4},
		 {ECookManifestEntryKind::CookedPackage, 1, "Game/A.dasset", 1, 5, 6}}};
	std::vector<std::byte> First, Second;
	ASSERT_TRUE(EncodeCookManifest(Manifest, First));
	ASSERT_TRUE(EncodeCookManifest(Manifest, Second));
	EXPECT_EQ(First, Second);
	const FXxHash128 GoldenHash = FXxHash128::HashBuffer(First);
	EXPECT_EQ(First.size(), 137u);
	EXPECT_EQ(GoldenHash.HashLow, 1127403949174504654ull);
	EXPECT_EQ(GoldenHash.HashHigh, 9302219320893799974ull);
	FCookManifest Decoded;
	ASSERT_TRUE(DecodeCookManifest(First, Decoded));
	ASSERT_EQ(Decoded.Entries.size(), 2u);
	EXPECT_EQ(Decoded.Entries[0].RelativePath, "Game/A.dasset");
	First.back() ^= std::byte{1};
	EXPECT_FALSE(DecodeCookManifest(First, Decoded));
}

TEST(FCookContextTests, PublishesRelocatesAndCleansOnlyManifestOwnedStaleOutputs)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookPublication");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FCookContext First(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(First.AddPackage("/Game/Old", MakePackageBytes(), {Payload(FGuid(1, 0, 0, 0), {3, 4})}));
	ASSERT_TRUE(First.Publish(&Error)) << Error;
	ASSERT_TRUE(std::filesystem::exists(Root / "Game/Old.dasset"));
	ASSERT_TRUE(std::filesystem::exists(Root / "Game/Old.dbulk"));
	const std::filesystem::path Unowned = Root / "keep.txt";
	const std::array<uint8, 1> Keep{9};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(
		std::span{reinterpret_cast<const std::byte*>(Keep.data()), Keep.size()}, Unowned));

	FCookContext Second(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Second.AddPackage("/Game/New", MakePackageBytes(), {Payload(FGuid(2, 0, 0, 0), {7, 8})}));
	ASSERT_TRUE(Second.Publish(&Error)) << Error;
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Old.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Old.dbulk"));
	EXPECT_TRUE(std::filesystem::exists(Unowned));

	const std::filesystem::path Relocated = Root.parent_path() / "CookPublicationRelocated";
	Durin::Testing::RemoveTestWorkDirectory(Relocated);
	std::filesystem::rename(Root, Relocated);
	std::vector<std::byte> BulkBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(BulkBytes, (Relocated / "Game/New.dbulk")));
	FCookedBulkContainer Container;
	EXPECT_TRUE(DecodeCookedBulk(BulkBytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Container));
}

TEST(FCookContextTests, PublishesPackageWithoutBulkCompanion)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "PackageOnlyCook");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FCookContext Context(
		Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Context.AddPackage("/Engine/Plain", MakePackageBytes(), {}));
	ASSERT_TRUE(Context.Publish(&Error)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		Root / "Engine/Plain.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Engine/Plain.dbulk"));

	std::vector<std::byte> ManifestBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(
		ManifestBytes, (Root / "CookManifest.bin")));
	FCookManifest Manifest;
	ASSERT_TRUE(DecodeCookManifest(ManifestBytes, Manifest));
	ASSERT_EQ(Manifest.Entries.size(), 1u);
	EXPECT_EQ(
		Manifest.Entries[0].Kind,
		ECookManifestEntryKind::CookedPackage);
}

TEST(FCookContextTests, DescriptorAwarePackageBuilderReceivesExactPublishedEntries)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "DescriptorAwareCook");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FCookContext Context(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookedPayloadDescriptor Captured;
	std::string Error;
	ASSERT_TRUE(Context.AddPackage(
		"/Game/DescriptorAware",
		{Payload(FGuid(4, 3, 2, 1), {7, 6, 5}, 64)},
		[&](std::span<const FCookedPayloadDescriptor> Descriptors, std::vector<std::byte>& OutBytes, std::string*) {
			if (Descriptors.size() != 1) return false;
			Captured = Descriptors.front();
			OutBytes = MakePackageBytes();
			return true;
		}));
	ASSERT_TRUE(Context.Publish(&Error)) << Error;

	FCookedBulkContainer Container;
	ASSERT_TRUE(LoadCookedBulkFile(
		Root / "Game/DescriptorAware.dbulk",
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		Container));
	ASSERT_EQ(Container.Entries.size(), 1u);
	EXPECT_EQ(Captured, Container.Entries.front());
}

TEST(FCookContextTests, PackagePublicationFailureLeavesNoReferencingPackageOrManifest)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookInterruption");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root / "Game/Blocked.dasset");
	FCookContext Context(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Context.AddPackage("/Game/Blocked", MakePackageBytes(), {Payload(FGuid(1, 0, 0, 0), {2})}));
	EXPECT_FALSE(Context.Publish());
	EXPECT_FALSE(std::filesystem::is_regular_file(Root / "Game/Blocked.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "CookManifest.bin"));
}

TEST(FCookContextTests, InvalidPackageFailsBeforeBulkPublication)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookInvalidPackage");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FCookContext Context(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Context.AddPackage("/Game/Invalid",
		{std::byte{1}, std::byte{2}, std::byte{3}},
		{Payload(FGuid(1, 0, 0, 0), {4})}));
	EXPECT_FALSE(Context.Publish());
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Invalid.dbulk"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Invalid.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "CookManifest.bin"));
}
