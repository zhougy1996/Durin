#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto NormalizeDirectory(std::string_view Directory) -> std::filesystem::path
	{
		std::filesystem::path Path(Directory);
		if (Path.filename().empty()) Path = Path.parent_path();
		return Path.lexically_normal();
	}

	class FMountRegistryTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<uint32> NextId = 0;
			Root = Durin::Testing::GetTestWorkDirectory()
				/ std::format("MountRegistry-{}", NextId++);
			std::error_code CleanupError;
			Durin::Testing::RemoveTestWorkDirectory(Root, CleanupError);
			std::filesystem::create_directories(Root / "Engine/Content/Textures");
			std::filesystem::create_directories(Root / "Game/Content");
			std::filesystem::create_directories(Root / "PCG/Content");
			std::filesystem::create_directories(Root / "StudioArt");
			std::filesystem::create_directories(Root / "External");
			std::ofstream(Root / "Engine/Content/Textures/Stone.png") << "stone";
			std::ofstream(Root / "StudioArt/Stone.png") << "external stone";
			std::ofstream(Root / "External/Escape.png") << "escape";
		}

		void TearDown() override
		{
			std::error_code Error;
			Durin::Testing::RemoveTestWorkDirectory(Root, Error);
		}

		auto Definitions() const -> std::array<Durin::PathUtilities::FMountPoint, 5>
		{
			using namespace Durin::PathUtilities;
			return {{
				{
					.VirtualRoot = "/Engine/",
					.Owner = EMountOwner::Engine,
					.Root = Root / "Engine",
					.ContentPath = "Content",
					.bAutoScan = true,
					.bContentWritable = true},
				{
					.VirtualRoot = "/Game/",
					.Owner = EMountOwner::ActiveProject,
					.Root = Root / "Game",
					.ContentPath = "Content",
					.bAutoScan = true,
					.bContentWritable = true,
					.Dependencies = {"/Engine/", "/Plugins/PCG/", "/Libraries/StudioArt/"}},
				{
					.VirtualRoot = "/Plugins/PCG/",
					.Owner = EMountOwner::Extension,
					.Root = Root / "PCG",
					.ContentPath = "Content",
					.bAutoScan = true,
					.bContentWritable = false,
					.Dependencies = {"/Engine/"}},
				{
					.VirtualRoot = "/Libraries/StudioArt/",
					.Owner = EMountOwner::ExternalSources,
					.Root = Root / "StudioArt",
					.ContentPath = ".",
					.bContentWritable = false,
					.Dependencies = {"/Engine/"}},
				{
					.VirtualRoot = "/Libraries/Offline/",
					.Owner = EMountOwner::ExternalSources,
					.Root = Root / "Offline",
					.bContentWritable = false,
					.Dependencies = {"/Engine/"}}}};
		}

		std::filesystem::path Root;
	};
}

