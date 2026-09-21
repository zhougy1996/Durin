#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "../../../../Source/Runtime/Engine/Private/Asset/CookMemoryBudget.h"
#include "../../../../Source/Runtime/Engine/Private/Asset/CookDependencyDiscovery.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Logging/Logger.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Materials/Material.h"
#include "NativeTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCookedData.h"
#include "Texture/VolumeTexture.h"

namespace
{
	using namespace Durin;

	template<typename T>
	concept HasPublicAddToCook = requires(
		T& Value, FCookContext& Context, std::string& Error)
	{
		Value.AddToCook(Context, std::string_view{}, Error);
	};

	auto MakePackageBytes() -> Durin::FByteBuffer
	{
		static const bool bInitialized = [] {
			Testing::InitializeDObjectSystemForTests();
			return true;
		}();
		(void)bInitialized;
		static const bool bMounted = [] {
			const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
											   / "CookPackageFixtureMount";
			std::filesystem::create_directories(Root);
			Testing::RegisterMountPointForTests(
				"/TestCook/", Root.generic_string() + "/"
			);
			return true;
		}();
		(void)bMounted;
		static uint64 NextPackage = 1;
		FPackagePath Path;
		EXPECT_TRUE(FPackagePath::TryCreate(std::format("/TestCook/Fixture{}", NextPackage++), Path));
		DPackage* Package = CreatePackage(Path);
		EXPECT_NE(Package, nullptr);
		DObject* Asset = NewObject<DObject>(Package, "Root");
		EXPECT_NE(Asset, nullptr);
		EXPECT_EQ(Package->FindTopLevelAsset(Asset->GetFName()), Asset);
		Durin::FByteBuffer Bytes;
		const auto Result = SerializeAssetPackageBytes(Package, Bytes);
		EXPECT_TRUE(Result) << Result.Message;
		EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
		return Bytes;
	}

	template<class TTexture, class TPlatformData>
	auto ExpectCookedTextureDecodeBoundaries(
		TPlatformData PlatformData, std::string_view Family) -> void
	{
		auto& Logger = FLogger::Get();
		FLogSettings LogSettings;
		LogSettings.LogDirectory = (Testing::GetTestWorkDirectory() / "CookedDecodeLogs").string();
		ASSERT_TRUE(Logger.Initialize(LogSettings));
		struct FScopedLoggerShutdown
		{
			~FScopedLoggerShutdown() { FLogger::Get().Shutdown(); }
		} LoggerShutdown;

		auto* Texture = NewObject<TTexture>(nullptr, "CookedDecodeBoundary");
		ASSERT_NE(Texture, nullptr);
		std::string Error;
		Texture->SetPlatformData(std::make_unique<TPlatformData>(PlatformData));
		const auto* Installed = Texture->GetPlatformData();
		const auto RevisionIdentity = Texture->GetPlatformDataShared();
		FByteBuffer ValidBytes;
		FCanonicalMemoryWriter Writer(ValidBytes, EArchivePurpose::CookedPayload,
			{.Target = {"Win64", "Game"}});
		PlatformData.Serialize(Writer);
		ASSERT_FALSE(Writer.IsError()) << Writer.GetError();
		FByteBuffer TrailingBytes = ValidBytes;
		TrailingBytes.push_back(std::byte{0x7f});

		FBulkData Bulk;
		EXPECT_FALSE(TexturePrivate::LoadCookedPlatformData<TPlatformData>(
			*Texture, Bulk, Family));
		for (const FByteBuffer& InvalidBytes : {FByteBuffer{std::byte{0xff}}, TrailingBytes})
		{
			{
				auto ValueResult = FBulkData::TryCreateDetached(InvalidBytes);
				ASSERT_TRUE(ValueResult);
				Bulk = std::move(*ValueResult);
			}
			Logger.Flush();
			const uint64 LogCursor = Logger.ReadRecords(1, 0).NewestAvailableSequence + 1;
			EXPECT_FALSE(TexturePrivate::LoadCookedPlatformData<TPlatformData>(
				*Texture, Bulk, Family));
			Logger.Flush();
			const auto Logs = Logger.ReadRecords(LogCursor);
			const std::string DiagnosticPrefix = std::format("Cooked {} '{}': ", Family, Texture->GetObjectPath());
			EXPECT_EQ(std::ranges::count_if(Logs.Records, [&](const FLogRecord& Record) {
				return Record.Level == ELogLevel::Warn && Record.Message.starts_with(DiagnosticPrefix)
					&& Record.Message.size() > DiagnosticPrefix.size();
			}), 1);
			EXPECT_EQ(Texture->GetPlatformData(), Installed);
			// A failed decoder must release its lock so the payload can be retried.
			Durin::FByteView LockedBytes;
			Durin::FBulkDataReadResult LockedBytesLease;
			LockedBytesLease = Bulk.AcquireRead();
			ASSERT_TRUE(LockedBytesLease) << Durin::FormatPackageResourceReadError(LockedBytesLease.Error);
			LockedBytes = LockedBytesLease.Lock.GetBytes();
			LockedBytesLease.Lock.Reset();
		}
		{
			auto ValueResult = FBulkData::TryCreateDetached(ValidBytes);
			ASSERT_TRUE(ValueResult);
			Bulk = std::move(*ValueResult);
		}
		ASSERT_TRUE(TexturePrivate::LoadCookedPlatformData<TPlatformData>(
			*Texture, Bulk, Family));
		EXPECT_TRUE(Texture->HasPlatformData());
		EXPECT_NE(Texture->GetPlatformDataShared(), RevisionIdentity);
		EXPECT_NE(Bulk.GetState(), EBulkDataState::ReadLocked);
	}

} // namespace

TEST(FCookedPathTests, OutputRootValidationRetainsPathsAndSystemFailure)
{
	const auto Relative = ValidateCookOutputRoot("relative-output");
	EXPECT_FALSE(Relative);
	EXPECT_EQ(Relative.Error, ECookOutputRootError::AbsolutePath);
	EXPECT_EQ(Relative.OutputRoot, std::filesystem::path("relative-output"));
	(void)MakePackageBytes();
	const auto Authored = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookPackageFixtureMount" / "Shaders");
	const auto Overlap = ValidateCookOutputRoot(Authored);
	EXPECT_FALSE(Overlap);
	EXPECT_EQ(Overlap.Error, ECookOutputRootError::AuthoredOverlap);
	EXPECT_EQ(Overlap.OutputRoot, Authored);
	EXPECT_EQ(Overlap.RelatedPath, std::filesystem::weakly_canonical(Authored.parent_path()));
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookRootValidation");
	Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root);
	const auto File = Root / "file";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, File));
	const auto NotDirectory = ValidateCookOutputRoot(File);
	EXPECT_FALSE(NotDirectory);
	EXPECT_EQ(NotDirectory.Error, ECookOutputRootError::EnumerateTree);
	EXPECT_EQ(NotDirectory.OutputRoot, File);
	EXPECT_TRUE(NotDirectory.SystemError);
	EXPECT_TRUE(ValidateCookOutputRoot(Root));
	Testing::RemoveTestWorkDirectory(Root);
}

TEST(FCookedPathTests, CoordinatorRetainsRootCauseAndResetsItOnNextRun)
{
	(void)MakePackageBytes();
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookPackageFixtureMount");
	FCookRequest Request{.OutputRoot = Root, .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game};
	FCookRunResult Result;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.OutputRootCause);
	EXPECT_EQ(Result.OutputRootCause->Error, ECookOutputRootError::AuthoredOverlap);
	EXPECT_EQ(Result.OutputRootCause->OutputRoot, Root);
	EXPECT_EQ(Result.OutputRootCause->RelatedPath, std::filesystem::weakly_canonical(Root));
	const auto Retained = Result.OutputRootCause;
	Request.TargetPlatform = ECookTargetPlatform::Invalid;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_FALSE(Result.OutputRootCause);
	EXPECT_EQ(Retained->OutputRoot, Root);
}

