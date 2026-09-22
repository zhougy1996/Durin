#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Project.h"
#include "Misc/ProjectHistory.h"
#include "NativeTestSupport.h"

#if defined(_WIN32)
	#include <process.h>
#elif defined(__APPLE__)
	#include <sys/wait.h>
	#include <unistd.h>
#endif

namespace
{
	class FProjectHistoryTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			static std::atomic<uint32> NextId = 0;
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

		std::filesystem::path Root;
	};
}

TEST(FProjectTests, PlatformProcessReportsCurrentProcessId)
{
#if defined(_WIN32)
	EXPECT_EQ(Durin::FPlatformProcess::CurrentProcessId(), static_cast<uint32>(::_getpid()));
#elif defined(__APPLE__)
	EXPECT_EQ(Durin::FPlatformProcess::CurrentProcessId(), static_cast<uint32>(::getpid()));
#endif
}

TEST(FProjectTests, PlatformProcessReportsExistingExecutable)
{
	const std::filesystem::path Executable = Durin::FPlatformProcess::ExecutablePath();
	EXPECT_FALSE(Executable.empty());
	EXPECT_TRUE(std::filesystem::is_regular_file(Executable));
}

TEST(FProjectTests, PlatformProcessLaunchFailureRetainsPathAndSystemError)
{
#if defined(_WIN32)
	constexpr std::string_view MissingExecutable = "Z:/DurinTests/MissingProfiler.exe";
#elif defined(__APPLE__)
	constexpr std::string_view MissingExecutable = "/DurinTests/MissingProfiler";
#endif
	const auto Result = Durin::FPlatformProcess::LaunchProcess(MissingExecutable, {});
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, Durin::EPlatformProcessError::Launch);
	EXPECT_EQ(Result.error().Path, MissingExecutable);
	ASSERT_TRUE(Result.error().NativeError);
	EXPECT_NE(*Result.error().NativeError, 0);
}

#if defined(_WIN32)
TEST(FProjectTests, PlatformProcessDistinguishesExitCodeFromLaunchFailure)
{
	const char* CommandProcessor = std::getenv("COMSPEC");
	ASSERT_NE(CommandProcessor, nullptr);
	const auto Executed = Durin::FPlatformProcess::ExecuteProcess(CommandProcessor, "/d /c exit 7");
	ASSERT_TRUE(Executed) << Executed.error().ToString();
	EXPECT_EQ(*Executed, 7);
	const auto Failed = Durin::FPlatformProcess::ExecuteProcess("Z:/DurinTests/MissingProcess.exe", {});
	ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.error().Code, Durin::EPlatformProcessError::Launch);
	EXPECT_FALSE(Failed.error().ToString().empty());
}
#endif

#if defined(__APPLE__)
TEST(FProjectTests, PlatformProcessExecutesAndReportsNativeReturnCode)
{
	const auto ExecuteProcessResult = Durin::FPlatformProcess::ExecuteProcess("/bin/sh", "-c 'exit 7'");
	ASSERT_TRUE(ExecuteProcessResult) << ExecuteProcessResult.error().ToString();
	EXPECT_EQ(*ExecuteProcessResult, 7);

	const auto ExecuteProcessResult2 = Durin::FPlatformProcess::ExecuteProcess("/bin/sh", "-c 'unfinished");
	ASSERT_FALSE(ExecuteProcessResult2);
	EXPECT_EQ(ExecuteProcessResult2.error().Code, Durin::EPlatformProcessError::InvalidArguments);
}

TEST(FProjectTests, PlatformProcessWaitsForObservedProcessExit)
{
	const pid_t Child = fork();
	ASSERT_GE(Child, 0);
	if (Child == 0)
	{
		usleep(20000);
		_exit(0);
	}
	const auto WaitForProcessExitResult = Durin::FPlatformProcess::WaitForProcessExit(static_cast<uint32>(Child));
	EXPECT_TRUE(WaitForProcessExitResult) << WaitForProcessExitResult.error().ToString();
	int Status = 0;
	EXPECT_EQ(waitpid(Child, &Status, 0), Child);
}

