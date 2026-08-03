#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/Project.h"
#include "Misc/ProjectHistory.h"
#include "NativeTestSupport.h"
#include "Yaml/Yaml.h"

#if PLATFORM_WINDOWS
	#include <process.h>
#endif

namespace
{
	class FProjectHistoryTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<Durin::uint32> NextId = 0;
			Root = Durin::Testing::GetTestWorkDirectory()
				/ std::format("ProjectHistory-{}", NextId++);
			std::filesystem::create_directories(Root);
		}

		void TearDown() override
		{
			std::error_code Error;
			Durin::Testing::RemoveTestWorkDirectory(Root, Error);
		}

		auto WriteProject(std::string_view DirectoryName, std::string_view Contents) const -> std::string
		{
			const std::filesystem::path Directory = Root / DirectoryName;
			std::filesystem::create_directories(Directory);
			const std::filesystem::path ProjectFile = Directory / std::format("{}.dproject", DirectoryName);
			std::ofstream Stream(ProjectFile, std::ios::binary);
			Stream << Contents;
			return ProjectFile.generic_string();
		}

		auto HistoryFile() const -> std::string { return (Root / "ProjectHistory.yaml").generic_string(); }
		auto LegacyFile() const -> std::string { return (Root / "LevelEditorSession.yaml").generic_string(); }

		std::filesystem::path Root;
	};
}

TEST(FProjectTests, PlatformProcessReportsCurrentProcessId)
{
#if PLATFORM_WINDOWS
	EXPECT_EQ(Durin::FPlatformProcess::CurrentProcessId(), static_cast<Durin::uint32>(::_getpid()));
#endif
}

TEST(FProjectTests, PlatformProcessLaunchFailureIncludesPathAndSystemError)
{
#if PLATFORM_WINDOWS
	constexpr std::string_view MissingExecutable = "Z:/DurinTests/MissingProfiler.exe";
	std::string Error;

	EXPECT_FALSE(Durin::FPlatformProcess::LaunchProcess(MissingExecutable, {}, &Error));
	EXPECT_NE(Error.find(MissingExecutable), std::string::npos);
	EXPECT_NE(Error.find("Windows error"), std::string::npos);
#endif
}

TEST(FProjectTests, LoadsExplicitProjectFile)
{
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = Durin::FPaths::RootDir() + "Sandbox/Sandbox.dproject";
	std::string Error;
	ASSERT_TRUE(Durin::InitializeCurrentProject(Params, &Error)) << Error;
	ASSERT_TRUE(Durin::HasCurrentProject());
	EXPECT_EQ(Durin::GetCurrentProject()->Name, "Sandbox");
	EXPECT_EQ(Durin::GetCurrentProject()->MountRoot, "/Game/");
	EXPECT_EQ(Durin::FPaths::ProjectDir(), Durin::GetCurrentProject()->ProjectDir);
}