TEST(FCookedPathTests, ResolvesMountedCompanionsAndRejectsTraversal)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "CookedPath";
	std::filesystem::path Package, Companion;
	ASSERT_TRUE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/Textures/T", Package));
	ASSERT_TRUE(ResolveCookedCompanionPath(std::filesystem::absolute(Root), Package, Companion));
	EXPECT_EQ(Package.filename(), "T.dasset");
	EXPECT_EQ(Companion.filename(), "T.dbulk");
	ASSERT_TRUE(ResolveCookedPackagePath(
		std::filesystem::absolute(Root), "/Plugins/PCG/Textures/T", Package
	));
	EXPECT_NE(Package.generic_string().find("Plugins/PCG/Textures/T.dasset"), std::string::npos);
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/../Escape", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/CON", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "G:/Source/T", Package));
}

TEST(FCookedPathTests, RejectionsOwnContextAndClearOutput)
{
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookedPaths");
	std::filesystem::path Output = "sentinel";
	std::string Input = "/Game/../Escape";
	const auto Traversal = ResolveCookedPackagePath(Root, Input, Output);
	Input.clear();
	EXPECT_FALSE(Traversal);
	EXPECT_EQ(Traversal.Error, ECookedPathError::NotNormalized);
	EXPECT_EQ(Traversal.VirtualPath, "/Game/../Escape");
	EXPECT_EQ(Traversal.CookRoot, Root);
	EXPECT_TRUE(Output.empty());
	EXPECT_EQ(ResolveCookedPackagePath("relative", "/Game/Test", Output).Error, ECookedPathError::Root);
	EXPECT_EQ(ResolveCookedPackagePath(Root, "relative", Output).Error, ECookedPathError::VirtualPath);
	EXPECT_EQ(ResolveCookedPackagePath(Root, "/Game", Output).Error, ECookedPathError::Mount);
	Output = "sentinel";
	const auto Extension = ResolveCookedCompanionPath(Root, Root / "Test.bin", Output);
	EXPECT_EQ(Extension.Error, ECookedPathError::PackageExtension);
	EXPECT_EQ(Extension.PackagePath, Root / "Test.bin");
	EXPECT_TRUE(Output.empty());
	Output = "sentinel";
	const auto Escape = ResolveCookedCompanionPath(Root, Root.parent_path() / "Outside.dasset", Output);
	EXPECT_EQ(Escape.Error, ECookedPathError::Escape);
	EXPECT_TRUE(Output.empty());
}

TEST(FCookedPathTests, ImmutableRuntimeConfigurationRejectsReplacementAndPackageMutation)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookedMode"
	);
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
	EXPECT_EQ(SavePackage(nullptr).Error, EAssetWriteError::ReadOnlyMode);
	ShutdownAssetManager();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST(FCookedPathTests, ScopedRuntimeRejectsInvalidRootAndRestoresNestedConfigurations)
{
	Testing::InitializeDObjectSystemForTests();
	ASSERT_TRUE(InitializeAssetManager());
	const FAssetRuntimeConfiguration Original = GetAssetRuntimeConfiguration();
	const auto FirstRoot = Testing::CreateTestFixtureDirectory("ScopedCookedRuntimeFirst");
	const auto SecondRoot = Testing::CreateTestFixtureDirectory("ScopedCookedRuntimeSecond");
	Testing::FScopedAssetRuntimeForTests Outer;
	ASSERT_TRUE(Outer.RestartCooked(FirstRoot));
	const FAssetRuntimeConfiguration First = GetAssetRuntimeConfiguration();
	{
		Testing::FScopedAssetRuntimeForTests Inner;
		const auto Invalid = Inner.RestartCooked("relative/cook");
		EXPECT_FALSE(Invalid);
		EXPECT_EQ(GetAssetRuntimeConfiguration(), First);
		EXPECT_FALSE(InitializeAssetManager(Original));
		ASSERT_TRUE(Inner.Restore());
		EXPECT_EQ(GetAssetRuntimeConfiguration(), First);
		ASSERT_TRUE(Inner.RestartCooked(SecondRoot));
		EXPECT_EQ(GetAssetRuntimeConfiguration().GetCookRoot(), SecondRoot);
	}
	EXPECT_EQ(GetAssetRuntimeConfiguration(), First);
	ASSERT_TRUE(Outer.Restore());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
	ASSERT_TRUE(Outer.Restore());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
	// The same scope can switch again after an explicit restore.
	ASSERT_TRUE(Outer.RestartCooked(SecondRoot));
	ASSERT_TRUE(Outer.Restore());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
}

TEST(FCookedPathTests, ScopedRuntimeRestoresAfterFatalAssertion)
{
	Testing::InitializeDObjectSystemForTests();
	ASSERT_TRUE(InitializeAssetManager());
	const FAssetRuntimeConfiguration Original = GetAssetRuntimeConfiguration();
	EXPECT_FATAL_FAILURE({
		Testing::FScopedAssetRuntimeForTests Runtime;
		ASSERT_TRUE(Runtime.RestartCooked(
			Testing::CreateTestFixtureDirectory("ScopedCookedRuntimeEarlyExit")));
		FAIL() << "intentional runtime scope exit";
	}, "intentional runtime scope exit");
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
	ASSERT_TRUE(InitializeAssetManager(Original));
}

TEST(FCookedTextureDataTests, DecodeFailureUnlocksBulkAndPreservesInstalledFamilyData)
{
	Testing::InitializeDObjectSystemForTests();
	FTexturePlatformData Texture2D;
	Texture2D.PixelFormat = EPixelFormat::BC1_UNORM;
	const FPixelFormatLayout Texture2DLayout =
		GetPixelFormatLayout(Texture2D.PixelFormat, 1, 1);
	Texture2D.Mips.push_back({FByteBuffer(Texture2DLayout.DataSize, std::byte{0x7f}),
		1, 1, static_cast<uint32>(Texture2DLayout.RowPitch)});
	ASSERT_NO_FATAL_FAILURE(ExpectCookedTextureDecodeBoundaries<DTexture2D>(Texture2D, "Texture2D"));
	FTextureCubePlatformData Cube;
	Cube.PixelFormat = Texture2D.PixelFormat;
	Cube.Faces.fill(Texture2D);
	ASSERT_NO_FATAL_FAILURE(ExpectCookedTextureDecodeBoundaries<DTextureCube>(Cube, "TextureCube"));
	FVolumeTexturePlatformData Volume;
	Volume.PixelFormat = EPixelFormat::R8_UNORM;
	Volume.Mips.push_back({FByteBuffer(1, std::byte{0x7f}), 1, 1, 1, 1, 1});
	ASSERT_NO_FATAL_FAILURE(ExpectCookedTextureDecodeBoundaries<DVolumeTexture>(Volume, "volume texture"));
	CollectGarbage();
}

TEST(FCookManifestTests, IsDeterministicAndRejectsCorruptRecords)
{
	FCookManifest Manifest{
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		{{ECookManifestEntryKind::CookedBulk, 1, "Game/B.dbulk", 2, 3, 4},
		 {ECookManifestEntryKind::CookedPackage, 1, "Game/A.dasset", 1, 5, 6}}
	};
	Durin::FByteBuffer First, Second;
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
	const auto Corrupt = DecodeCookManifest(First, Decoded);
	EXPECT_FALSE(Corrupt);
	EXPECT_EQ(Corrupt.Error, ECookManifestError::Checksum);
	EXPECT_NE(Corrupt.Actual, Corrupt.Expected);
	EXPECT_TRUE(Decoded.Entries.empty());
}

