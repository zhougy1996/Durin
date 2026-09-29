#include "gtest/gtest.h"

#include "HAL/Platform.h"
#include "HAL/PlatformProcess.h"
#include "NativeTestSupport.h"
#include "ProfilingToolService.h"
#include "Profiling/Profiling.h"

#include <fstream>

namespace Durin::Editor::MainFrame
{
	namespace
	{
		constexpr std::string_view PlatformName = DURIN_BUILD_PLATFORM_STRING;

		class FProfilingToolServiceTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				RootDirectory = Testing::GetTestWorkDirectory() / "ProfilingToolService";
				Durin::Testing::RemoveTestWorkDirectory(RootDirectory);
				WriteManifest("0.14.1", "v0.14.1");
			}

			void TearDown() override
			{
				Durin::Testing::RemoveTestWorkDirectory(RootDirectory);
			}

			void WriteFile(const std::filesystem::path& RelativePath, std::string_view Contents = {})
			{
				const std::filesystem::path FilePath = RootDirectory / RelativePath;
				std::filesystem::create_directories(FilePath.parent_path());
				std::ofstream File(FilePath, std::ios::binary);
				File.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			}

			void WriteManifest(std::string_view ToolVersion, std::string_view ClientTag)
			{
				WriteFile(
					"Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/tracy-tools.json",
					std::format(
						R"({{
							"name": "tracy-tools",
							"version": "{}",
							"kind": "tool_package",
							"repair_command": "DevTool.bat dependency prepare --libs tracy,tracy-tools",
							"repair_commands_by_platform": {{
								"MacOS": "./DevTool dependency prepare --libs tracy,tracy-tools"
							}},
							"source_dir": "Engine/External/Packages/tracy-tools/{}/Win64",
							"source_dirs_by_platform": {{
								"Win64": "Engine/External/Packages/tracy-tools/{}/Win64",
								"MacOS": "Engine/External/Packages/tracy-tools/{}/MacOS"
							}},
							"source": {{
								"platforms": {{
									"Win64": {{
										"profiler_path": "tracy-profiler.exe",
										"required_files": [
											"tracy-profiler.exe",
											"tracy-capture.exe",
											"tracy-csvexport.exe"
										]
									}},
									"MacOS": {{
										"profiler_path": "tracy-profiler.app/Contents/MacOS/tracy-profiler",
										"required_files": [
											"tracy-profiler.app/Contents/MacOS/tracy-profiler",
											"tracy-capture",
											"tracy-csvexport"
										]
									}}
								}}
							}}
						}})",
						ToolVersion,
						ToolVersion,
						ToolVersion,
						ToolVersion
					)
				);
				WriteFile(
					"Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/tracy.json",
					std::format(R"({{"source": {{"tag": "{}"}}}})", ClientTag)
				);
			}

			void WriteRequiredTools(std::string_view Version = "0.14.1")
			{
				const std::filesystem::path Package = std::filesystem::path("Engine/External/Packages/tracy-tools")
					/ Version / PlatformName;
				if (PlatformName == "MacOS")
				{
					WriteFile(Package / "tracy-profiler.app/Contents/MacOS/tracy-profiler");
					WriteFile(Package / "tracy-capture");
					WriteFile(Package / "tracy-csvexport");
				}
				else
				{
					WriteFile(Package / "tracy-profiler.exe");
					WriteFile(Package / "tracy-capture.exe");
					WriteFile(Package / "tracy-csvexport.exe");
				}
			}

			std::filesystem::path RootDirectory;
		};
	}

	TEST_F(FProfilingToolServiceTests, ResolvesMatchingManagedInstallation)
	{
		WriteRequiredTools();
		const FTracyToolStatus Status = FProfilingToolService(RootDirectory).QueryStatus();

		EXPECT_TRUE(Status.bManifestValid);
		EXPECT_TRUE(Status.bPlatformSupported);
		EXPECT_TRUE(Status.bVersionMatches);
		EXPECT_TRUE(Status.bAvailable);
		EXPECT_EQ(Status.ExpectedVersion, "0.14.1");
		if (PlatformName == "MacOS")
			EXPECT_TRUE(Status.ProfilerPath.ends_with(
				"tracy-tools/0.14.1/MacOS/tracy-profiler.app/Contents/MacOS/tracy-profiler"));
		else
			EXPECT_TRUE(Status.ProfilerPath.ends_with("tracy-tools/0.14.1/Win64/tracy-profiler.exe"));
		EXPECT_TRUE(Status.MissingFiles.empty());
	}

	TEST_F(FProfilingToolServiceTests, ReportsVersionMismatch)
	{
		WriteManifest("0.14.1", "v0.14.0");
		WriteRequiredTools();
		const FTracyToolStatus Status = FProfilingToolService(RootDirectory).QueryStatus();

		EXPECT_FALSE(Status.bAvailable);
		EXPECT_FALSE(Status.bVersionMatches);
		EXPECT_NE(Status.Diagnostic.find("does not match"), std::string::npos);
	}

	TEST_F(FProfilingToolServiceTests, ReportsMissingFilesWithoutCreatingPackage)
	{
		const FProfilingToolService Service(RootDirectory);
		const FTracyToolStatus Status = Service.QueryStatus();

		EXPECT_FALSE(Status.bAvailable);
		EXPECT_EQ(Status.MissingFiles.size(), 3);
		EXPECT_FALSE(std::filesystem::exists(RootDirectory / "Engine/External"));
		EXPECT_NE(
			Status.Diagnostic.find(
				PlatformName == "MacOS"
					? "tracy-profiler.app/Contents/MacOS/tracy-profiler"
					: "tracy-profiler.exe"
			),
			std::string::npos
		);
		EXPECT_EQ(
			Status.RepairCommand,
			PlatformName == "MacOS"
				? R"(./DevTool dependency prepare --libs tracy,tracy-tools)"
				: R"(DevTool.bat dependency prepare --libs tracy,tracy-tools)"
		);
	}

	TEST_F(FProfilingToolServiceTests, QuotesCapturePaths)
	{
		EXPECT_EQ(
			FProfilingToolService::BuildCaptureArguments("C:/Capture Files/frame.tracy"),
			R"("C:/Capture Files/frame.tracy")"
		);
	}

	TEST_F(FProfilingToolServiceTests, RegistersExpectedMenuActions)
	{
		EXPECT_EQ(FProfilingToolService::LaunchProfilerLabel, "Profile This Editor");
		EXPECT_EQ(FProfilingToolService::OpenCaptureLabel, "Open Tracy Capture...");
		EXPECT_EQ(FProfilingToolService::OpenCaptureDirectoryLabel, "Open Capture Directory");
		EXPECT_EQ(FProfilingToolService::ShowStatusLabel, "Tool Status...");
	}

	TEST_F(FProfilingToolServiceTests, ConnectsToActualLocalPortIncludingNonDefaultAndOverridePorts)
	{
		for (uint16 Port : {uint16{8086}, uint16{8101}, uint16{29000}})
		{
			const auto Arguments = FProfilingToolService::BuildConnectionArguments({true, false, Port});
			ASSERT_TRUE(Arguments.has_value());
			EXPECT_EQ(*Arguments, std::format("-a 127.0.0.1 -p {}", Port));
		}
	}

	TEST_F(FProfilingToolServiceTests, DoesNotGuessAnEndpointWhenClientIsUnavailableOrBusy)
	{
		std::string Error;
		EXPECT_FALSE(FProfilingToolService::BuildConnectionArguments({false, false, 0}, &Error));
		EXPECT_NE(Error.find("built without Tracy"), std::string::npos);
		EXPECT_FALSE(FProfilingToolService::BuildConnectionArguments({true, false, 0}, &Error));
		EXPECT_NE(Error.find("listener is not ready"), std::string::npos);
		EXPECT_FALSE(FProfilingToolService::BuildConnectionArguments({true, true, 8090}, &Error));
		EXPECT_NE(Error.find("already connected"), std::string::npos);
	}
}