TEST(FProjectTests, PlatformOpenPathRejectsEmptyAndMissingPathsWithDiagnostics)
{
	const auto OpenPathResult = Durin::FPlatformProcess::OpenPath({});
	ASSERT_FALSE(OpenPathResult);
	EXPECT_EQ(OpenPathResult.error().Code, Durin::EPlatformProcessError::InvalidArguments);
	const auto OpenPathResult2 = Durin::FPlatformProcess::OpenPath("/DurinTests/MissingOpenPath");
	ASSERT_FALSE(OpenPathResult2);
	EXPECT_EQ(OpenPathResult2.error().Code, Durin::EPlatformProcessError::OpenPath);
	EXPECT_EQ(OpenPathResult2.error().Path, "/DurinTests/MissingOpenPath");
	ASSERT_TRUE(OpenPathResult2.error().ExitCode);
	EXPECT_NE(*OpenPathResult2.error().ExitCode, 0);
}
#endif

TEST(FProjectTests, LoadsExplicitProjectFile)
{
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = Durin::FPaths::RootDir() + "Sandbox/Sandbox.dproject";
	std::string Error;
	const auto InitializeCurrentProjectResult = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult ? std::string{} : InitializeCurrentProjectResult.error().ToString();
	ASSERT_TRUE(InitializeCurrentProjectResult.has_value()) << Error;
	ASSERT_TRUE(Durin::HasCurrentProject());
	EXPECT_EQ(Durin::GetCurrentProject()->Name, "Sandbox");
	EXPECT_EQ(Durin::GetCurrentProject()->MountRoot, "/Game/");
	EXPECT_EQ(Durin::FPaths::ProjectDir(), Durin::GetCurrentProject()->ProjectDir);
}

#if defined(__APPLE__)
TEST(FProjectTests, ProjectEditOwnershipIsExclusiveAcrossProcesses)
{
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = Durin::FPaths::RootDir() + "Sandbox/Sandbox.dproject";
	std::string Error;
	const auto InitializeCurrentProjectResult2 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult2 ? std::string{} : InitializeCurrentProjectResult2.error().ToString();
	ASSERT_TRUE(InitializeCurrentProjectResult2.has_value()) << Error;
	const auto AcquireProjectEditOwnershipResult = Durin::AcquireProjectEditOwnership();
	Error = AcquireProjectEditOwnershipResult ? std::string{} : AcquireProjectEditOwnershipResult.error().ToString();
	ASSERT_TRUE(AcquireProjectEditOwnershipResult.has_value()) << Error;
	const auto AcquireProjectEditOwnershipResult2 = Durin::AcquireProjectEditOwnership();
	Error = AcquireProjectEditOwnershipResult2 ? std::string{} : AcquireProjectEditOwnershipResult2.error().ToString();
	EXPECT_TRUE(AcquireProjectEditOwnershipResult2.has_value());

	const pid_t Child = fork();
	ASSERT_GE(Child, 0);
	if (Child == 0)
	{
		std::string ChildError;
		const auto AcquireProjectEditOwnershipResult3 = Durin::AcquireProjectEditOwnership();
		ChildError = AcquireProjectEditOwnershipResult3 ? std::string{} : AcquireProjectEditOwnershipResult3.error().ToString();
		const bool bAcquired = AcquireProjectEditOwnershipResult3.has_value();
		_exit(!bAcquired && ChildError.find("already owns") != std::string::npos ? 0 : 1);
	}
	int Status = 0;
	ASSERT_EQ(waitpid(Child, &Status, 0), Child);
	EXPECT_TRUE(WIFEXITED(Status));
	EXPECT_EQ(WEXITSTATUS(Status), 0);

	Durin::ReleaseProjectEditOwnership();
	const auto AcquireProjectEditOwnershipResult4 = Durin::AcquireProjectEditOwnership();
	Error = AcquireProjectEditOwnershipResult4 ? std::string{} : AcquireProjectEditOwnershipResult4.error().ToString();
	EXPECT_TRUE(AcquireProjectEditOwnershipResult4.has_value()) << Error;
	Durin::ReleaseProjectEditOwnership();
}
#endif