TEST(FCookManifestTests, TypedFailuresClearOutputsAndOwnEntryContext)
{
	FCookManifest Manifest;
	FByteBuffer Bytes{std::byte{1}};
	const auto Target = EncodeCookManifest(Manifest, Bytes);
	EXPECT_FALSE(Target);
	EXPECT_EQ(Target.Error, ECookManifestError::Target);
	EXPECT_EQ(Target.TargetPlatform, ECookTargetPlatform::Invalid);
	EXPECT_TRUE(Bytes.empty());
	Manifest.TargetPlatform = ECookTargetPlatform::Win64;
	Manifest.TargetProfile = ECookTargetProfile::Game;
	Manifest.Entries.push_back({ECookManifestEntryKind::CookedPackage, 1, "../escape", 1, 0, 0});
	const auto Entry = EncodeCookManifest(Manifest, Bytes);
	Manifest.Entries.clear();
	EXPECT_FALSE(Entry);
	EXPECT_EQ(Entry.Error, ECookManifestError::Entry);
	EXPECT_EQ(Entry.RelativePath, "../escape");
	EXPECT_TRUE(Bytes.empty());
	Manifest.Entries.resize(1);
	const auto Truncated = DecodeCookManifest(FByteBuffer{std::byte{1}}, Manifest);
	EXPECT_FALSE(Truncated);
	EXPECT_EQ(Truncated.Error, ECookManifestError::Truncated);
	EXPECT_EQ(Truncated.Actual, 1u);
	EXPECT_GT(Truncated.Expected, Truncated.Actual);
	EXPECT_TRUE(Manifest.Entries.empty());
}

TEST(FCookStateTests, IsCanonicalVersionedAndRejectsCorruption)
{
	FCookState State{
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		{{"/Game/B", {1, 2}, {3, 4}, {5, 6}, 12, 8, 2, 3, "texture", "ddc-hit"},
		 {"/Game/A", {7, 8}, {9, 10}, {}, 11, 0, 4, 5, "generic", "captured"}}
	};
	State.Entries[1].BuildDependencies = {{ECookBuildDependencyKind::ExternalFile,
		"compiler/include", {std::byte{7}}}};
	Durin::FByteBuffer First, Second;
	std::string Error;
	ASSERT_TRUE(EncodeCookState(State, First)) << Error;
	ASSERT_TRUE(EncodeCookState(State, Second)) << Error;
	EXPECT_EQ(First, Second);
	FCookState Decoded;
	ASSERT_TRUE(DecodeCookState(First, Decoded)) << Error;
	ASSERT_EQ(Decoded.Entries.size(), 2u);
	EXPECT_EQ(Decoded.Entries[0].VirtualPackagePath, "/Game/A");
	EXPECT_EQ(Decoded.Entries[0].BuildDependencies, State.Entries[1].BuildDependencies);
	for (size_t Size = 0; Size < First.size(); ++Size)
	{
		EXPECT_FALSE(DecodeCookState(FByteView(First).first(Size), Decoded));
		EXPECT_TRUE(Decoded.Entries.empty());
	}
	auto Trailing = First; Trailing.push_back(std::byte{0});
	EXPECT_FALSE(DecodeCookState(Trailing, Decoded));
	First[4] = std::byte{1}; // Tool-only v1 is deliberately a full cache miss.
	EXPECT_FALSE(DecodeCookState(First, Decoded));
}

TEST(FCookStateTests, RetainsNestedDependencyErrorsWithoutPartialOutputs)
{
	FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game,
		{{"/Game/State", {}, {}, {}, 1, 0, 1, 1, "generic", "captured"}}};
	State.Entries[0].BuildDependencies = {
		{ECookBuildDependencyKind::ExternalFile, "source", {}},
		{ECookBuildDependencyKind::ExternalFile, "source", {}}};
	FByteBuffer Bytes{std::byte{1}};
	const auto Encoded = EncodeCookState(State, Bytes);
	EXPECT_FALSE(Encoded);
	EXPECT_EQ(Encoded.Error, ECookStateError::Dependency);
	EXPECT_EQ(Encoded.VirtualPath, "/Game/State");
	ASSERT_TRUE(Encoded.DependencyCause);
	EXPECT_EQ(Encoded.DependencyCause->Error, ECookDependencyCodecError::Duplicate);
	EXPECT_EQ(Encoded.DependencyCause->LogicalName, "source");
	EXPECT_TRUE(Bytes.empty());
	State.Entries[0].BuildDependencies.clear();
	ASSERT_TRUE(EncodeCookState(State, Bytes));
	ASSERT_GE(Bytes.size(), 8u);
	Bytes[Bytes.size() - 8] = std::byte{255}; // Nested dependency version.
	const auto Decoded = DecodeCookState(Bytes, State);
	EXPECT_FALSE(Decoded);
	EXPECT_EQ(Decoded.Error, ECookStateError::Dependency);
	EXPECT_EQ(Decoded.VirtualPath, "/Game/State");
	ASSERT_TRUE(Decoded.DependencyCause);
	EXPECT_EQ(Decoded.DependencyCause->Error, ECookDependencyCodecError::Header);
	EXPECT_EQ(Decoded.DependencyCause->Version, 255u);
	EXPECT_TRUE(State.Entries.empty());
}

TEST(FCookInputTests, DiscoveryLimitsAndRootClassRetainFirstCause)
{
	(void)MakePackageBytes();
	FCookRequest Request;
	const auto Resolve = [](const FAssetData&, FCookContributorRegistration&) -> FCookContributionResult { return {}; };
	AssetPrivate::FCookDependencyDiscovery Limited(Request, CaptureAssetRegistrySnapshot(), Resolve);
	std::vector<FPackagePath> Roots(MaximumCookDependencyRecords + 1);
	const auto Rejected = Limited.Acquire(Roots, {});
	EXPECT_FALSE(Rejected);
	ASSERT_TRUE(Limited.GetFailureInfo());
	EXPECT_EQ(Limited.GetFailureInfo()->Error, ECookInputError::RootLimit);
	EXPECT_EQ(Limited.GetFailureInfo()->Actual, Roots.size());
	EXPECT_EQ(Limited.GetFailureInfo()->Maximum, MaximumCookDependencyRecords);
	EXPECT_EQ(Limited.GetStatus(), ECookInputStatus::LimitExceeded);
	Request.IsCancelled = [] { return true; };
	EXPECT_EQ(Limited.CheckCancellation().Status, Rejected.Status);
	EXPECT_EQ(Limited.GetFailureInfo()->Error, ECookInputError::RootLimit);
	EXPECT_EQ(Limited.GetStatus(), ECookInputStatus::LimitExceeded);
	Request.IsCancelled = {};
	FPackagePath Target;
	ASSERT_TRUE(FPackagePath::TryCreate("/TestCook/UnknownRoot", Target));
	FAssetReferenceStoreCapture External;
	External.Stores.push_back({.ProviderId = "test-root", .Occurrences = {
		{.TargetPath = Target, .ExpectedClass = "MissingCookRootClass", .bCookRoot = true}}});
	AssetPrivate::FCookDependencyDiscovery UnknownClass(Request, CaptureAssetRegistrySnapshot(), Resolve);
	const auto ClassFailure = UnknownClass.Acquire({}, External);
	External = {};
	EXPECT_EQ(ClassFailure.Status, ECookInputStatus::InvalidDependency);
	ASSERT_TRUE(UnknownClass.GetFailureInfo());
	EXPECT_EQ(UnknownClass.GetFailureInfo()->Error, ECookInputError::RootClass);
	EXPECT_EQ(UnknownClass.GetFailureInfo()->Package, Target);
	EXPECT_EQ(UnknownClass.GetFailureInfo()->Name, "MissingCookRootClass");
	AssetPrivate::FCookDependencyDiscovery Empty(Request, CaptureAssetRegistrySnapshot(), Resolve);
	const auto EmptyFailure = Empty.Acquire({}, {});
	EXPECT_EQ(EmptyFailure.Status, ECookInputStatus::InvalidDependency);
	ASSERT_TRUE(Empty.GetFailureInfo());
	EXPECT_EQ(Empty.GetFailureInfo()->Error, ECookInputError::NoRuntimePackages);
}