TEST(FProjectTests, RejectsMissingProject)
{
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = "Missing.dproject";
	std::string Error;
	EXPECT_FALSE(Durin::InitializeCurrentProject(Params, &Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Durin::HasCurrentProject());
}

TEST(FProjectTests, ExplicitBrowserSkipsRecentProject)
{
	Durin::FProjectInitializationParams Params;
	Params.bOpenProjectBrowser = true;
	std::string Error;
	EXPECT_TRUE(Durin::InitializeCurrentProject(Params, &Error));
	EXPECT_FALSE(Durin::HasCurrentProject());
}

TEST_F(FProjectHistoryTest, ValidatesAdditionalMountDescriptorSchema)
{
	Durin::PathUtilities::FScopedMountRegistryFixture Registry;
	const std::string Valid = WriteProject(
		"Mounted",
		R"({
			"ProjectName":"Mounted",
			"Mounts":[
				{
					"VirtualRoot":"/Plugins/PCG/",
					"Owner":"Extension",
					"Root":"Extensions/PCG",
					"ContentPath":"Content",
					"AutoScan":true,
					"AuthoringWritable":false,
					"Dependencies":["/Engine/"]
				},
				{
					"VirtualRoot":"/Libraries/StudioArt/",
					"Owner":"ExternalSources",
					"Root":"Libraries/StudioArt",
					"ContentPath":".",
					"AutoScan":false,
					"AuthoringWritable":false,
					"Dependencies":["/Engine/"]
				}
			]
		})");
	const std::filesystem::path MountedRoot = std::filesystem::path(Valid).parent_path();
	std::filesystem::create_directories(MountedRoot / "Content");
	std::filesystem::create_directories(MountedRoot / "Extensions/PCG/Content");
	std::filesystem::create_directories(MountedRoot / "Libraries/StudioArt");
	std::ofstream(MountedRoot / "Extensions/PCG/Content/Noise.png") << "noise";
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = Valid;
	std::string Error;
	ASSERT_TRUE(Durin::InitializeCurrentProject(Params, &Error)) << Error;
	ASSERT_NE(Durin::GetCurrentProject(), nullptr);
	EXPECT_EQ(Durin::GetCurrentProject()->MountRoot, "/Game/");
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::PathUtilities::InitDefaultMountPoints(&Error)) << Error;
	EXPECT_TRUE(Durin::PathUtilities::ResolveAssetPath("/Game/Levels/Test"));
	EXPECT_TRUE(Durin::PathUtilities::ResolveAssetPath("/Engine/StaticMeshes/Box"));
	EXPECT_TRUE(Durin::PathUtilities::ResolveSourcePath("/Plugins/PCG/Noise.png"));
	EXPECT_TRUE(Durin::PathUtilities::ResolveAssetPath("/Libraries/StudioArt/Texture"));
	EXPECT_TRUE(Durin::PathUtilities::CheckMountDependency("/Game/Asset", "/Engine/Source"));
	EXPECT_TRUE(Durin::PathUtilities::CheckMountDependency("/Game/Asset", "/Plugins/PCG/Source"));
	EXPECT_EQ(
		Durin::PathUtilities::CheckMountDependency("/Engine/Asset", "/Game/Source").Error,
		Durin::PathUtilities::EMountPathError::ForbiddenDependency);

	const std::array InvalidDescriptors{
		WriteProject(
			"UnknownField",
			R"({"ProjectName":"UnknownField","Mounts":[{
				"VirtualRoot":"/Libraries/Art/","Owner":"ExternalSources",
				"Root":"Art","ContentPath":".","AutoScan":false,"AuthoringWritable":false,
				"Dependencies":["/Engine/"],"Unexpected":true}]})"),
		WriteProject(
			"BuiltInOverride",
			R"({"ProjectName":"BuiltInOverride","Mounts":[{
				"VirtualRoot":"/Engine/","Owner":"Extension",
				"Root":"Plugin","ContentPath":"Content","AutoScan":true,"AuthoringWritable":false,
				"Dependencies":[]}]})"),
		WriteProject(
			"Traversal",
			R"({"ProjectName":"Traversal","Mounts":[{
				"VirtualRoot":"/Libraries/Art/","Owner":"ExternalSources",
				"Root":"../Art","ContentPath":".","AutoScan":false,"AuthoringWritable":false,
				"Dependencies":["/Engine/"]}]})"),
		WriteProject(
			"LegacyDomains",
			R"({"ProjectName":"LegacyDomains","Mounts":[{
				"VirtualRoot":"/Libraries/Art/","Owner":"ExternalSources",
				"Root":"Art","Domains":{"SourceAssets":"."},"SourceWritable":false,
				"Dependencies":["/Engine/"]}]})")};
	for (const std::string& Descriptor : InvalidDescriptors)
	{
		Params.RequestedProjectFile = Descriptor;
		Error.clear();
		EXPECT_FALSE(Durin::InitializeCurrentProject(Params, &Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_FALSE(Durin::HasCurrentProject());
	}
}

TEST_F(FProjectHistoryTest, RecordsNewestFirstDeduplicatesAndCapsHistory)
{
	Durin::FProjectHistory History(HistoryFile());
	ASSERT_TRUE(History.Load());
	for (int32_t Index = 0; Index < 12; ++Index)
	{
		const std::string Name = std::format("Project{}", Index);
		ASSERT_TRUE(History.Record(Name, (Root / Name / std::format("{}.dproject", Name)).generic_string()));
	}
	ASSERT_EQ(History.GetEntries().size(), Durin::FProjectHistory::MaximumRecentProjects);
	EXPECT_EQ(History.GetEntries().front().Name, "Project11");
	EXPECT_EQ(History.GetEntries().back().Name, "Project2");

	ASSERT_TRUE(History.Record("Renamed", History.GetEntries()[4].ProjectFile));
	ASSERT_EQ(History.GetEntries().size(), Durin::FProjectHistory::MaximumRecentProjects);
	EXPECT_EQ(History.GetEntries().front().Name, "Renamed");
}

TEST_F(FProjectHistoryTest, ReloadClassifiesAvailableMissingAndInvalidProjects)
{
	const std::string Valid = WriteProject("Valid", R"({"ProjectName":"Valid"})");
	const std::string InvalidJson = WriteProject("InvalidJson", "not json");
	const std::string MissingName = WriteProject("MissingName", R"({"BaseModules":[]})");
	const std::string Missing = (Root / "Missing" / "Missing.dproject").generic_string();

	Durin::FProjectHistory History(HistoryFile());
	ASSERT_TRUE(History.Load());
	ASSERT_TRUE(History.Record("Missing", Missing));
	ASSERT_TRUE(History.Record("MissingName", MissingName));
	ASSERT_TRUE(History.Record("InvalidJson", InvalidJson));
	ASSERT_TRUE(History.Record("StoredName", Valid));

	Durin::FProjectHistory Reloaded(HistoryFile());
	ASSERT_TRUE(Reloaded.Load());
	const auto& Entries = Reloaded.GetEntries();
	ASSERT_EQ(Entries.size(), 4u);
	EXPECT_EQ(Entries[0].Name, "Valid");
	EXPECT_EQ(Entries[0].Status, Durin::ERecentProjectStatus::Available);
	EXPECT_EQ(Entries[1].Status, Durin::ERecentProjectStatus::Invalid);
	EXPECT_EQ(Entries[2].Status, Durin::ERecentProjectStatus::Invalid);
	EXPECT_EQ(Entries[3].Status, Durin::ERecentProjectStatus::Missing);
}

TEST_F(FProjectHistoryTest, MigratesLegacyRecentProjectOnlyOnceAndPersistsEmptyHistory)
{
	const std::string ProjectFile = WriteProject("Legacy", R"({"ProjectName":"Legacy"})");
	Durin::FYamlDocument LegacyDocument;
	Durin::FYamlNodeRef LegacyRoot = LegacyDocument.GetMutableRoot();
	LegacyRoot.EnsureMap();
	LegacyRoot.SetChildValue("RecentProject", ProjectFile);
	ASSERT_TRUE(LegacyDocument.SaveToFile(LegacyFile()));

	Durin::FProjectHistory History(HistoryFile(), LegacyFile());
	ASSERT_TRUE(History.Load());
	ASSERT_EQ(History.GetEntries().size(), 1u);
	EXPECT_EQ(History.GetEntries().front().Name, "Legacy");
	ASSERT_TRUE(std::filesystem::exists(HistoryFile()));

	ASSERT_TRUE(History.Remove(ProjectFile));
	Durin::FProjectHistory Reloaded(HistoryFile(), LegacyFile());
	ASSERT_TRUE(Reloaded.Load());
	EXPECT_TRUE(Reloaded.GetEntries().empty());
}