TEST(FProjectTests, RejectsMissingProject)
{
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = "Missing.dproject";
	std::string Error;
	const auto InitializeCurrentProjectResult3 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult3 ? std::string{} : InitializeCurrentProjectResult3.error().ToString();
	EXPECT_FALSE(InitializeCurrentProjectResult3.has_value());
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Durin::HasCurrentProject());
}

TEST(FProjectTests, ExplicitBrowserSkipsRecentProject)
{
	Durin::FProjectInitializationParams Params;
	Params.bOpenProjectBrowser = true;
	std::string Error;
	const auto InitializeCurrentProjectResult4 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult4 ? std::string{} : InitializeCurrentProjectResult4.error().ToString();
	EXPECT_TRUE(InitializeCurrentProjectResult4.has_value());
	EXPECT_FALSE(Durin::HasCurrentProject());
}

TEST_F(FProjectHistoryTest, ValidatesAdditionalMountDescriptorSchema)
{
	Durin::Testing::FScopedMountRegistryFixture Registry;
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
					"ContentWritable":false,
					"Dependencies":["/Engine/"]
				},
				{
					"VirtualRoot":"/Libraries/StudioArt/",
					"Owner":"ExternalSources",
					"Root":"Libraries/StudioArt",
					"ContentPath":".",
					"AutoScan":false,
					"ContentWritable":false,
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
	const auto InitializeCurrentProjectResult5 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult5 ? std::string{} : InitializeCurrentProjectResult5.error().ToString();
	ASSERT_TRUE(InitializeCurrentProjectResult5.has_value()) << Error;
	ASSERT_NE(Durin::GetCurrentProject(), nullptr);
	EXPECT_EQ(Durin::GetCurrentProject()->MountRoot, "/Game/");
	if (!Durin::GIsGameThreadIdInitialized)
	{
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints(&Error)) << Error;
	EXPECT_TRUE(Durin::FMountPaths::ResolveAssetPath("/Game/Levels/Test"));
	EXPECT_TRUE(Durin::FMountPaths::ResolveAssetPath("/Engine/StaticMeshes/Box"));
	EXPECT_TRUE(Durin::FMountPaths::ResolveAssetPath(
		"/Plugins/PCG/Noise.png", Durin::EMountPathExistence::RequireFile));
	EXPECT_TRUE(Durin::FMountPaths::ResolveAssetPath("/Libraries/StudioArt/Texture"));
	EXPECT_TRUE(Durin::FMountPaths::CheckMountDependency("/Game/Asset", "/Engine/Source"));
	EXPECT_TRUE(Durin::FMountPaths::CheckMountDependency("/Game/Asset", "/Plugins/PCG/Source"));
	EXPECT_EQ(
		Durin::FMountPaths::CheckMountDependency("/Engine/Asset", "/Game/Source").error().Code,
		Durin::EMountPathError::ForbiddenDependency);

	const std::string LegacyWritable = WriteProject(
		"LegacyWritable",
		R"({"ProjectName":"LegacyWritable","Mounts":[{
			"VirtualRoot":"/Libraries/Legacy/","Owner":"ExternalSources",
			"Root":"Legacy","ContentPath":".","AutoScan":false,"AuthoringWritable":false,
			"Dependencies":["/Engine/"]}]})");
	std::filesystem::create_directories(
		std::filesystem::path(LegacyWritable).parent_path() / "Legacy");
	Params.RequestedProjectFile = LegacyWritable;
	Error.clear();
	const auto InitializeCurrentProjectResult6 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult6 ? std::string{} : InitializeCurrentProjectResult6.error().ToString();
	EXPECT_TRUE(InitializeCurrentProjectResult6.has_value()) << Error;

	const std::array InvalidDescriptors{
		WriteProject(
			"ConflictingWritableKeys",
			R"({"ProjectName":"ConflictingWritableKeys","Mounts":[{
				"VirtualRoot":"/Libraries/Art/","Owner":"ExternalSources",
				"Root":"Art","ContentPath":".","AutoScan":false,
				"ContentWritable":false,"AuthoringWritable":false,
				"Dependencies":["/Engine/"]}]})"),
		WriteProject(
			"UnknownField",
			R"({"ProjectName":"UnknownField","Mounts":[{
				"VirtualRoot":"/Libraries/Art/","Owner":"ExternalSources",
				"Root":"Art","ContentPath":".","AutoScan":false,"ContentWritable":false,
				"Dependencies":["/Engine/"],"Unexpected":true}]})"),
		WriteProject(
			"BuiltInOverride",
			R"({"ProjectName":"BuiltInOverride","Mounts":[{
				"VirtualRoot":"/Engine/","Owner":"Extension",
				"Root":"Plugin","ContentPath":"Content","AutoScan":true,"ContentWritable":false,
				"Dependencies":[]}]})"),
		WriteProject(
			"Traversal",
			R"({"ProjectName":"Traversal","Mounts":[{
				"VirtualRoot":"/Libraries/Art/","Owner":"ExternalSources",
				"Root":"../Art","ContentPath":".","AutoScan":false,"ContentWritable":false,
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
		const auto InitializeCurrentProjectResult7 = Durin::InitializeCurrentProject(Params);
		Error = InitializeCurrentProjectResult7 ? std::string{} : InitializeCurrentProjectResult7.error().ToString();
		EXPECT_FALSE(InitializeCurrentProjectResult7.has_value());
		EXPECT_FALSE(Error.empty());
		EXPECT_FALSE(Durin::HasCurrentProject());
	}
}