TEST(FCookInputTests, MissingReaderClearsOutputAndClassifiesFailure)
{
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FByteBuffer Bytes{std::byte{1}};
	std::string Name = "missing-value";
	const auto Result = Context.ReadDeclaredInput(ECookBuildDependencyKind::ConfigurationValue, Name, Bytes);
	Name.clear();
	EXPECT_FALSE(Result);
	EXPECT_TRUE(Bytes.empty());
	EXPECT_EQ(Result.Status, ECookInputStatus::InvalidDependency);

	FCookInputFailure Io{.Error = ECookInputError::FileIo, .File = "input.bin",
		.FileCause = FFileHelper::FFileIoError{.NativeError = std::make_error_code(std::errc::io_error),
			.Path = "input.bin", .Offset = 42, .Size = 128}};
	const auto Adapted = Io.ToInputResult();
	EXPECT_EQ(Adapted.Status, ECookInputStatus::IoError);
	Io = {};

}

TEST(FCookContributorTests, FamilyRejectionsOwnTargetAndNestedPlanContext)
{
	(void)MakePackageBytes();
	auto* Texture = NewObject<DTexture2D>(nullptr, "ContributionFailure");
	ASSERT_NE(Texture, nullptr);
	FCookContext Context(ECookTargetPlatform::Invalid, ECookTargetProfile::Game);
	std::string Path = "/Game/Rejected";
	auto Rejected = ContributeEngineCookAsset(*Texture, Path, Context);
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Rejected.Error, ECookContributionError::Target);
	EXPECT_EQ(Rejected.TargetPlatform, ECookTargetPlatform::Invalid);
	EXPECT_EQ(Rejected.TargetProfile, ECookTargetProfile::Game);
	EXPECT_EQ(Rejected.ObjectPath, Texture->GetObjectPath());
	Path.clear();
	EXPECT_EQ(Rejected.VirtualPath, "/Game/Rejected");
	Rejected.Error = ECookContributionError::Plan;
	Rejected.PlanCause = FCookPlanError{.Code = ECookPlanError::DuplicatePath,
		.VirtualPath = Rejected.VirtualPath};
	const auto Adapted = Rejected;
	Rejected = {};
	EXPECT_FALSE(Adapted);
	EXPECT_EQ(Adapted.Error, ECookContributionError::Plan);
	EXPECT_NE(FormatCookContributionError(Adapted).find("/Game/Rejected"), std::string::npos);

	EXPECT_TRUE(FCookContributionResult{});

}

TEST(FCookContributorTests, BatchFailureRetainsCauseAndRollsBackOnlyNewHandles)
{
	const FCookContributor Callback = [](DObject&, std::string_view, FCookContext&) -> FCookContributionResult { return {}; };
	const auto Invalid = RegisterCookContributor(nullptr, {"invalid", 1, 1, Callback});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.Error, ECookContributorRegistrationError::InvalidClass);
	EXPECT_EQ(Invalid.ContributorName, "invalid");
	EXPECT_EQ(Invalid.Handle, 0u);
	const auto Existing = RegisterCookContributor(DTextureCube::StaticClass(), {"existing", 1, 1, Callback});
	ASSERT_TRUE(Existing);
	std::vector<FCookContributorHandle> Handles{Existing.Handle};
	const auto Batch = RegisterEngineCookContributors(Handles);
	const auto Replacement = RegisterCookContributor(DTexture2D::StaticClass(), {"replacement", 1, 1, Callback});
	const auto StillRegistered = RegisterCookContributor(DTextureCube::StaticClass(), {"duplicate", 1, 1, Callback});
	for (const auto Handle : Handles) UnregisterCookContributor(Handle);
	UnregisterCookContributor(Replacement.Handle);
	UnregisterCookContributor(StillRegistered.Handle);
	EXPECT_FALSE(Batch);
	EXPECT_EQ(Batch.Error, ECookContributorRegistrationError::DuplicateClass);
	EXPECT_EQ(Batch.ContributorName, "texture-cube");
	EXPECT_EQ(Batch.ClassName, DTextureCube::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(Handles, std::vector<FCookContributorHandle>{Existing.Handle});
	EXPECT_TRUE(Replacement);
	EXPECT_FALSE(StillRegistered);
	EXPECT_EQ(StillRegistered.Error, ECookContributorRegistrationError::DuplicateClass);
}

TEST(FCookContributorTests, RejectsDuplicatesAndAllowsOwnerRetirement)
{
	const FCookContributor Contributor = [](DObject&, std::string_view,
											FCookContext&) -> FCookContributionResult { return {}; };
	const FCookContributorHandle First = RegisterCookContributor(
		DObject::StaticClass(), {"generic-test", 1, 1, Contributor}
	).Handle;
	ASSERT_NE(First, 0u);
	EXPECT_EQ(RegisterCookContributor(DObject::StaticClass(), {"duplicate", 1, 1, Contributor}).Handle, 0u);
	UnregisterCookContributor(First);
	const FCookContributorHandle Replacement = RegisterCookContributor(
		DObject::StaticClass(), {"replacement", 1, 1, Contributor}
	).Handle;
	EXPECT_NE(Replacement, 0u);
	UnregisterCookContributor(Replacement);
}

TEST(FCookContributorTests, RunPinsRetiredOwnerUntilCancellationReturns)
{
	const FCookContributor Callback = [](DObject&, std::string_view,
		FCookContext&) -> FCookContributionResult { return {}; };
	auto Owner = std::make_shared<int>(42);
	std::weak_ptr<int> WeakOwner = Owner;
	const auto Handle = RegisterCookContributor(DObject::StaticClass(),
		{"pinned-test", 1, 1, Callback, {}, std::move(Owner)}).Handle;
	ASSERT_NE(Handle, 0u);
	FCookRequest Request;
	Request.TargetPlatform = ECookTargetPlatform::Win64;
	Request.TargetProfile = ECookTargetProfile::Game;
	Request.bDryRun = true;
	Request.IsCancelled = [&] {
		UnregisterCookContributor(Handle);
		EXPECT_FALSE(WeakOwner.expired());
		const auto Replacement = RegisterCookContributor(DObject::StaticClass(),
			{"replacement-test", 1, 1, Callback}).Handle;
		EXPECT_NE(Replacement, 0u);
		UnregisterCookContributor(Replacement);
		return true;
	};
	FCookRunResult Result;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Result.Status, ECookRunStatus::Cancelled);
	EXPECT_TRUE(WeakOwner.expired());
	UnregisterCookContributor(Handle);
}

TEST(FCookContributorTests, OwnerDestructionCanReenterRegistration)
{
	const FCookContributor Callback = [](DObject&, std::string_view,
		FCookContext&) -> FCookContributionResult { return {}; };
	bool Destroyed = false;
	auto Owner = std::shared_ptr<void>(new int(42), [&](void* Value) {
		delete static_cast<int*>(Value);
		const auto Replacement = RegisterCookContributor(DObject::StaticClass(),
			{"destructor-test", 1, 1, Callback}).Handle;
		EXPECT_NE(Replacement, 0u);
		UnregisterCookContributor(Replacement);
		Destroyed = true;
	});
	const auto Handle = RegisterCookContributor(DObject::StaticClass(),
		{"owner-test", 1, 1, Callback, {}, std::move(Owner)}).Handle;
	ASSERT_NE(Handle, 0u);
	EXPECT_FALSE(Destroyed);
	UnregisterCookContributor(Handle);
	EXPECT_TRUE(Destroyed);
}

TEST(FCookContributorTests, CallbackDestructionPrecedesOwnerReleaseEvenWhenRejected)
{
	int DestroyedCallbacks = 0;
	auto MakeRegistration = [&] {
		auto Owner = std::make_shared<int>(42);
		std::weak_ptr<int> WeakOwner = Owner;
		auto CallbackState = std::shared_ptr<void>(new int(7),
			[&, WeakOwner](void* Value) {
				EXPECT_FALSE(WeakOwner.expired());
				++DestroyedCallbacks;
				delete static_cast<int*>(Value);
			});
		return FCookContributorRegistration{"ordered-test", 1, 1,
			[State = std::move(CallbackState)](DObject&, std::string_view,
				FCookContext&) -> FCookContributionResult { return {}; }, {}, std::move(Owner)};
	};
	const auto Handle = RegisterCookContributor(DObject::StaticClass(), MakeRegistration()).Handle;
	ASSERT_NE(Handle, 0u);
	EXPECT_EQ(DestroyedCallbacks, 0);
	EXPECT_EQ(RegisterCookContributor(DObject::StaticClass(), MakeRegistration()).Handle, 0u);
	EXPECT_EQ(DestroyedCallbacks, 1);
	UnregisterCookContributor(Handle);
	EXPECT_EQ(DestroyedCallbacks, 2);
}