TEST(FPathsTests, ResolvesDerivedDataProjectFallbackAndTestOverrideRoots)
{
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
	ASSERT_TRUE(Durin::FPaths::SetProjectFile({}));
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()),
		(std::filesystem::path(Durin::FPaths::EngineDir()) / "DerivedDataCache").lexically_normal());

	const std::filesystem::path ProjectDir = Durin::Testing::GetTestWorkDirectory() / "CacheProject";
	std::filesystem::create_directories(ProjectDir);
	const std::filesystem::path ProjectFile = ProjectDir / "CacheProject.dproject";
	{
		std::ofstream Stream(ProjectFile);
		Stream << R"({"ProjectName":"CacheProject"})";
	}
	ASSERT_TRUE(Durin::FPaths::SetProjectFile(ProjectFile.generic_string()));
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()),
		(ProjectDir / "DerivedDataCache").lexically_normal());

	const std::filesystem::path Override = Durin::Testing::GetTestWorkDirectory() / "IsolatedCache";
	Durin::FPaths::SetDerivedDataCacheDirForTests(Override.generic_string());
	EXPECT_EQ(NormalizeDirectory(Durin::FPaths::DerivedDataCacheDir()), Override.lexically_normal());
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FPathsTests, RootAndEngineMountAreWorkspaceRelative)
{
	std::filesystem::path EngineDir = Durin::FPaths::EngineDir();
	if (EngineDir.filename().empty()) EngineDir = EngineDir.parent_path();
	const std::filesystem::path ExpectedRoot = EngineDir.parent_path().lexically_normal();

	EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(Durin::FPaths::RootDir()), ExpectedRoot));

	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	const std::array Definitions{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::PathUtilities::EMountOwner::Engine,
			.Root = EngineDir,
			.ContentPath = "Content",
			.bAutoScan = true,
			.bContentWritable = true}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	const Durin::PathUtilities::FAssetPathResult Result =
		Durin::PathUtilities::ResolveAssetPath("/Engine/StaticMeshes/Test");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.PhysicalPath.lexically_normal(), (EngineDir / "Content/StaticMeshes/Test").lexically_normal());
}

TEST(FPathsTests, ThirdPartyRuntimeBinariesAreSharedByBuildConfiguration)
{
	std::filesystem::path EngineDir = Durin::FPaths::EngineDir();
	if (EngineDir.filename().empty()) EngineDir = EngineDir.parent_path();
	std::filesystem::path ThirdPartyDir =
		std::filesystem::path(Durin::FPaths::EngineThirdPartyRuntimeBinariesDir()).lexically_normal();
	if (ThirdPartyDir.filename().empty()) ThirdPartyDir = ThirdPartyDir.parent_path();

	EXPECT_EQ(ThirdPartyDir.filename(), "ThirdParty");
	EXPECT_EQ(ThirdPartyDir.parent_path().filename(), DURIN_BUILD_CONFIGURATION);
	EXPECT_EQ(ThirdPartyDir.parent_path().parent_path().parent_path(), (EngineDir / "Binaries").lexically_normal());
}

TEST(FPathsTests, LaunchSavedDirectoriesAreGroupedUnderRuntimeSavedDirectory)
{
	const auto NormalizeDirectory = [](std::filesystem::path Directory) {
		if (Directory.filename().empty()) Directory = Directory.parent_path();
		return Directory.lexically_normal();
	};
	const std::filesystem::path LaunchDir = NormalizeDirectory(Durin::FPaths::LaunchDir());
	const std::filesystem::path SavedDir = NormalizeDirectory(Durin::FPaths::LaunchSavedDir());
	const std::filesystem::path ConfigsDir = NormalizeDirectory(Durin::FPaths::LaunchConfigsDir());
	const std::filesystem::path LogsDir = NormalizeDirectory(Durin::FPaths::LaunchLogsDir());

	EXPECT_EQ(SavedDir, (LaunchDir / "Saved").lexically_normal());
	EXPECT_EQ(ConfigsDir, (SavedDir / "Configs").lexically_normal());
	EXPECT_EQ(LogsDir, (SavedDir / "Logs").lexically_normal());
}

TEST(FPathsTests, ExplicitProjectFileControlsProjectDirectoryAndMount)
{
	const std::filesystem::path ProjectDir = Durin::Testing::GetTestWorkDirectory() / "ExternalProject";
	std::filesystem::create_directories(ProjectDir / "Content");
	const std::filesystem::path ProjectFile = ProjectDir / "ExternalGame.dproject";
	{
		std::ofstream Stream(ProjectFile);
		Stream << R"({"ProjectName":"ExternalGame"})";
	}

	std::string Error;
	ASSERT_TRUE(Durin::FPaths::SetProjectFile(ProjectFile.generic_string(), &Error)) << Error;
	EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(Durin::FPaths::ProjectDir()), ProjectDir));

	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	const std::array Definitions{
		Durin::PathUtilities::FMountPoint{
			.VirtualRoot = "/Game/",
			.Owner = Durin::PathUtilities::EMountOwner::ActiveProject,
			.Root = ProjectDir,
			.ContentPath = "Content",
			.bAutoScan = true,
			.bContentWritable = true}};
	Durin::PathUtilities::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	const Durin::PathUtilities::FAssetPathResult Result =
		Durin::PathUtilities::ResolveAssetPath("/Game/Levels/Test");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.PhysicalPath.lexically_normal(), (ProjectDir / "Content/Levels/Test").lexically_normal());
}

