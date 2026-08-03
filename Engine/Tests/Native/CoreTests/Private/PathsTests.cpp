#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	class FMountRegistryTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<Durin::uint32> NextId = 0;
			Root = Durin::Testing::GetTestWorkDirectory()
				/ std::format("MountRegistry-{}", NextId++);
			std::error_code CleanupError;
			Durin::Testing::RemoveTestWorkDirectory(Root, CleanupError);
			std::filesystem::create_directories(Root / "Engine/Content/Textures");
			std::filesystem::create_directories(Root / "Game/Content");
			std::filesystem::create_directories(Root / "PCG");
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
					.Root = Root / "Engine/Content",
					.bAssetPackages = true,
					.bAuthoringWritable = true},
				{
					.VirtualRoot = "/Game/",
					.Owner = EMountOwner::ActiveProject,
					.Root = Root / "Game/Content",
					.bAssetPackages = true,
					.bAuthoringWritable = true,
					.Dependencies = {"/Engine/", "/Plugins/PCG/", "/Libraries/StudioArt/"}},
				{
					.VirtualRoot = "/Plugins/PCG/",
					.Owner = EMountOwner::Extension,
					.Root = Root / "PCG",
					.bAssetPackages = true,
					.bAuthoringWritable = false,
					.Dependencies = {"/Engine/"}},
				{
					.VirtualRoot = "/Libraries/StudioArt/",
					.Owner = EMountOwner::ExternalSources,
					.Root = Root / "StudioArt",
					.bAuthoringWritable = false,
					.Dependencies = {"/Engine/"}},
				{
					.VirtualRoot = "/Libraries/Offline/",
					.Owner = EMountOwner::ExternalSources,
					.Root = Root / "Offline",
					.bAuthoringWritable = false,
					.Dependencies = {"/Engine/"}}}};
		}

		std::filesystem::path Root;
	};
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
			.Root = EngineDir / "Content",
			.bAssetPackages = true,
			.bAuthoringWritable = true}};
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

	EXPECT_EQ(ThirdPartyDir.filename(), DURIN_BUILD_CONFIGURATION);
	EXPECT_EQ(ThirdPartyDir.parent_path().filename(), "ThirdParty");
	EXPECT_EQ(ThirdPartyDir.parent_path().parent_path().parent_path(), (EngineDir / "Binaries").lexically_normal());
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
			.Root = ProjectDir / "Content",
			.bAssetPackages = true,
			.bAuthoringWritable = true}};
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

	const FSourcePathResult EngineSource = ResolveSourcePath("/engine/Textures/Stone.png");
	ASSERT_TRUE(EngineSource) << EngineSource.Message;
	EXPECT_EQ(EngineSource.NormalizedVirtualPath, "/Engine/Textures/Stone.png");
	EXPECT_EQ(EngineSource.PhysicalPath, Root / "Engine/Content/Textures/Stone.png");
	const FAssetPathResult SamePhysicalPath = ResolveAssetPath("/Engine/Textures/Stone.png");
	ASSERT_TRUE(SamePhysicalPath) << SamePhysicalPath.Message;
	EXPECT_EQ(SamePhysicalPath.PhysicalPath, EngineSource.PhysicalPath);

	const FSourcePathResult Classified = ClassifySourcePath(EngineSource.PhysicalPath);
	ASSERT_TRUE(Classified) << Classified.Message;
	EXPECT_EQ(Classified.NormalizedVirtualPath, "/Engine/Textures/Stone.png");
	const FAssetPathResult ClassifiedGameRoot =
		ClassifyAssetPath(Root / "Game/Content");
	ASSERT_TRUE(ClassifiedGameRoot) << ClassifiedGameRoot.Message;
	EXPECT_EQ(ClassifiedGameRoot.NormalizedVirtualPath, "/Game/");

	EXPECT_EQ(
		ResolveAssetPath("/Libraries/StudioArt/Texture").Error,
		EMountPathError::AssetPackagesDisabled);
	const FSourcePathResult ExternalSource =
		ResolveSourcePath("/Libraries/StudioArt/Stone.png");
	ASSERT_TRUE(ExternalSource) << ExternalSource.Message;
	EXPECT_EQ(ExternalSource.PhysicalPath, Root / "StudioArt/Stone.png");
	EXPECT_EQ(
		ResolveSourcePath("/Libraries/Offline/Texture.png").Error,
		EMountPathError::UnavailableRoot);
	const FSourcePathResult MissingSource =
		ResolveSourcePath("/Engine/Textures/Missing.png");
	EXPECT_EQ(MissingSource.Error, EMountPathError::MissingFile) << MissingSource.Message;
	EXPECT_EQ(
		FindMountForVirtualPath("/Game/../Engine/Stone").Error,
		EMountPathError::InvalidRelativePath);

	EXPECT_TRUE(CheckMountDependency("/Game/Asset", "/Engine/Source"));
	EXPECT_TRUE(CheckMountDependency("/Game/Asset", "/Plugins/PCG/Source"));
	EXPECT_EQ(
		CheckMountDependency("/Engine/Asset", "/Game/Source").Error,
		EMountPathError::ForbiddenDependency);
	EXPECT_TRUE(CheckAuthoringMutation("/Game/Asset", "/Game/Textures/New.png"));
	EXPECT_EQ(
		CheckAuthoringMutation("/Game/Asset", "/Engine/Textures/Stone.png").Error,
		EMountPathError::ReadOnlyMount);

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
		ResolveSourcePath("/Game/Escape/Escape.png").Error,
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
	const FSourcePathResult Missing =
		ResolveSourcePath("/Linked/New.png", EPathExistence::AllowMissing);
	ASSERT_TRUE(Missing) << Missing.Message;
	EXPECT_EQ(Missing.PhysicalPath.lexically_normal(), (LinkedRoot / "New.png").lexically_normal());

	const std::array OverlappingDefinitions{
		FMountPoint{.VirtualRoot = "/Outer/", .Root = Root / "Game"},
		FMountPoint{.VirtualRoot = "/Inner/", .Root = Root / "Game/Content"}};
	FScopedMountRegistryFixture OverlappingRegistry(OverlappingDefinitions);
	EXPECT_FALSE(OverlappingRegistry.IsValid());
	EXPECT_FALSE(OverlappingRegistry.GetError().empty());
}