TEST(FCookContributorTests, FamilyCookHelpersAreNotPublicApi)
{
	EXPECT_FALSE(HasPublicAddToCook<DTexture2D>);
	EXPECT_FALSE(HasPublicAddToCook<DTextureCube>);
	EXPECT_FALSE(HasPublicAddToCook<DVolumeTexture>);
	EXPECT_FALSE(HasPublicAddToCook<DStaticMesh>);
	EXPECT_FALSE(HasPublicAddToCook<DMaterial>);
}

TEST(FCookSavePlanTests, CapturesWithoutAnOutputRootAndIsDeterministic)
{
	std::string Error;
	FCookContext First(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookContext Second(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const Durin::FByteBuffer PackageBytes = MakePackageBytes();
	ASSERT_TRUE(First.AddRawPackage("/Game/Detached", PackageBytes, {std::byte{1}, std::byte{2}, std::byte{3}})) << Error;
	ASSERT_TRUE(Second.AddRawPackage("/Game/Detached", PackageBytes, {std::byte{1}, std::byte{2}, std::byte{3}})) << Error;
	std::vector<FCookSavePlan> FirstPlans, SecondPlans;
	ASSERT_TRUE(First.TakeSavePlans(FirstPlans)) << Error;
	ASSERT_TRUE(Second.TakeSavePlans(SecondPlans)) << Error;
	EXPECT_EQ(FirstPlans, SecondPlans);
	ASSERT_EQ(FirstPlans.size(), 1u);
	EXPECT_EQ(FirstPlans[0].VirtualPath, "/Game/Detached");
	EXPECT_FALSE(FirstPlans[0].PackageDigest.IsZero());
	EXPECT_FALSE(FirstPlans[0].SegmentDigest.IsZero());
}

TEST(FCookDependencyTests, RetainedByteLimitAccumulatesAndRejectsOverflow)
{
	constexpr uint64 Limit = 1024ull * 1024 * 1024;
	uint64 Retained = 0;
	EXPECT_TRUE(AssetPrivate::TryRetainCookBytes(Limit / 2, Retained, Limit));
	EXPECT_TRUE(AssetPrivate::TryRetainCookBytes(Limit / 2 - 1, Retained, Limit));
	EXPECT_FALSE(AssetPrivate::TryRetainCookBytes(2, Retained, Limit));
	EXPECT_EQ(Retained, Limit - 1);
	EXPECT_TRUE(AssetPrivate::TryRetainCookBytes(1, Retained, Limit));
	EXPECT_FALSE(AssetPrivate::TryRetainCookBytes(std::numeric_limits<uint64>::max(), Retained, Limit));
	EXPECT_EQ(Retained, Limit);
}

TEST(FCookSavePlanTests, TransfersBuffersAndConsumesPendingPackages)
{
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Context.AddRawPackage("/Game/Transfer", MakePackageBytes(),
		FByteBuffer(1024, std::byte{7}))) << Error;
	const auto* PackageData = Context.GetSavePlans()[0].PackageBytes.data();
	const auto* BulkData = Context.GetSavePlans()[0].BulkBytes.data();
	std::vector<FCookSavePlan> Plans;
	ASSERT_TRUE(Context.TakeSavePlans(Plans)) << Error;
	ASSERT_EQ(Plans.size(), 1u);
	EXPECT_EQ(Plans[0].PackageBytes.data(), PackageData);
	EXPECT_EQ(Plans[0].BulkBytes.data(), BulkData);
	EXPECT_TRUE(Context.GetSavePlans().empty());
	ASSERT_TRUE(Context.TakeSavePlans(Plans)) << Error;
	EXPECT_TRUE(Plans.empty());
}

TEST(FCookSavePlanTests, AdmissionRetainsTypedRejectionWithoutPublishing)
{
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const auto Invalid = Context.AddPackage("relative", MakePackageBytes());
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.Error.Code, ECookPlanError::SourceIdentity);
	EXPECT_EQ(Invalid.Error.VirtualPath, "relative");
	const auto Empty = Context.AddPackage("/Game/Empty", FByteBuffer{});
	EXPECT_FALSE(Empty);
	EXPECT_EQ(Empty.Error.Code, ECookPlanError::EmptyPackage);
	const auto Missing = Context.AddPackage("/Game/Missing", static_cast<DPackage*>(nullptr));
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Error.Code, ECookPlanError::InvalidPackage);
	EXPECT_TRUE(Context.GetSavePlans().empty());
	ASSERT_TRUE(Context.AddPackage("/Game/Valid", MakePackageBytes()));
	const auto Duplicate = Context.AddPackage("/Game/Valid", MakePackageBytes());
	EXPECT_FALSE(Duplicate);
	EXPECT_EQ(Duplicate.Error.Code, ECookPlanError::DuplicatePath);
	EXPECT_EQ(Duplicate.Error.VirtualPath, "/Game/Valid");
	EXPECT_EQ(Context.GetSavePlans().size(), 1u);
}

TEST(FCookSavePlanTests, RawAdmissionRetainsTypedRejectionWithoutPublishing)
{
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const auto Invalid = Context.AddRawPackage("relative", MakePackageBytes(), {std::byte{1}});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.Error.Code, ECookPlanError::Path);
	EXPECT_EQ(Invalid.Error.VirtualPath, "relative");
	const auto EmptyPackage = Context.AddRawPackage("/Game/Empty", {}, {std::byte{1}});
	EXPECT_FALSE(EmptyPackage);
	EXPECT_EQ(EmptyPackage.Error.Code, ECookPlanError::EmptyRawPackage);
	EXPECT_EQ(EmptyPackage.Error.PackageBytes, 0u);
	EXPECT_EQ(EmptyPackage.Error.SegmentBytes, 1u);
	const auto Bytes = MakePackageBytes();
	const auto EmptySegment = Context.AddRawPackage("/Game/Empty", Bytes, {});
	EXPECT_FALSE(EmptySegment);
	EXPECT_EQ(EmptySegment.Error.Code, ECookPlanError::EmptyRawPackage);
	EXPECT_EQ(EmptySegment.Error.PackageBytes, Bytes.size());
	EXPECT_EQ(EmptySegment.Error.SegmentBytes, 0u);
	EXPECT_TRUE(Context.GetSavePlans().empty());
	ASSERT_TRUE(Context.AddRawPackage("/Game/Valid", Bytes, {std::byte{1}}));
	const auto Duplicate = Context.AddRawPackage("/Game/Valid", Bytes, {std::byte{2}});
	EXPECT_FALSE(Duplicate);
	EXPECT_EQ(Duplicate.Error.Code, ECookPlanError::DuplicatePath);
	EXPECT_EQ(Duplicate.Error.VirtualPath, "/Game/Valid");
	ASSERT_EQ(Context.GetSavePlans().size(), 1u);
	EXPECT_EQ(Context.GetSavePlans()[0].BulkBytes[0], std::byte{1});
}