TEST_F(FMountRegistryTests, ResolvesTypedPathsClassifiesRootsAndEnforcesPolicy)
{
	using namespace Durin::PathUtilities;
	const auto MountDefinitions = Definitions();
	FScopedMountRegistryFixture Registry(MountDefinitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	const FAssetPathResult EngineContent = ResolveAssetPath("/Engine/StaticMeshes/Box");
	ASSERT_TRUE(EngineContent) << EngineContent.Message;
	EXPECT_EQ(
		EngineContent.PhysicalPath.lexically_normal(),
		(Root / "Engine/Content/StaticMeshes/Box").lexically_normal());

	const FAssetPathResult EngineSource = ResolveAssetPath(
		"/engine/Textures/Stone.png", EPathExistence::RequireFile);
	ASSERT_TRUE(EngineSource) << EngineSource.Message;
	EXPECT_EQ(EngineSource.NormalizedVirtualPath, "/Engine/Textures/Stone.png");
	EXPECT_EQ(EngineSource.PhysicalPath, Root / "Engine/Content/Textures/Stone.png");
	const FAssetPathResult SamePhysicalPath = ResolveAssetPath("/Engine/Textures/Stone.png");
	ASSERT_TRUE(SamePhysicalPath) << SamePhysicalPath.Message;
	EXPECT_EQ(SamePhysicalPath.PhysicalPath, EngineSource.PhysicalPath);

	const FAssetPathResult Classified = ClassifyAssetPath(EngineSource.PhysicalPath);
	ASSERT_TRUE(Classified) << Classified.Message;
	EXPECT_EQ(Classified.NormalizedVirtualPath, "/Engine/Textures/Stone.png");
	const FAssetPathResult ClassifiedGameRoot =
		ClassifyAssetPath(Root / "Game/Content");
	ASSERT_TRUE(ClassifiedGameRoot) << ClassifiedGameRoot.Message;
	EXPECT_EQ(ClassifiedGameRoot.NormalizedVirtualPath, "/Game/");

	EXPECT_TRUE(ResolveAssetPath("/Libraries/StudioArt/Texture"));
	const FAssetPathResult ExternalSource = ResolveAssetPath(
		"/Libraries/StudioArt/Stone.png", EPathExistence::RequireFile);
	ASSERT_TRUE(ExternalSource) << ExternalSource.Message;
	EXPECT_EQ(ExternalSource.PhysicalPath, Root / "StudioArt/Stone.png");
	EXPECT_EQ(
		ResolveAssetPath("/Libraries/Offline/Texture.png", EPathExistence::RequireFile).Error,
		EMountPathError::UnavailableRoot);
	const FAssetPathResult MissingSource = ResolveAssetPath(
		"/Engine/Textures/Missing.png", EPathExistence::RequireFile);
	EXPECT_EQ(MissingSource.Error, EMountPathError::MissingFile) << MissingSource.Message;
	EXPECT_EQ(
		FindMountForVirtualPath("/Game/../Engine/Stone").Error,
		EMountPathError::InvalidRelativePath);

	EXPECT_TRUE(CheckMountDependency("/Game/Asset", "/Engine/Source"));
	EXPECT_TRUE(CheckMountDependency("/Game/Asset", "/Plugins/PCG/Source"));
	EXPECT_EQ(
		CheckMountDependency("/Engine/Asset", "/Game/Source").Error,
		EMountPathError::ForbiddenDependency);

	std::string PublishError;
	EXPECT_FALSE(PublishMountRegistry(MountDefinitions, &PublishError));
	EXPECT_FALSE(PublishError.empty());
}

TEST_F(FMountRegistryTests, RejectsNestedLinkEscapesOverlappingRootsAndAcceptsLinkedRoots)
{
	using namespace Durin::PathUtilities;
	std::error_code Error;
	std::filesystem::create_directory_symlink(
		Root / "External", Root / "Game/Content/Escape", Error);
	if (Error) GTEST_SKIP() << "Directory symlinks are unavailable: " << Error.message();

	auto MountDefinitions = Definitions();
	FScopedMountRegistryFixture Registry(MountDefinitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	EXPECT_EQ(
		ResolveAssetPath("/Game/Escape/Escape.png", EPathExistence::RequireFile).Error,
		EMountPathError::EscapedRoot);

	const std::filesystem::path LinkedRoot = Root / "StudioLink";
	std::filesystem::create_directory_symlink(Root / "StudioArt", LinkedRoot, Error);
	ASSERT_FALSE(Error) << Error.message();
	const std::array LinkedDefinitions{
		FMountPoint{
			.VirtualRoot = "/Linked/",
			.Owner = EMountOwner::ExternalSources,
			.Root = LinkedRoot}};
	FScopedMountRegistryFixture LinkedRegistry(LinkedDefinitions);
	ASSERT_TRUE(LinkedRegistry.IsValid()) << LinkedRegistry.GetError();
	const FAssetPathResult Missing =
		ResolveAssetPath("/Linked/New.png", EPathExistence::AllowMissing);
	ASSERT_TRUE(Missing) << Missing.Message;
	EXPECT_EQ(Missing.PhysicalPath.lexically_normal(), (LinkedRoot / "New.png").lexically_normal());

	const std::array OverlappingDefinitions{
		FMountPoint{.VirtualRoot = "/Outer/", .Root = Root / "Game", .ContentPath = "Content"},
		FMountPoint{.VirtualRoot = "/Inner/", .Root = Root / "Game/Content", .ContentPath = "."}};
	FScopedMountRegistryFixture OverlappingRegistry(OverlappingDefinitions);
	EXPECT_FALSE(OverlappingRegistry.IsValid());
	EXPECT_FALSE(OverlappingRegistry.GetError().empty());
}

TEST_F(FMountRegistryTests, AllowsSharedOwnerRootsWithDistinctContentDirectories)
{
	using namespace Durin::PathUtilities;
	std::filesystem::create_directories(Root / "Shared/ContentA");
	std::filesystem::create_directories(Root / "Shared/ContentB");
	const std::array Definitions{
		FMountPoint{
			.VirtualRoot = "/First/",
			.Root = Root / "Shared",
			.ContentPath = "ContentA"},
		FMountPoint{
			.VirtualRoot = "/Second/",
			.Root = Root / "Shared",
			.ContentPath = "ContentB"}};
	FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	EXPECT_EQ(
		ResolveAssetPath("/First/File.bin", EPathExistence::AllowMissing).PhysicalPath,
		Root / "Shared/ContentA/File.bin");
	EXPECT_EQ(
		ResolveAssetPath("/Second/Asset").PhysicalPath,
		Root / "Shared/ContentB/Asset");
}

TEST_F(FMountRegistryTests, RejectsContentPathTraversal)
{
	using namespace Durin::PathUtilities;
	const std::array Definitions{
		FMountPoint{
			.VirtualRoot = "/Escaped/",
			.Root = Root / "Game",
			.ContentPath = "../External"}};
	FScopedMountRegistryFixture Registry(Definitions);
	EXPECT_FALSE(Registry.IsValid());
	EXPECT_FALSE(Registry.GetError().empty());
}
