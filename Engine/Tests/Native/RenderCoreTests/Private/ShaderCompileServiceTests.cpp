#include "Shader/ShaderPaths.h"
#include "Shader/ShaderCompilerCore.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Shader/ShaderCompileService.h"
#include "NativeTestSupport.h"

#include "gtest/gtest.h"

#include <fstream>

namespace Durin
{
	namespace
	{
		auto GetServiceTestRoot() -> std::filesystem::path
		{
			return Durin::Testing::GetTestWorkDirectory() / "ShaderCompileService";
		}

		auto WriteTestShader(const std::filesystem::path& FilePath) -> void
		{
			std::filesystem::create_directories(FilePath.parent_path());
			std::ofstream Stream(FilePath, std::ios::binary | std::ios::trunc);
			Stream << R"([shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    return float4(positions[vertexID], 0.0, 1.0);
}
)";
			ASSERT_TRUE(Stream.good());
		}

		auto WriteTextFile(const std::filesystem::path& FilePath, std::string_view Text) -> void
		{
			std::ofstream Stream(FilePath, std::ios::binary | std::ios::trunc);
			Stream << Text;
			ASSERT_TRUE(Stream.good());
		}

		auto MakeServiceOptions() -> FShaderCompileOptions
		{
			FShaderCompileOptions Options;
			Options.EntryPoints = {"VertexMain"};
			Options.Frequencies = {EShaderFrequency::Vertex};
			return Options;
		}

		class FShaderCompileServiceTests : public testing::Test
		{
		protected:
			void SetUp() override
			{
				ShutdownShaderCompileService();
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
				const std::filesystem::path Root = GetServiceTestRoot();
				std::error_code ErrorCode;
				Durin::Testing::RemoveTestWorkDirectory(Root, ErrorCode);
				ASSERT_FALSE(ErrorCode);
				std::filesystem::create_directories(Root / "Source");
				std::filesystem::create_directories(Root / "Cache");
				FShaderPaths::RegisterMountPoint(
					"/ShaderCompileServiceTests/",
					(Root / "Source").generic_string(),
					(Root / "Cache").generic_string()
				);
				WriteTestShader(Root / "Source" / "Simple.slang");
			}

			void TearDown() override
			{
				ShutdownShaderCompileService();
			}
		};
	}

	TEST_F(FShaderCompileServiceTests, WarmRestartUsesManifestWithoutResolvingOrHashingSources)
	{
		const FShaderCompileOptions Options = MakeServiceOptions();
		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		const FShaderCompileServiceStats ColdStats = GetShaderCompileServiceStats();
		EXPECT_EQ(ColdStats.DependencyResolutions, 1u);
		EXPECT_EQ(ColdStats.Compilations, 1u);
		EXPECT_GT(ColdStats.ContentReads, 0u);

		ShutdownShaderCompileService();
		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		const FShaderCompileServiceStats WarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.Compilations, 0u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 1u);
		EXPECT_EQ(WarmStats.DiskHits, 1u);
	}

	TEST_F(FShaderCompileServiceTests, ConcurrentIdenticalRequestsCompileOnce)
	{
		const FShaderCompileOptions Options = MakeServiceOptions();
		InitShaderCompileService();
		std::vector<std::future<FShaderCompilerOutput>> Requests;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			Requests.push_back(std::async(std::launch::async, [Options] {
				return GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options);
			}));
		}

		for (auto& Request : Requests)
		{
			EXPECT_TRUE(Request.get());
		}
		const FShaderCompileServiceStats Stats = GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 1u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		CorrectedShaderRecompilesAfterAnUncachedFailure)
	{
		const std::filesystem::path ShaderPath =
			GetServiceTestRoot() / "Source" / "Simple.slang";
		WriteTextFile(
			ShaderPath,
			"[shader(\"vertex\")] broken shader source\n");
		InitShaderCompileService();

		const FShaderCompileOptions Options = MakeServiceOptions();
		const FShaderCompilerOutput Broken =
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options);
		EXPECT_FALSE(Broken);
		EXPECT_FALSE(Broken.ErrorMessage.empty());

		WriteTestShader(ShaderPath);
		std::filesystem::last_write_time(
			ShaderPath,
			std::filesystem::last_write_time(ShaderPath)
				+ std::chrono::seconds(2));
		const FShaderCompilerOutput Corrected =
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options);
		EXPECT_TRUE(Corrected);
		const FShaderCompileServiceStats Stats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(Stats.DependencyResolutions, 2u);
		EXPECT_EQ(Stats.Compilations, 1u);
	}

	TEST_F(FShaderCompileServiceTests,
		ForceRecompileBypassesSuccessfulMemoryAndDiskOutputReuse)
	{
		InitShaderCompileService();
		const FShaderCompileOptions Options = MakeServiceOptions();
		ASSERT_TRUE(
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		ASSERT_TRUE(
			GetOrCompileShader("/ShaderCompileServiceTests/Simple", Options));
		const FShaderCompileServiceStats WarmStats =
			GetShaderCompileServiceStats();
		ASSERT_EQ(WarmStats.Compilations, 1u);
		ASSERT_EQ(WarmStats.MemoryHits, 1u);

		FShaderCompileOptions ForcedOptions = Options;
		ForcedOptions.bForceRecompile = true;
		ASSERT_TRUE(GetOrCompileShader(
			"/ShaderCompileServiceTests/Simple", ForcedOptions));
		const FShaderCompileServiceStats ForcedStats =
			GetShaderCompileServiceStats();
		EXPECT_EQ(ForcedStats.Compilations, 2u);
		EXPECT_EQ(ForcedStats.MemoryHits, 1u);
		EXPECT_EQ(ForcedStats.DiskHits, 0u);
	}

	TEST_F(FShaderCompileServiceTests, AlternatingMacroDependencyGraphsRemainWarmHits)
	{
		const std::filesystem::path SourceRoot = GetServiceTestRoot() / "Source";
		WriteTextFile(SourceRoot / "VariantA.slang", "static const float SelectedValue = 1.0;\n");
		WriteTextFile(SourceRoot / "VariantB.slang", "static const float SelectedValue = 2.0;\n");
		WriteTextFile(SourceRoot / "Conditional.slang", R"(#ifdef USE_A
#include "VariantA.slang"
#else
#include "VariantB.slang"
#endif
[shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    return float4(SelectedValue + float(vertexID), 0.0, 0.0, 1.0);
}
)"
		);

		FShaderCompileOptions FirstOptions = MakeServiceOptions();
		FirstOptions.Macros.emplace_back("USE_A");
		FShaderCompileOptions SecondOptions = MakeServiceOptions();
		SecondOptions.Macros.emplace_back("USE_B");

		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", FirstOptions));
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", SecondOptions));
		ShutdownShaderCompileService();

		InitShaderCompileService();
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", FirstOptions));
		ASSERT_TRUE(GetOrCompileShader("/ShaderCompileServiceTests/Conditional", SecondOptions));
		const FShaderCompileServiceStats WarmStats = GetShaderCompileServiceStats();
		EXPECT_EQ(WarmStats.DependencyResolutions, 0u);
		EXPECT_EQ(WarmStats.Compilations, 0u);
		EXPECT_EQ(WarmStats.ContentReads, 0u);
		EXPECT_EQ(WarmStats.ManifestHits, 2u);
		EXPECT_EQ(WarmStats.DiskHits, 2u);
	}
} // namespace Durin