TEST(FCookSavePlanTests, PublicationRetainsFinalizationAndStoreCauses)
{
	FCookContext Invalid(ECookTargetPlatform::Invalid, ECookTargetProfile::Game);
	ASSERT_TRUE(Invalid.AddRawPackage("/Game/Good", MakePackageBytes(), {std::byte{7}}));
	const auto Finalization = PublishCookContext(Invalid, "relative-output");
	EXPECT_FALSE(Finalization);
	EXPECT_EQ(Finalization.Error, ECookContextPublishError::Finalization);
	EXPECT_EQ(Finalization.OutputRoot, std::filesystem::path("relative-output"));
	ASSERT_TRUE(Finalization.PlanCause);
	EXPECT_EQ(Finalization.PlanCause->Code, ECookPlanError::Target);
	EXPECT_FALSE(Finalization.PublicationCause);
	EXPECT_TRUE(Invalid.GetSavePlans().empty());
	FCookContext Valid(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const auto Publication = PublishCookContext(Valid, "relative-output");
	EXPECT_FALSE(Publication);
	EXPECT_EQ(Publication.Error, ECookContextPublishError::Publication);
	EXPECT_EQ(Publication.OutputRoot, std::filesystem::path("relative-output"));
	EXPECT_FALSE(Publication.PlanCause);
	ASSERT_TRUE(Publication.PublicationCause);
	EXPECT_EQ(Publication.PublicationCause->Status, ECookPublishStatus::Failed);
}

TEST(FCookSavePlanTests, InvalidTargetConsumesPlansAndRetainsTarget)
{
	FCookContext Context(ECookTargetPlatform::Invalid, ECookTargetProfile::Game);
	ASSERT_TRUE(Context.AddRawPackage("/Game/Good", MakePackageBytes(), {std::byte{7}}));
	std::vector<FCookSavePlan> Plans(1);
	const auto Result = Context.TakeSavePlans(Plans);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error.Code, ECookPlanError::Target);
	EXPECT_EQ(Result.Error.TargetPlatform, ECookTargetPlatform::Invalid);
	EXPECT_EQ(Result.Error.TargetProfile, ECookTargetProfile::Game);
	EXPECT_TRUE(Plans.empty());
	EXPECT_TRUE(Context.GetSavePlans().empty());
}

TEST(FCookSavePlanTests, FailedFinalizationReturnsNoPartialPlans)
{
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Context.AddRawPackage("/Game/Good", MakePackageBytes(), {std::byte{7}}));
	ASSERT_TRUE(Context.AddPackage("/Game/Corrupt", {std::byte{0}}));
	std::vector<FCookSavePlan> Plans(1);
	const auto Result = Context.TakeSavePlans(Plans);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error.Code, ECookPlanError::Canonicalization);
	EXPECT_EQ(Result.Error.VirtualPath, "/Game/Corrupt");
	ASSERT_FALSE(Result.Error.CanonicalizationDiagnostic.empty());
	EXPECT_FALSE(Result.Error.CanonicalizationDiagnostic.empty());
	EXPECT_TRUE(Plans.empty());
	EXPECT_TRUE(Context.GetSavePlans().empty());
}

TEST(FCookOutputStoreTests, InjectedFailureRetainsBoundedExternalContext)
{
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookInjectedCause");
	Testing::RemoveTestWorkDirectory(Root);
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game};
	FCookRunResult Run;
	const auto Result = Store->Publish({}, State, Run, {},
		[](ECookOperationStage Stage, size_t, std::string& Text) {
			if (Stage != ECookOperationStage::WriterLock) return false;
			Text.assign(4096, 'x');
			return true;
		});
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error, ECookPublishError::Injected);
	ASSERT_TRUE(Result.InjectionCause);
	EXPECT_EQ(Result.InjectionCause->Stage, ECookOperationStage::WriterLock);
	EXPECT_EQ(Result.InjectionCause->Index, 0u);
	EXPECT_EQ(Result.InjectionCause->ExternalDiagnostic.size(), 2048u);
	EXPECT_EQ(FormatCookPublishError(Result), std::string(2048, 'x'));
	EXPECT_FALSE(std::filesystem::exists(Root / "CookManifest.bin"));
	Testing::RemoveTestWorkDirectory(Root);
}

TEST(FCookOutputStoreTests, ValidationFailuresRetainIdentityWithoutCreatingOutput)
{
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookValidationCause");
	Testing::RemoveTestWorkDirectory(Root);
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Run;
	FCookState State{ECookTargetPlatform::Invalid, ECookTargetProfile::Game};
	const auto Request = Store->Publish({}, State, Run, {}, {});
	EXPECT_FALSE(Request);
	ASSERT_TRUE(Request.ValidationCause);
	EXPECT_EQ(Request.ValidationCause->Error, ECookPublishValidationError::Request);
	EXPECT_EQ(Request.ValidationCause->OutputRoot, Root);
	EXPECT_EQ(Request.ValidationCause->TargetPlatform, ECookTargetPlatform::Invalid);
	State.TargetPlatform = ECookTargetPlatform::Win64;
	std::vector<FCookSavePlan> Plans(1);
	Plans[0].VirtualPath = "/Game/Invalid";
	const auto Plan = Store->Publish(Plans, State, Run, {}, {});
	Plans.clear();
	EXPECT_FALSE(Plan);
	ASSERT_TRUE(Plan.ValidationCause);
	EXPECT_EQ(Plan.ValidationCause->Error, ECookPublishValidationError::Plan);
	EXPECT_EQ(Plan.ValidationCause->Path, "/Game/Invalid");
	std::vector<FCookAuxiliaryOutput> Auxiliary(1);
	Auxiliary[0].RelativePath = "../outside";
	const auto Output = Store->Publish({}, Auxiliary, State, Run, {}, {});
	EXPECT_FALSE(Output);
	ASSERT_TRUE(Output.ValidationCause);
	EXPECT_EQ(Output.ValidationCause->Error, ECookPublishValidationError::AuxiliaryOutput);
	EXPECT_EQ(Output.ValidationCause->Path, "../outside");
	EXPECT_FALSE(std::filesystem::exists(Root));
}

TEST(FCookOutputStoreTests, RetainsStateCodecCauseWithoutPublishing)
{
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CookStateCause");
	Testing::RemoveTestWorkDirectory(Root);
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game,
		{{"/Game/Bad", {}, {}, {}, 1, 0, 1, 1, "", "captured"}}};
	FCookRunResult Run;
	const auto Rejected = Store->Publish({}, State, Run, {}, {});
	State = {};
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Rejected.Status, ECookPublishStatus::Failed);
	ASSERT_TRUE(Rejected.StateCause);
	EXPECT_EQ(Rejected.StateCause->Error, ECookStateError::String);
	EXPECT_EQ(Rejected.StateCause->VirtualPath, "/Game/Bad");
	EXPECT_FALSE(std::filesystem::exists(Root / "CookManifest.bin"));
	EXPECT_FALSE(std::filesystem::exists(Root / ".durin-cook-writer"));
	Testing::RemoveTestWorkDirectory(Root);
}

