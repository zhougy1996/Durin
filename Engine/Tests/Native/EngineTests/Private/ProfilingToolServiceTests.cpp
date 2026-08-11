#include "gtest/gtest.h"

#include "HAL/PlatformProcess.h"
#include "NativeTestSupport.h"
#include "ProfilingToolService.h"

#include <fstream>

namespace Durin::Editor::MainFrame
{
	namespace
	{
		class FProfilingToolServiceTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				RootDirectory = Testing::GetTestWorkDirectory() / "ProfilingToolService";
				Durin::Testing::RemoveTestWorkDirectory(RootDirectory);
				WriteManifest("0.13.1", "v0.13.1");
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
							"source_dir": "Engine/External/Packages/tracy-tools/{}/Win64",
							"source": {{
								"platforms": {{
									"Win64": {{
										"required_files": [
											"tracy-profiler.exe",
											"tracy-capture.exe",
											"tracy-csvexport.exe"
										]
									}}
								}}
							}}
						}})",
						ToolVersion,
						ToolVersion
					)
				);
				WriteFile(
					"Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/tracy.json",
					std::format(R"({{"source": {{"tag": "{}"}}}})", ClientTag)
				);
			}

			void WriteRequiredTools(std::string_view Version = "0.13.1")
			{
				const std::filesystem::path Package =
					std::filesystem::path("Engine/External/Packages/tracy-tools") / Version / "Win64";
				WriteFile(Package / "tracy-profiler.exe");
				WriteFile(Package / "tracy-capture.exe");
				WriteFile(Package / "tracy-csvexport.exe");
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
		EXPECT_EQ(Status.ExpectedVersion, "0.13.1");
		EXPECT_TRUE(Status.ProfilerPath.ends_with("tracy-tools/0.13.1/Win64/tracy-profiler.exe"));
		EXPECT_TRUE(Status.MissingFiles.empty());
	}

	TEST_F(FProfilingToolServiceTests, ReportsVersionMismatch)
	{
		WriteManifest("0.13.1", "v0.13.0");
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
		EXPECT_NE(Status.Diagnostic.find("tracy-profiler.exe"), std::string::npos);
		EXPECT_EQ(
			Status.RepairCommand,
			R"(DevTool.bat dependency prepare --libs tracy,tracy-tools)"
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
		EXPECT_EQ(FProfilingToolService::LaunchProfilerLabel, "Launch Tracy Profiler");
		EXPECT_EQ(FProfilingToolService::OpenCaptureLabel, "Open Tracy Capture...");
		EXPECT_EQ(FProfilingToolService::OpenCaptureDirectoryLabel, "Open Capture Directory");
		EXPECT_EQ(FProfilingToolService::ShowStatusLabel, "Tool Status...");
	}
}