TEST_F(FProjectHistoryTest, ResolvesEnabledModulesForCurrentRuntimeVariant)
{
	const std::string Project = WriteProject(
		"Modules",
		std::format(
			R"({{
				"ProjectName":"Modules",
				"ModuleDirs":{{"Runtime":"Source/Runtime","Editor":"Source/Editor"}},
				"BaseModules":["Runtime"],
				"ExtraModules":{{"{}":{{"Modules":["Editor","Runtime"]}}}}
			}})",
			DURIN_RUNTIME_VARIANT));
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = Project;
	std::string Error;
	const auto InitializeCurrentProjectResult8 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult8 ? std::string{} : InitializeCurrentProjectResult8.error().ToString();
	ASSERT_TRUE(InitializeCurrentProjectResult8.has_value()) << Error;
	ASSERT_NE(Durin::GetCurrentProject(), nullptr);
	EXPECT_EQ(
		Durin::GetCurrentProject()->EnabledRootModules,
		(std::vector<std::string>{"Runtime", "Editor"}));
}

TEST_F(FProjectHistoryTest, DefaultsEnabledBaseModulesToModuleDirectoryKeys)
{
	const std::string Project = WriteProject(
		"DefaultModules",
		R"({
			"ProjectName":"DefaultModules",
			"ModuleDirs":{"Runtime":"Source/Runtime","Tools":"Source/Tools"}
		})");
	Durin::FProjectInitializationParams Params;
	Params.RequestedProjectFile = Project;
	std::string Error;
	const auto InitializeCurrentProjectResult9 = Durin::InitializeCurrentProject(Params);
	Error = InitializeCurrentProjectResult9 ? std::string{} : InitializeCurrentProjectResult9.error().ToString();
	ASSERT_TRUE(InitializeCurrentProjectResult9.has_value()) << Error;
	ASSERT_NE(Durin::GetCurrentProject(), nullptr);
	EXPECT_EQ(
		Durin::GetCurrentProject()->EnabledRootModules,
		(std::vector<std::string>{"Runtime", "Tools"}));
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