TEST(FCookOutputStoreTests, RestoresEveryPriorFileAfterMidCommitFailure)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookTransactionalRollback"
	);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::string Error;
	auto Capture = [&](std::initializer_list<std::byte> Segment) {
		FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		EXPECT_TRUE(Context.AddRawPackage("/Game/Transactional", MakePackageBytes(), Durin::FByteBuffer(Segment))) << Error;
		std::vector<FCookSavePlan> Plans;
		EXPECT_TRUE(Context.TakeSavePlans(Plans)) << Error;
		Plans[0].Contributor = "opaque-test";
		Plans[0].BuildProvenance = "captured";
		return Plans;
	};
	auto MakeState = [](const FCookSavePlan& Plan) {
		return FCookState{ECookTargetPlatform::Win64, ECookTargetProfile::Game, {{Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance}}};
	};
	std::vector<FCookSavePlan> First = Capture({std::byte{1}, std::byte{2}});
	std::vector<FCookAuxiliaryOutput> FirstAuxiliary{{
		ECookManifestEntryKind::ShaderLibrary, "Shaders/ShaderLibrary.dslb",
		{std::byte{1}, std::byte{3}}}};
	FirstAuxiliary[0].Digest = FXxHash128::HashBuffer(FirstAuxiliary[0].Bytes);
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	FCookPublishResult PublishResult = Store->Publish(
		First, FirstAuxiliary, MakeState(First[0]), Result, {}, {});
	ASSERT_TRUE(PublishResult) << FormatCookPublishError(PublishResult);
	Durin::FByteBuffer PriorPackage, PriorSegment, PriorLibrary, PriorManifest, PriorState;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorPackage, Root / "Game/Transactional.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorSegment, Root / "Game/Transactional.dbulk"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorLibrary, Root / "Shaders/ShaderLibrary.dslb"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorManifest, Root / "CookManifest.bin"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorState, Root / "CookState.bin"));

	std::vector<FCookSavePlan> Second = Capture(
		{std::byte{9}, std::byte{8}, std::byte{7}}
	);
	std::vector<FCookAuxiliaryOutput> SecondAuxiliary{{
		ECookManifestEntryKind::ShaderLibrary, "Shaders/ShaderLibrary.dslb",
		{std::byte{9}, std::byte{7}, std::byte{5}}}};
	SecondAuxiliary[0].Digest = FXxHash128::HashBuffer(SecondAuxiliary[0].Bytes);
	for (const ECookOperationStage FailureStage : {
			 ECookOperationStage::CommitPackage,
			 ECookOperationStage::CommitAuxiliary,
			 ECookOperationStage::CommitState,
			 ECookOperationStage::CommitManifest
		 })
	{
		PublishResult = Store->Publish(Second, SecondAuxiliary, MakeState(Second[0]), Result, {}, [FailureStage](ECookOperationStage Stage, size_t, std::string& OutError) {
				if (Stage != FailureStage) return false;
				OutError = "injected commit failure";
				return true; });
		EXPECT_FALSE(PublishResult);
		EXPECT_EQ(PublishResult.Status, ECookPublishStatus::Failed);
		Durin::FByteBuffer Bytes;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dasset"));
		EXPECT_EQ(Bytes, PriorPackage);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dbulk"));
		EXPECT_EQ(Bytes, PriorSegment);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Shaders/ShaderLibrary.dslb"));
		EXPECT_EQ(Bytes, PriorLibrary);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookManifest.bin"));
		EXPECT_EQ(Bytes, PriorManifest);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookState.bin"));
		EXPECT_EQ(Bytes, PriorState);
	}

	bool bCancelled = false;
	PublishResult = Store->Publish(Second, SecondAuxiliary, MakeState(Second[0]), Result, [&bCancelled] { return bCancelled; }, [&bCancelled](ECookOperationStage Stage, size_t, std::string&) {
			if (Stage == ECookOperationStage::CommitSegment) bCancelled = true;
			return false; });
	EXPECT_FALSE(PublishResult);
	EXPECT_EQ(PublishResult.Status, ECookPublishStatus::Cancelled);
	ASSERT_TRUE(PublishResult.OperationCause);
	EXPECT_EQ(PublishResult.OperationCause->Error, ECookPublishOperationError::CancelledCommit);
	EXPECT_EQ(PublishResult.OperationCause->Stage, ECookOperationStage::CommitPackage);
	EXPECT_EQ(PublishResult.OperationCause->Path, Root / "Game/Transactional.dasset");
	Durin::FByteBuffer Bytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dasset"));
	EXPECT_EQ(Bytes, PriorPackage);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dbulk"));
	EXPECT_EQ(Bytes, PriorSegment);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Shaders/ShaderLibrary.dslb"));
	EXPECT_EQ(Bytes, PriorLibrary);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookManifest.bin"));
	EXPECT_EQ(Bytes, PriorManifest);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookState.bin"));
	EXPECT_EQ(Bytes, PriorState);
}

TEST(FCookOutputStoreTests, RepairsCorruptReusedOutputAndRejectsCompetingWriter)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookRepairAndLock"
	);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Context.AddRawPackage("/Game/Repair", MakePackageBytes(), {std::byte{4}, std::byte{5}, std::byte{6}})) << Error;
	std::vector<FCookSavePlan> Plans;
	ASSERT_TRUE(Context.TakeSavePlans(Plans)) << Error;
	Plans[0].Contributor = "repair-test";
	Plans[0].BuildProvenance = "captured";
	FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game, {{Plans[0].VirtualPath, Plans[0].InputFingerprint, Plans[0].PackageDigest, Plans[0].SegmentDigest, Plans[0].PackageFileSize, Plans[0].SegmentFileSize, Plans[0].ContributorVersion, Plans[0].FamilyProducerVersion, Plans[0].Contributor, Plans[0].BuildProvenance}}};
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	FCookPublishResult PublishResult = Store->Publish(Plans, State, Result, {}, {});
	ASSERT_TRUE(PublishResult) << FormatCookPublishError(PublishResult);
	const std::array<std::byte, 1> Corrupt{std::byte{0}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, Root / "Game/Repair.dbulk"));
	Plans[0].bReuseExistingOutput = true;
	PublishResult = Store->Publish(Plans, State, Result, {}, {});
	ASSERT_TRUE(PublishResult) << FormatCookPublishError(PublishResult);
	Durin::FByteBuffer Repaired;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Repaired, Root / "Game/Repair.dbulk"));
	EXPECT_EQ(Repaired, Plans[0].BulkBytes);

	ASSERT_TRUE(std::filesystem::create_directory(Root / ".durin-cook-writer"));
	PublishResult = Store->Publish(Plans, State, Result, {}, {});
	EXPECT_FALSE(PublishResult);
	EXPECT_EQ(PublishResult.Status, ECookPublishStatus::Failed);
	ASSERT_TRUE(PublishResult.OperationCause);
	EXPECT_EQ(PublishResult.OperationCause->Error, ECookPublishOperationError::WriterLock);
	EXPECT_EQ(PublishResult.OperationCause->Stage, ECookOperationStage::WriterLock);
	EXPECT_EQ(PublishResult.OperationCause->Path, Root / ".durin-cook-writer");
	std::filesystem::remove(Root / ".durin-cook-writer");
}

TEST(FCookOutputStoreTests, CleansOnlyPreviousManifestOwnedStaleFiles)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookStaleCleanup"
	);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	auto Capture = [](std::string Path, std::byte Value) {
		FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		std::string Error;
		EXPECT_TRUE(Context.AddRawPackage(std::move(Path), MakePackageBytes(), {Value})) << Error;
		std::vector<FCookSavePlan> Plans;
		EXPECT_TRUE(Context.TakeSavePlans(Plans)) << Error;
		Plans[0].Contributor = "stale-test";
		Plans[0].BuildProvenance = "captured";
		return Plans[0];
	};
	FCookSavePlan Keep = Capture("/Game/Keep", std::byte{1});
	FCookSavePlan Stale = Capture("/Game/Stale", std::byte{2});
	auto StateFor = [](std::span<const FCookSavePlan> Plans) {
		FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game};
		for (const FCookSavePlan& Plan : Plans)
			State.Entries.push_back({Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance});
		return State;
	};
	std::vector<FCookSavePlan> First{Keep, Stale};
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	FCookPublishResult PublishResult = Store->Publish(First, StateFor(First), Result, {}, {});
	ASSERT_TRUE(PublishResult) << FormatCookPublishError(PublishResult);
	const std::array<std::byte, 1> UnownedBytes{std::byte{9}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(UnownedBytes, Root / "unowned.bin"));
	std::vector<FCookSavePlan> Second{Keep};
	PublishResult = Store->Publish(Second, StateFor(Second), Result, {}, {});
	ASSERT_TRUE(PublishResult) << FormatCookPublishError(PublishResult);
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Stale.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Stale.dbulk"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Game/Keep.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "unowned.bin"));
}

TEST(FCookDependencyTests, AssetAdapterPreservesFailureClassification)
{
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreateProjectContent("/Game/Cause", Path));
	FCookBuildDependencyGraph Graph;
	std::vector<FCookPackageBuildInputs> Inputs{{Path, {
		{ECookBuildDependencyKind::SourcePackage, "source", {}},
		{ECookBuildDependencyKind::SourcePackage, "source", {}}}, {}}};
	auto Rejected = Graph.Initialize(Inputs);
	ASSERT_FALSE(Rejected);
	const auto AssetResult = Rejected.ToInputResult();
	Rejected = {};
	Inputs.clear();
	EXPECT_FALSE(AssetResult);
	EXPECT_EQ(AssetResult.Status, ECookInputStatus::InvalidDependency);
	EXPECT_FALSE(AssetResult.Message.empty());

	const auto Success = FCookDependencyGraphResult{}.ToInputResult();
	EXPECT_TRUE(Success);

}

TEST(FCookDependencyTests, CodecFailuresRetainIdentityAndClearOutputs)
{
	std::vector<FCookBuildDependency> Records{{ECookBuildDependencyKind::ExternalFile, "source", {std::byte{1}}}};
	Records.push_back(Records.front());
	FByteBuffer Bytes{std::byte{7}};
	const auto Duplicate = EncodeCookBuildDependencies(Records, Bytes);
	Records.clear();
	EXPECT_FALSE(Duplicate);
	EXPECT_EQ(Duplicate.Error, ECookDependencyCodecError::Duplicate);
	EXPECT_EQ(Duplicate.Kind, ECookBuildDependencyKind::ExternalFile);
	EXPECT_EQ(Duplicate.LogicalName, "source");
	EXPECT_EQ(Duplicate.RecordIndex, 1u);
	EXPECT_TRUE(Bytes.empty());
	Records.push_back({ECookBuildDependencyKind::ExternalFile, "valid", {}});
	ASSERT_TRUE(EncodeCookBuildDependencies(Records, Bytes));
	Bytes.push_back(std::byte{0});
	const auto Trailing = DecodeCookBuildDependencies(Bytes, Records);
	EXPECT_FALSE(Trailing);
	EXPECT_EQ(Trailing.Error, ECookDependencyCodecError::TrailingBytes);
	EXPECT_EQ(Trailing.RemainingBytes, 1u);
	EXPECT_TRUE(Records.empty());
}

TEST(FCookDependencyTests, CanonicalFramingAndBoundedDecoding)
{
	using K = ECookBuildDependencyKind;
	std::vector<FCookBuildDependency> Records = {
		{K::ConfigurationValue, "bc", {std::byte{'a'}}},
		{K::ExternalFile, "source", {std::byte{1}, std::byte{2}}}};
	FByteBuffer First, Second;
	ASSERT_TRUE(EncodeCookBuildDependencies(Records, First));
	std::ranges::reverse(Records);
	ASSERT_TRUE(EncodeCookBuildDependencies(Records, Second));
	EXPECT_EQ(First, Second);
	std::vector<FCookBuildDependency> Decoded;
	ASSERT_TRUE(DecodeCookBuildDependencies(First, Decoded));
	ASSERT_EQ(Decoded.size(), 2u);
	EXPECT_EQ(Decoded, Records);
	FXxHash128 Before, After;
	ASSERT_TRUE(FingerprintCookBuildDependencies(Records, Before));
	Records.back().LogicalName = "b";
	Records.back().Value = {std::byte{'c'}, std::byte{'a'}};
	ASSERT_TRUE(FingerprintCookBuildDependencies(Records, After));
	EXPECT_NE(Before, After);
	Records.push_back(Records.front());
	EXPECT_FALSE(EncodeCookBuildDependencies(Records, Second));
	EXPECT_TRUE(Second.empty());
	for (size_t Size = 0; Size < First.size(); ++Size)
	{
		EXPECT_FALSE(DecodeCookBuildDependencies(FByteView(First).first(Size), Decoded));
		EXPECT_TRUE(Decoded.empty());
	}
	auto Bad = First;
	Bad.push_back(std::byte{0});
	EXPECT_FALSE(DecodeCookBuildDependencies(Bad, Decoded));
	Bad = First; Bad[8] = std::byte{255};
	EXPECT_FALSE(DecodeCookBuildDependencies(Bad, Decoded));
	Bad = First; Bad[4] = std::byte{255};
	EXPECT_FALSE(DecodeCookBuildDependencies(Bad, Decoded));
	Records = {{K::ExternalFile, std::string(4097, 'x'), {}}};
	EXPECT_FALSE(EncodeCookBuildDependencies(Records, Second));
	Records = {{K::ConfigurationValue, "oversized", FByteBuffer(MaximumCookDependencyValueBytes + 1)}};
	EXPECT_FALSE(EncodeCookBuildDependencies(Records, Second));
}

TEST(FCookDependencyTests, DirectTransitiveCyclesAndSharedInputValues)
{
	using K = ECookBuildDependencyKind;
	auto Path = [](std::string_view Name) {
		FPackagePath Result;
		EXPECT_TRUE(FPackagePath::TryCreateProjectContent(Name, Result));
		return Result;
	};
	const auto A = Path("/Game/A"), B = Path("/Game/B"), C = Path("/Game/C");
	std::vector<FCookPackageBuildInputs> Graph = {
		{A, {{K::SourcePackage, A.ToString(), {std::byte{1}}}}, {{B, false}}},
		{B, {{K::SourcePackage, B.ToString(), {std::byte{2}}}}, {{C, true}}},
		{C, {{K::SourcePackage, C.ToString(), {std::byte{3}}},
			{K::OwnedBulk, C.ToString(), {std::byte{4}}}}, {{A, true}}}};
	auto Hash = [&] {
		std::vector<FCookBuildDependency> Expanded;
		EXPECT_TRUE(ExpandCookBuildDependencies(A, Graph, Expanded));
		FXxHash128 Result;
		EXPECT_TRUE(FingerprintCookBuildDependencies(Expanded, Result));
		return Result;
	};
	const auto Direct = Hash();
	Graph[2].Inputs.back().Value[0] = std::byte{5};
	EXPECT_EQ(Direct, Hash()); // A directly observes B source/bulk only.
	Graph[0].Packages[0].bTransitive = true;
	const auto Transitive = Hash();
	Graph[2].Inputs.back().Value[0] = std::byte{6};
	EXPECT_NE(Transitive, Hash()); // Cycle terminates; C bulk invalidates A.
	const auto Stable = Hash();
	std::ranges::reverse(Graph);
	EXPECT_EQ(Stable, Hash());
	FCookBuildDependencyGraph Prepared;
	ASSERT_TRUE(Prepared.Initialize(Graph));
	Graph.clear();
	std::vector<FCookBuildDependency> Expanded;
	ASSERT_TRUE(Prepared.Expand(A, Expanded));
	FXxHash128 Retained;
	ASSERT_TRUE(FingerprintCookBuildDependencies(Expanded, Retained));
	EXPECT_EQ(Retained, Stable);
	Graph = {{A, {{K::SourcePackage, A.ToString(), {}}}, {{B, true}, {B, false}}}};
	const auto Declaration = Prepared.Initialize(Graph);
	EXPECT_FALSE(Declaration);
	EXPECT_EQ(Declaration.Error, ECookDependencyGraphError::Declaration);
	EXPECT_EQ(Declaration.Package, A);
	EXPECT_EQ(Declaration.Dependency, B);
	const auto Cleared = Prepared.Expand(A, Expanded);
	EXPECT_FALSE(Cleared);
	EXPECT_EQ(Cleared.Error, ECookDependencyGraphError::MissingPackage);
	EXPECT_TRUE(Expanded.empty());
	Graph.front().Packages.pop_back();
	const auto Missing = ExpandCookBuildDependencies(A, Graph, Expanded);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Error, ECookDependencyGraphError::MissingSource);
	EXPECT_EQ(Missing.Dependency, B);
	EXPECT_TRUE(Expanded.empty());
	Graph.front().Packages.clear();
	Graph.front().Inputs.push_back(Graph.front().Inputs.front());
	const auto Codec = Prepared.Initialize(Graph);
	EXPECT_FALSE(Codec);
	EXPECT_EQ(Codec.Error, ECookDependencyGraphError::Codec);
	ASSERT_TRUE(Codec.CodecCause);
	EXPECT_EQ(Codec.CodecCause->Error, ECookDependencyCodecError::Duplicate);
	EXPECT_EQ(Codec.CodecCause->LogicalName, A.ToString());
	FXxHash128 FailedFingerprint{1, 2};
	const auto Fingerprinted = FingerprintCookBuildDependencies(Graph.front().Inputs, FailedFingerprint);
	EXPECT_FALSE(Fingerprinted);
	EXPECT_EQ(Fingerprinted.Error, ECookDependencyCodecError::Duplicate);
	EXPECT_EQ(FailedFingerprint, FXxHash128{});
}
