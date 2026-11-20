#include "HAL/FileManager.h"
#include "MetalShaderFormat.h"
#include "ShaderOutputTestSupport.h"
#include <gtest/gtest.h>
#include "NativeRHIBackendTestSupport.h"

#include "Shader/Shader.h"
#include "Shader/ShaderParameterSchema.h"
#include "Shader/ShaderCompiledOutput.h"
#include "Shader/ShaderCookedLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/ContentPathInspection.h"
#include "SlangShaderCompiler.h"
#include "SlangShaderDependencyResolver.h"

#include <barrier>
#include <exception>
#include <regex>


namespace Durin
{
	namespace
	{
		// Scan shader declarations and C++ emitters together. Native MSL indices
		// require their own exact native-fixture boundary exceptions.
		const std::regex FixedResourceLocation(
			R"(vk\s*::\s*binding\s*\(([^)]*)\)|\bregister\s*\(\s*([tusb]\s*[0-9]+[^)]*)\)|\[\[\s*(buffer|texture|sampler)\s*\(([^)]*)\))");

		auto LocationToken(const std::smatch& Match) -> std::string
		{
			std::string Token = Match[1].matched ? "v:" + Match[1].str()
				: Match[2].matched ? "r:" + Match[2].str() : "m:" + Match[3].str() + ":" + Match[4].str();
			std::erase_if(Token, [](unsigned char C) { return std::isspace(C); });
			return Token;
		}

		auto FindFixedResourceLocations(const std::string& Source) -> std::vector<std::pair<size_t, std::string>>
		{
			std::vector<std::pair<size_t, std::string>> Result;
			for (size_t Begin = 0; Begin < Source.size();)
			{
				const auto Position = std::min({Source.find("vk", Begin), Source.find("register", Begin), Source.find("[[", Begin)});
				if (Position == std::string::npos) break;
				std::smatch Match;
				if ((Source[Position] == '[' || Position == 0 || !(std::isalnum(static_cast<unsigned char>(Source[Position - 1])) || Source[Position - 1] == '_'))
					&& std::regex_search(Source.begin() + Position, Source.end(), Match, FixedResourceLocation, std::regex_constants::match_continuous))
					Result.emplace_back(Position, LocationToken(Match));
				Begin = Position + 1;
			}
			return Result;
		}

		auto FindBinding(
			const FShaderCode& Shader,
			std::string_view Name
		) -> const FShaderResourceBinding*
		{
			const auto It = std::ranges::find(
				Shader.Reflection.ResourceBindings,
				Name,
				&FShaderResourceBinding::Name
			);
			return It == Shader.Reflection.ResourceBindings.end() ? nullptr : &*It;
		}

		auto GetSpirvInterfaceLocations(
			const FShaderCode& Shader,
			uint32 StorageClass
		) -> std::set<uint32>
		{
			constexpr uint16 OpDecorate = 71;
			constexpr uint16 OpVariable = 59;
			constexpr uint32 DecorationLocation = 30;
			std::vector<uint32> Words(
				Shader.Code.size() / sizeof(uint32)
			);
			std::memcpy(
				Words.data(),
				Shader.Code.data(),
				Shader.Code.size()
			);

			std::unordered_map<uint32, uint32> LocationsById;
			std::set<uint32> InputIds;
			for (size_t Offset = 5; Offset < Words.size();)
			{
				const uint32 Instruction = Words[Offset];
				const uint16 WordCount =
					static_cast<uint16>(Instruction >> 16);
				const uint16 OpCode =
					static_cast<uint16>(Instruction & 0xffffu);
				if (WordCount == 0
					|| Offset + WordCount > Words.size())
				{
					return {};
				}
				if (OpCode == OpDecorate && WordCount >= 4
					&& Words[Offset + 2] == DecorationLocation)
				{
					LocationsById.emplace(
						Words[Offset + 1],
						Words[Offset + 3]
					);
				}
				else if (OpCode == OpVariable && WordCount >= 4
						 && Words[Offset + 3] == StorageClass)
				{
					InputIds.insert(Words[Offset + 2]);
				}
				Offset += WordCount;
			}

			std::set<uint32> InputLocations;
			for (const uint32 InputId : InputIds)
			{
				if (const auto It = LocationsById.find(InputId);
					It != LocationsById.end())
				{
					InputLocations.insert(It->second);
				}
			}
			return InputLocations;
		}

		auto GetSpirvInputLocations(
			const FShaderCode& Shader
		) -> std::set<uint32>
		{
			constexpr uint32 StorageClassInput = 1;
			return GetSpirvInterfaceLocations(Shader, StorageClassInput);
		}

		auto GetSpirvOutputLocations(
			const FShaderCode& Shader
		) -> std::set<uint32>
		{
			constexpr uint32 StorageClassOutput = 3;
			return GetSpirvInterfaceLocations(Shader, StorageClassOutput);
		}

		auto GetSpirvImageFormats(
			const FShaderCode& Shader
		) -> std::set<uint32>
		{
			constexpr uint16 OpTypeImage = 25;
			std::vector<uint32> Words(
				Shader.Code.size() / sizeof(uint32)
			);
			std::memcpy(
				Words.data(),
				Shader.Code.data(),
				Shader.Code.size()
			);

			std::set<uint32> Formats;
			for (size_t Offset = 5; Offset < Words.size();)
			{
				const uint32 Instruction = Words[Offset];
				const uint16 WordCount =
					static_cast<uint16>(Instruction >> 16);
				const uint16 OpCode =
					static_cast<uint16>(Instruction & 0xffffu);
				if (WordCount == 0
					|| Offset + WordCount > Words.size())
				{
					return {};
				}
				if (OpCode == OpTypeImage && WordCount >= 9)
				{
					Formats.insert(Words[Offset + 8]);
				}
				Offset += WordCount;
			}
			return Formats;
		}

		auto HasSpirvInputBuiltIn(
			const FShaderCode& Shader,
			uint32 BuiltIn
		) -> bool
		{
			constexpr uint16 OpDecorate = 71;
			constexpr uint16 OpVariable = 59;
			constexpr uint32 DecorationBuiltIn = 11;
			constexpr uint32 StorageClassInput = 1;
			std::vector<uint32> Words(
				Shader.Code.size() / sizeof(uint32)
			);
			std::memcpy(
				Words.data(),
				Shader.Code.data(),
				Shader.Code.size()
			);

			std::set<uint32> BuiltInIds;
			std::set<uint32> InputIds;
			for (size_t Offset = 5; Offset < Words.size();)
			{
				const uint32 Instruction = Words[Offset];
				const uint16 WordCount =
					static_cast<uint16>(Instruction >> 16);
				const uint16 OpCode =
					static_cast<uint16>(Instruction & 0xffffu);
				if (WordCount == 0
					|| Offset + WordCount > Words.size())
				{
					return false;
				}
				if (OpCode == OpDecorate && WordCount >= 4
					&& Words[Offset + 2] == DecorationBuiltIn
					&& Words[Offset + 3] == BuiltIn)
				{
					BuiltInIds.insert(Words[Offset + 1]);
				}
				else if (OpCode == OpVariable && WordCount >= 4
						 && Words[Offset + 3] == StorageClassInput)
				{
					InputIds.insert(Words[Offset + 2]);
				}
				Offset += WordCount;
			}
			return std::ranges::any_of(
				BuiltInIds,
				[&InputIds](uint32 Id) { return InputIds.contains(Id); }
			);
		}

        auto ExpectResource(const FShaderCode& Shader, std::string_view Name,
            ERHIBindingType Type, EShaderStageFlags Stage) -> void
        {
            const auto* Binding = FindBinding(Shader, Name);
            ASSERT_NE(Binding, nullptr) << Name;
            EXPECT_EQ(Binding->Type, Type);
            EXPECT_EQ(Binding->StageFlags, Stage);
            EXPECT_EQ(Binding->ArraySize, 1u);
        }

	} // namespace

	class FShaderReflectionTests : public ::testing::Test
	{
	protected:
		Testing::FScopedRHIBackendOverride Backend{"vulkan"};
	};

	TEST(FShaderSourcePolicyTests, FixedLocationsRequireExactBoundaryExceptions)
	{
		struct FException
		{
			std::string Path;
			std::string Test;
			std::vector<std::string> Tokens;
			std::string Reason;
			std::vector<std::string> Observed;
		};
		std::vector<FException> Exceptions{
			{"Engine/Tests/Native/VulkanRHITests/Data/RecoverableResourceFactories.slang", "", {"v:5,2", "v:3,0", "v:7,2"}, "Sparse sets, dynamic offsets and compatible pipeline transitions"},
			{"Engine/Tests/Native/VulkanRHITests/Data/DualUseTexture.slang", "", {"v:0,0", "v:0,0", "v:1,0"}, "Entry-specific sampled/storage layout"},
			{"Engine/Tests/Native/VulkanRHITests/Data/DeferredBufferVersions.slang", "", {"v:0,0", "v:1,0", "v:2,0"}, "Direct RHI versioned-resource replay ABI"},
			{"Engine/Tests/Native/VulkanRHITests/Data/VulkanTextureSampling.slang", "", {"v:0,0", "v:1,0", "v:2,0", "v:3,0", "v:4,0", "v:5,0", "v:6,0", "v:7,0"}, "Direct RHI texture/view/sampler ABI"},
			{"Engine/Tests/Native/VulkanRHITests/Data/VolumeTextureCompute.slang", "", {"v:0,0", "v:1,0", "v:2,0", "v:3,0"}, "Independent 3D sampling and storage oracle"},
			{"Engine/Tests/Native/VulkanRHITests/Data/VolumetricCloudFixture.slang", "", {"v:0,0", "v:1,0", "v:2,0", "v:3,0", "v:4,0"}, "Independent low-level density sampling oracle"},
			{"Engine/Tests/Native/VulkanRHITests/Data/ComputeGraphicsInterop.slang", "", {"v:0,0"}, "Direct compute-to-graphics queue/barrier ABI"},
			{"Engine/Tests/Data/MetalShaderQualification/PublicComputePipeline.slang", "", {"v:0,0", "v:1,0"}, "Direct native compute and push-constant qualification"},
			{"Engine/Tests/Data/MetalShaderQualification/ResourceArray.slang", "", {"v:0,0", "v:1,0", "v:2,0"}, "Logical-to-native array expansion"},
			{"Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp", "FShaderReflectionTests.MetalComputeReservesArrayAndPushConstantSlots", {"v:0,0", "v:1,0", "v:2,0"}, "Compact Metal array slots and push-buffer reservation"},
			{"Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp", "FShaderReflectionTests.ContactVisibilityUsesCompilerAllocationsAcrossStagesAndCookReplay", {"v:11,2"}, "Controlled graphics/compute relocation through DDC and Cook"},
			{"Engine/Tests/Native/EngineTests/Private/Materials/MaterialCompilerTests.cpp", "FMaterialBindingContractTests.CompilerLocationsSurviveAdmissionAndSourceFreeCookReplay", {"v:11,2"}, "Controlled accepted-material allocation relocation"},
			{"Engine/Tests/Native/MetalShaderQualificationTests/Private/MetalShaderQualificationTests.mm", "FMetalShaderQualificationTests.R8StorageTextureWriteProducesCheckedReadback", {"m:texture:0"}, "Direct native MSL R8StorageTextureWriteProducesCheckedReadback ABI"},
			{"Engine/Tests/Native/MetalShaderQualificationTests/Private/MetalShaderQualificationTests.mm", "FMetalShaderQualificationTests.SRGBTextureSamplingDecodesToLinearColor", {"m:texture:0"}, "Direct native MSL SRGBTextureSamplingDecodesToLinearColor ABI"},
			{"Engine/Tests/Native/MetalShaderQualificationTests/Private/MetalShaderQualificationTests.mm", "FMetalShaderQualificationTests.DepthArrayComparisonSamplesCorrectLayer", {"m:texture:0", "m:sampler:0", "m:buffer:0"}, "Direct native MSL DepthArrayComparisonSamplesCorrectLayer ABI"},
			{"Engine/Tests/Native/MetalShaderQualificationTests/Private/MetalShaderQualificationTests.mm", "FMetalShaderQualificationTests.RGBA16CubeSamplingSelectsExpectedFace", {"m:texture:0", "m:buffer:0"}, "Direct native MSL RGBA16CubeSamplingSelectsExpectedFace ABI"},
			{"Engine/Tests/Native/MetalShaderQualificationTests/Private/MetalShaderQualificationTests.mm", "FMetalShaderQualificationTests.RGBA32FloatTextureSamplesWithLinearFilter", {"m:texture:0"}, "Direct native MSL RGBA32FloatTextureSamplesWithLinearFilter ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHIBufferTests.DeferredUniformVersionsReachSeparateDispatches", {"m:buffer:0", "m:buffer:1"}, "Direct native MSL DeferredUniformVersionsReachSeparateDispatches ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHIComputeTests.RecordedDispatchWritesStructuredBufferInBothExecutionModes", {"m:buffer:0", "m:buffer:1"}, "Direct native MSL RecordedDispatchWritesStructuredBufferInBothExecutionModes ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHIComputeTests.SparseDynamicArraysSurviveEncoderRestartAndPipelineReplacement", {"m:buffer:0", "m:buffer:1", "m:buffer:2", "m:buffer:3", "m:buffer:0", "m:buffer:1", "m:buffer:2", "m:buffer:3"}, "Direct native MSL SparseDynamicArraysSurviveEncoderRestartAndPipelineReplacement ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHIComputeTests.StorageWriteThenSampleUsesNativeTextureAndSamplerViews", {"m:texture:0", "m:texture:0", "m:sampler:0", "m:buffer:0", "m:texture:0", "m:sampler:0", "m:buffer:0", "m:buffer:1"}, "Direct native MSL StorageWriteThenSampleUsesNativeTextureAndSamplerViews ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHITextureTests.ProductionFallbackDescriptorsAdmitShaderOnlyUsage", {"m:texture:0", "m:buffer:0", "m:texture:0"}, "Direct native MSL ProductionFallbackDescriptorsAdmitShaderOnlyUsage ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHITextureTests.BC1SrgbCubeUploadsAndReadsBlockRowsAndMipTail", {"m:texture:0", "m:buffer:0"}, "Direct native MSL BC1SrgbCubeUploadsAndReadsBlockRowsAndMipTail ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHITextureTests.VolumeMipCopiesPreserveDepthSlices", {"m:texture:0", "m:buffer:0"}, "Direct native MSL VolumeMipCopiesPreserveDepthSlices ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIResourceTests.mm", "FMetalRHISubmissionTests.DelayedGPUCompletionRetainsPayloadAndShutdownDrainsCallbacks", {"m:buffer:0"}, "Direct native MSL DelayedGPUCompletionRetainsPayloadAndShutdownDrainsCallbacks ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIHeadlessTests.cpp", "FMetalRHIHeadlessTests.ShaderCreationValidatesTargetEntryAndNativeBindingMap", {"m:buffer:0"}, "Direct native MSL ShaderCreationValidatesTargetEntryAndNativeBindingMap ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIHeadlessTests.cpp", "FMetalRHIHeadlessTests.RecordedTriangleDrawFillsColorTargetInBothExecutionModes", {"m:buffer:0"}, "Direct native MSL RecordedTriangleDrawFillsColorTargetInBothExecutionModes ABI"},
			{"Engine/Tests/Native/MetalRHITests/Private/MetalRHIHeadlessTests.cpp", "FMetalRHIHeadlessTests.SparseDynamicArraysFanOutAcrossGraphicsStages", {"m:buffer:16", "m:buffer:17", "m:buffer:0", "m:buffer:1"}, "Direct native MSL SparseDynamicArraysFanOutAcrossGraphicsStages ABI"}
		};
		const auto Root = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR).parent_path().parent_path().parent_path();
		const auto Workspace = FFileHelper::LoadFileToString(Root / "Durin.dworkspace");
		ASSERT_TRUE(Workspace) << Workspace.error().GetMessage();
		const std::regex ProjectPattern(R"re("([^"]+\.dproject)")re");
		const std::regex TestPattern(R"(\bTEST(?:_F)?\s*\(\s*(\w+)\s*,\s*(\w+)\s*\))");
		const std::set<std::string> Extensions{".slang", ".hlsl", ".hlsli", ".fx", ".cpp", ".h", ".hpp", ".inl", ".mm", ".metal"};
		size_t ProjectCount = 0;
		for (std::sregex_iterator Project(Workspace->begin(), Workspace->end(), ProjectPattern), End; Project != End; ++Project)
		{
			++ProjectCount;
			const auto ProjectFile = Root / (*Project)[1].str();
			std::error_code Error;
			std::filesystem::path ReparsePoint;
			if (ContentStorage::FindReparsePointInPath(Root, ProjectFile.parent_path(), ReparsePoint, Error)) continue;
			if (Error)
			{
				ADD_FAILURE() << ProjectFile.parent_path().string() << ": " << Error.message();
				continue;
			}
			const bool IsProjectFile = std::filesystem::is_regular_file(ProjectFile, Error);
			if (Error || !IsProjectFile)
			{
				ADD_FAILURE() << ProjectFile.string() << ": " << (Error ? Error.message() : "not a regular project file");
				continue;
			}
			for (const char* Subtree : {"Shaders", "Source", "Tests", "Plugins"})
			{
				const auto Directory = ProjectFile.parent_path() / Subtree;
				struct FSourceVisitor final : IPlatformFile::FDirectoryVisitor
				{
					std::function<void(const FFilePath&)> OnFile;
					auto ShouldVisitLeafPathname(std::string_view Name) -> bool override
					{
						return !Name.starts_with('.') && Name != "ThirdParty" && Name != "Binaries"
							&& Name != "Build" && Name != "Intermediate";
					}
					auto Visit(const FFilePath& File, bool bIsDirectory) -> bool override
					{
						if (!bIsDirectory) OnFile(File);
						return true;
					}
				} Visitor;
				Visitor.OnFile = [&](const FFilePath& File)
				{
					if (!Extensions.contains(File.extension().string())) return;
					const auto Path = File.lexically_relative(Root).generic_string();
					const auto Source = FFileHelper::LoadFileToString(File);
					if (!Source)
					{
						ADD_FAILURE() << File.string() << ": " << Source.error().GetMessage();
						return;
					}
					if (!(Source->contains("vk") && Source->contains("binding")) && !Source->contains("register") && !Source->contains("[[")) return;
					for (const auto& [Position, Token] : FindFixedResourceLocations(*Source))
					{
						std::string Test;
						for (auto CasePosition = Source->find("TEST"); CasePosition != std::string::npos && CasePosition < Position; CasePosition = Source->find("TEST", CasePosition + 1))
						{
							std::smatch Case;
							if (std::regex_search(Source->begin() + CasePosition, Source->end(), Case, TestPattern, std::regex_constants::match_continuous))
								Test = Case[1].str() + "." + Case[2].str();
						}
						const auto Exception = std::ranges::find_if(Exceptions, [&](const auto& E) { return E.Path == Path && E.Test == Test; });
						const auto Line = 1 + std::count(Source->begin(), Source->begin() + Position, '\n');
						EXPECT_NE(Exception, Exceptions.end()) << Path << ':' << Line << " unapproved fixed resource location";
						if (Exception != Exceptions.end()) Exception->Observed.push_back(Token);
					}
				};
				auto& Files = IFileManager::Get();
				if (Files.DirectoryExists(Directory))
					EXPECT_TRUE(Files.IterateDirectoryRecursively(Directory, Visitor)) << Directory.string();
			}
		}
		EXPECT_GT(ProjectCount, 0u);
		for (auto& Exception : Exceptions)
		{
			EXPECT_FALSE(Exception.Reason.empty());
			std::ranges::sort(Exception.Tokens);
			std::ranges::sort(Exception.Observed);
			EXPECT_EQ(Exception.Observed, Exception.Tokens) << Exception.Path << ' ' << Exception.Test
				<< ": stale or changed exception: " << Exception.Reason;
		}
		// Exercise alternate syntax without adding fixed-layout fixture declarations.
		const std::string Probes = "vk" " :: binding \n ( 7, 3 ) : reg" "ister ( t9, space2 ) Value[" "[ texture ( 5 ) ]]";
		std::vector<std::string> Tokens;
		for (const auto& [Position, Token] : FindFixedResourceLocations(Probes)) Tokens.push_back(Token);
		EXPECT_EQ(Tokens, (std::vector<std::string>{"v:7,3", "r:t9,space2", "m:texture:5"}));
	}

	TEST_F(FShaderReflectionTests, PooledSessionsDoNotCrossThreadLifetimes)
	{
		FSlangGlobalSessionPool Pool;
		slang::IGlobalSession* FirstSession = nullptr;
		slang::IGlobalSession* NextSession = nullptr;
		std::thread First([&] {
			auto Lease = Pool.Acquire();
			FirstSession = Lease.operator->();
		});
		First.join();
		std::thread Next([&] {
			auto Lease = Pool.Acquire();
			NextSession = Lease.operator->();
		});
		Next.join();
		// std::thread::id can be recycled after join; this is still a new thread.
		EXPECT_NE(FirstSession, NextSession);
	}

	TEST_F(FShaderReflectionTests, PooledSessionsSurviveResolverChurnAndConcurrentReflection)
	{
		constexpr std::string_view Source = R"(
Texture2D<float4> Inputs[2];
SamplerState InputSamplers[2];
RWStructuredBuffer<uint> Output;
[shader("compute")]
[numthreads(2, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = uint(Inputs[id.x].SampleLevel(InputSamplers[id.x], float2(0.5, 0.5), 0).r * 255);
}
)";
		FSlangShaderCompiler Compiler;
		// Exceed the pool's four idle slots, then repeat to cover warm reuse
		// across resolver destruction without turning this into a stress benchmark.
		constexpr uint32 WorkerCount = 6;
		constexpr uint32 IterationCount = 3;
		std::barrier Start(WorkerCount);
		std::vector<std::future<std::string>> Workers;
		for (uint32 Worker = 0; Worker < WorkerCount; ++Worker)
			Workers.push_back(std::async(std::launch::async, [&] {
				FShaderCompileOptions Options;
				Options.Target = VulkanShaderTarget;
				Options.VirtualShaderPath = "/Unit/SessionLifetime";
				Options.EntryPoints = {"ComputeMain"};
				Options.Frequencies = {EShaderFrequency::Compute};
				Start.arrive_and_wait();
				for (uint32 Iteration = 0; Iteration < IterationCount; ++Iteration)
				{
					{
						FSlangShaderDependencyResolver Resolver;
						std::vector<std::string> Dependencies;
						const auto Resolved = Resolver.ResolveSource("SessionLifetime", "/Unit/SessionLifetime.slang", Source, Options, Dependencies);
						if (!Resolved) return std::string(Resolved.error());
					}
					const auto Result = Compiler.CompileSource("SessionLifetime", "/Unit/SessionLifetime.slang", Source, Options);
					if (!Result) return FormatShaderCompilerErrors(Result.Errors);
					if (Result.CompiledShaders.size() != 1 || Result.CompiledShaders.front().Reflection.ResourceBindings.size() != 3)
						return std::string("Reflection lost resource bindings after resolver destruction.");
				}
				return std::string{};
			}));
		for (auto& Worker : Workers) EXPECT_EQ(Worker.get(), "");
	}

	TEST_F(FShaderReflectionTests, CompilerSessionsSurviveSequentialWorkerLifetimes)
	{
		FSlangShaderCompiler Compiler;
		const auto Path = (std::filesystem::path(DURIN_TEST_DATA_DIR) / "StorageResources.slang").string();
		FShaderCompileOptions Options;
		Options.Target = VulkanShaderTarget;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};
		for (uint32 Iteration = 0; Iteration < 16; ++Iteration)
		{
			SCOPED_TRACE(Iteration);
			FShaderCompilerOutput Output;
			std::exception_ptr Failure;
			std::thread Worker([&] {
				try { Output = Compiler.Compile(Path, Options); }
				catch (...) { Failure = std::current_exception(); }
			});
			Worker.join();
			if (Failure) std::rethrow_exception(Failure);
			ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
			ASSERT_EQ(Output.CompiledShaders.size(), 1u);
			EXPECT_FALSE(Output.CompiledShaders.front().Reflection.ResourceBindings.empty());
		}
	}

	TEST_F(FShaderReflectionTests, ReflectsStorageBuffersAndImages)
	{
		const std::filesystem::path ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "StorageResources.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);

		const auto& Bindings = Output.CompiledShaders[0].Reflection.ResourceBindings;
		ASSERT_EQ(Bindings.size(), 3u);
		EXPECT_EQ(Bindings[0].Name, "ReadBuffer");
		EXPECT_EQ(Bindings[0].Type, ERHIBindingType::StorageBuffer);
		EXPECT_EQ(Bindings[1].Name, "WriteBuffer");
		EXPECT_EQ(Bindings[1].Type, ERHIBindingType::StorageBuffer);
		EXPECT_EQ(Bindings[2].Name, "OutputImage");
		EXPECT_EQ(Bindings[2].Type, ERHIBindingType::StorageImage);
	}

    TEST_F(FShaderReflectionTests, FiniteEditorProgramsRetainOptimizedAllocationsThroughLibraryReplay)
    {
        for (const auto Program : {"ImGui", "TexturePreview"}) {
            SCOPED_TRACE(Program);
            FShaderCompileOptions Options;
            Options.Target = VulkanShaderTarget;
            Options.VirtualShaderPath = std::format("/Engine/{}", Program);
            Options.EntryPoints = {"VertexMain", "FragmentMain"};
            Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
            FShaderType Policy(std::format("{}LibraryPolicy", Program), Options.VirtualShaderPath, EShaderFrequency::Vertex, "VertexMain");
            Policy.ApplyParameterCompilationEnvironment(Options);
            const auto Output = FSlangShaderCompiler().Compile(
                (std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR) / std::format("{}.slang", Program)).string(), Options);
            ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
            ASSERT_EQ(Output.CompiledShaders.size(), 2u);
            const bool ImGui = std::string_view(Program) == "ImGui";
            EXPECT_EQ(Output.CompiledShaders[0].Reflection.ResourceBindings.size(), ImGui ? 1u : 0u);
            EXPECT_EQ(Output.CompiledShaders[1].Reflection.ResourceBindings.size(), ImGui ? 2u : 3u);
            FShaderRuntimeRequest Request{.TargetPlatform = EShaderTargetPlatform::Win64,
                .TargetProfile = EShaderTargetProfile::EditorValidation, .Category = EShaderRuntimeRequestCategory::FeatureProgram,
                .Owner = "FiniteEditorAllocationTest", .Name = Program,
                .Members = {{"Vertex", "VertexMain", EShaderFrequency::Vertex}, {"pixel", "FragmentMain", EShaderFrequency::Fragment}}};
            const FShaderCookedLibraryRecord Record{Request, {.HashLow = 1}, Output};
            FByteBuffer Bytes;
            ASSERT_TRUE(EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::EditorValidation, std::span(&Record, 1), Bytes));
            FShaderCookedLibrary Library;
            const auto Shared = std::make_shared<const FByteBuffer>(Bytes);
            ASSERT_TRUE(FShaderCookedLibrary::OpenBytes(Shared, EShaderTargetPlatform::Win64,
                EShaderTargetProfile::EditorValidation, std::span(&Request, 1), Library));
            FShaderCompilerOutput Replay;
            ASSERT_TRUE(Library.Load(Request, Replay));
            ASSERT_EQ(Replay.CompiledShaders.size(), 2u);
            for (size_t Index = 0; Index < 2; ++Index)
                EXPECT_EQ(Replay.CompiledShaders[Index].Reflection.ResourceBindings, Output.CompiledShaders[Index].Reflection.ResourceBindings);
            FShaderCookedLibrary WrongProfile;
            EXPECT_FALSE(FShaderCookedLibrary::OpenBytes(Shared, EShaderTargetPlatform::Win64,
                EShaderTargetProfile::Game, {}, WrongProfile));
        }
    }

	TEST_F(FShaderReflectionTests, TexturePreviewCompilesWithMaterialNormalDecoder)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "TexturePreview.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/TexturePreview";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const auto Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
		const auto& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 3u);
		ExpectResource(Fragment, "PreviewTexture", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "PreviewSampler", ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "PreviewSettings", ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

#if defined(__APPLE__)
	TEST_F(FShaderReflectionTests, MetalTexturePreviewCarriesCheckedNativeBindings)
	{
		const auto ShaderPath = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "TexturePreview.slang";
		FShaderCompileOptions Options;
		Options.Target = MetalShaderTarget;
		Options.VirtualShaderPath = "/Engine/TexturePreview";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		const auto Output = FSlangShaderCompiler().Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		for (const auto& Shader : Output.CompiledShaders)
		{
			EXPECT_EQ(Shader.Target, MetalShaderTarget);
			EXPECT_EQ(Shader.CodeFormat, EShaderCodeFormat::Msl20Source);
			EXPECT_EQ(Shader.BinaryEntryPoint, "main0");
			ASSERT_TRUE(!Shader.Code.IsEmpty());
			const std::string_view Source(reinterpret_cast<const char*>(Shader.Code.data()),
				Shader.Code.size());
			EXPECT_TRUE(Source.contains("#include <metal_stdlib>"));
			EXPECT_NE(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Identity, FXxHash128{});
		}
		const auto& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->Bindings.size(), 3u);
        const auto Native = ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes());
        ASSERT_TRUE(Native);
        for (const auto& Resource : Fragment.Reflection.ResourceBindings) {
            const auto Match = std::ranges::find_if(Native->Bindings, [&](const auto& Binding) {
                return Binding.SetIndex == Resource.SetIndex && Binding.BindingIndex == Resource.BindingIndex;
            });
            ASSERT_NE(Match, Native->Bindings.end());
            EXPECT_EQ(Match->Type, Resource.Type);
            EXPECT_EQ(Match->Count, Resource.ArraySize);
        }
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Fragment.Frequency, Fragment.PlatformData.GetBytes())->PushConstantBufferSlot, UINT32_MAX);
	}

	TEST_F(FShaderReflectionTests, MetalComputeReservesArrayAndPushConstantSlots)
	{
		constexpr std::string_view Source = R"(
[[vk::binding(0, 0)]] Texture2D<float4> Inputs[2];
[[vk::binding(1, 0)]] SamplerState InputSamplers[2];
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> Output;
struct FPush { uint Offset; };
[[vk::push_constant]] ConstantBuffer<FPush> Push;
[shader("compute")]
[numthreads(2, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    Output[id.x] = uint(Inputs[id.x].SampleLevel(InputSamplers[id.x], float2(0.5, 0.5), 0).r * 255) + Push.Offset;
}
)";
		FShaderCompileOptions Options;
		Options.Target = MetalShaderTarget;
		Options.VirtualShaderPath = "/Unit/MetalBindingArrays";
		Options.EntryPoints = {"ComputeMain"};
		Options.Frequencies = {EShaderFrequency::Compute};
		const auto Output = FSlangShaderCompiler().CompileSource(
			"MetalBindingArrays", "/Unit/MetalBindingArrays.slang", Source, Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);
		const auto& Shader = Output.CompiledShaders[0];
		ASSERT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings.size(), 3u);
		EXPECT_EQ(Shader.ComputeThreadGroupSize,
			(std::array<uint32, 3>{2, 1, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings[0], (FMetalShaderBinding{0, 0, ERHIBindingType::Texture, 0, 2}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings[1], (FMetalShaderBinding{0, 1, ERHIBindingType::Sampler, 0, 2}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->Bindings[2], (FMetalShaderBinding{0, 2, ERHIBindingType::StorageBuffer, 0, 1}));
		EXPECT_EQ(ShaderFormat::DecodeMetalMetadata(Shader.Frequency, Shader.PlatformData.GetBytes())->PushConstantBufferSlot, 1u);
	}
#endif

	TEST_F(FShaderReflectionTests, VolumetricCloudPublishesMatchedSpatialBindingsAndRgba16Output)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloud.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloud";
		Options.EntryPoints = {"VertexMain", "CloudFragmentMain", "CloudComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 3u);
		const FShaderCode& Vertex = Output.CompiledShaders[0];
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		const FShaderCode& Compute = Output.CompiledShaders[2];
		EXPECT_TRUE(Vertex.Reflection.ResourceBindings.empty());
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		ASSERT_EQ(Compute.Reflection.ResourceBindings.size(), 7u);

		for (const FShaderCode* Shader : {&Fragment, &Compute})
		{
			const EShaderStageFlags Stage = Shader == &Fragment ? EShaderStageFlags::Fragment : EShaderStageFlags::Compute;
			ExpectResource(*Shader, "BaseDensity", ERHIBindingType::Texture, Stage);
			ExpectResource(*Shader, "DetailDensity", ERHIBindingType::Texture, Stage);
			ExpectResource(*Shader, "WeatherTexture", ERHIBindingType::Texture, Stage);
			ExpectResource(*Shader, "SceneDepth", ERHIBindingType::Texture, Stage);
			ExpectResource(*Shader, "DensitySampler", ERHIBindingType::Sampler, Stage);
			ExpectResource(*Shader, "Params", ERHIBindingType::UniformBuffer, Stage);
		}
		ExpectResource(Compute, "CloudOutput", ERHIBindingType::StorageImage, EShaderStageFlags::Compute);
		constexpr uint32 SpirvImageFormatRgba16f = 2;
		EXPECT_TRUE(GetSpirvImageFormats(Compute).contains(
			SpirvImageFormatRgba16f
		));
	}

	TEST_F(FShaderReflectionTests, VolumetricCloudShadowPublishesMatchedR8VisibilityBindings)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloudShadow.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloudShadow";
		Options.EntryPoints = {"VertexMain", "CloudVisibilityFragmentMain", "CloudVisibilityComputeMain"};
		Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(
			ShaderPath.string(), Options
		);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 3u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		const FShaderCode& Compute = Output.CompiledShaders[2];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		ASSERT_EQ(Compute.Reflection.ResourceBindings.size(), 7u);
		for (const FShaderCode* Shader : {&Fragment, &Compute})
		{
			const EShaderStageFlags Stage = Shader == &Fragment ? EShaderStageFlags::Fragment : EShaderStageFlags::Compute;
			const std::array<const char*, 4> Textures{
				"BaseDensity", "DetailDensity", "WeatherTexture", "SceneDepth"
			};
			for (uint32 Index = 0; Index < Textures.size(); ++Index)
				ExpectResource(*Shader, Textures[Index], ERHIBindingType::Texture, Stage);
			ExpectResource(*Shader, "DensitySampler", ERHIBindingType::Sampler, Stage);
			ExpectResource(*Shader, "Params", ERHIBindingType::UniformBuffer, Stage);
		}
		ExpectResource(Compute, "CloudVisibilityOutput", ERHIBindingType::StorageImage, EShaderStageFlags::Compute);
	}

	TEST_F(FShaderReflectionTests, VolumetricCloudCompositePublishesDepthAwareReconstructionBindings)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloudComposite.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloudComposite";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 5u);
		ExpectResource(Fragment, "SceneColorTexture", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "CloudTexture", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "SceneDepthTexture", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "DebugTexture", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "Params", ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

	TEST_F(FShaderReflectionTests, VolumetricCloudTemporalPublishesTypedHistoryBindings)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "VolumetricCloudTemporal.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/VolumetricCloudTemporal";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 4u);
		ExpectResource(Fragment, "CurrentCloud", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "PreviousCloud", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "HistorySampler", ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "Params", ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

	TEST_F(FShaderReflectionTests, FragmentDepthOutputCompilesWithoutPipelineBindings)
	{
		const std::filesystem::path ShaderPath = std::filesystem::path(DURIN_TEST_DATA_DIR) / "FragmentDepth.slang";
		FShaderCompileOptions Options;
		Options.EntryPoints = {"fragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output = Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);

		const FShaderCode& Shader = Output.CompiledShaders[0];
		EXPECT_EQ(Shader.Frequency, EShaderFrequency::Fragment);
		EXPECT_FALSE(Shader.Code.IsEmpty());
		EXPECT_TRUE(Shader.Reflection.ResourceBindings.empty());
		EXPECT_TRUE(Shader.Reflection.PushConstantRanges.empty());

		FPipelineLayoutDesc PipelineLayout;
		FShaderOperationResult ErrorMessage;
		ASSERT_TRUE((ErrorMessage = BuildPipelineLayoutFromShaders(Output.CompiledShaders, PipelineLayout))) << std::string(ErrorMessage.error());
		EXPECT_TRUE(PipelineLayout.BindingLayouts.empty());
		EXPECT_TRUE(PipelineLayout.PushConstantRanges.empty());
	}

	TEST_F(FShaderReflectionTests, ErrorTerminalPreservesVertexAbiWithoutFragmentDescriptors)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "StaticMeshBasePass.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/StaticMeshBasePass";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex,
			EShaderFrequency::Fragment
		};

		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& VertexShader =
			Output.CompiledShaders[0];
		const FShaderCode& FragmentShader =
			Output.CompiledShaders[1];

		EXPECT_EQ(VertexShader.SourceEntryPoint, "VertexMain");
		EXPECT_EQ(FragmentShader.SourceEntryPoint, "FragmentMain");
		EXPECT_EQ(VertexShader.BinaryEntryPoint, "main");
		EXPECT_EQ(FragmentShader.BinaryEntryPoint, "main");
		// Constant error color does not consume the FrontFacing built-in.
		EXPECT_FALSE(HasSpirvInputBuiltIn(FragmentShader, 17u));
		EXPECT_EQ(
			GetSpirvInputLocations(VertexShader),
			(std::set<uint32>{0, 1, 2, 3, 4, 5, 6, 7})
		);

		ASSERT_EQ(VertexShader.Reflection.ResourceBindings.size(), 5u);
		for (const auto Name : {"MaterialView", "GPUScenePrimitiveSceneData", "GPUSceneInstanceSceneData",
			"GPUSceneInstancePayloadData", "GPUSceneInstanceIds"}) {
			const auto* Binding = FindBinding(VertexShader, Name);
			ASSERT_NE(Binding, nullptr) << Name;
			EXPECT_EQ(Binding->Type, std::string_view(Name) == "MaterialView" ? ERHIBindingType::UniformBuffer : ERHIBindingType::StorageBuffer);
			EXPECT_EQ(Binding->ArraySize, 1u);
			EXPECT_EQ(Binding->StageFlags, EShaderStageFlags::Vertex);
		}
		EXPECT_TRUE(FragmentShader.Reflection.ResourceBindings.empty());
		EXPECT_TRUE(FragmentShader.Reflection.PushConstantRanges.empty());
		FPipelineLayoutDesc PipelineLayout;
		FShaderOperationResult ErrorMessage;
		ASSERT_TRUE((ErrorMessage = BuildPipelineLayoutFromShaders(Output.CompiledShaders, PipelineLayout))) << std::string(ErrorMessage.error());
		ASSERT_EQ(PipelineLayout.BindingLayouts.size(), 1u);
		const auto& ViewLayout = PipelineLayout.BindingLayouts[0].BindingLayouts;
		ASSERT_EQ(ViewLayout.size(), 5u);
		for (const auto& Binding : VertexShader.Reflection.ResourceBindings)
		{
			const auto Slot = std::ranges::find(ViewLayout, Binding.BindingIndex, &FBindingLayoutItem::Slot);
			ASSERT_NE(Slot, ViewLayout.end());
			EXPECT_EQ(Slot->Type, Binding.Type);
			EXPECT_EQ(Slot->StageFlags, Binding.StageFlags);
		}
	}

	TEST_F(FShaderReflectionTests, GeometryPassPublishesFourTargetsForEveryVertexFactoryDomain)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "StaticMeshBasePass.slang";
		const std::array<std::pair<const char*, const char*>, 2> Domains{{
			{"Local", nullptr},
			{"Spline", "DURIN_SPLINE_MESH"},
		}};
		FSlangShaderCompiler Compiler;
		for (const auto& [Name, DomainMacro] : Domains)
		{
			FShaderCompileOptions Options;
			Options.VirtualShaderPath = "/Engine/StaticMeshBasePass";
			Options.EntryPoints = {"VertexMain", "GeometryFragmentMain"};
			Options.Frequencies = {
				EShaderFrequency::Vertex,
				EShaderFrequency::Fragment
			};
			Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", "1");
			Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", "1");
			Options.Macros.emplace_back(
				"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
				"1056964608"
			);
			if (DomainMacro != nullptr)
				Options.Macros.emplace_back(DomainMacro, "1");

			const FShaderCompilerOutput Output =
				Compiler.Compile(ShaderPath.string(), Options);
			ASSERT_TRUE(Output) << Name << ": " << FormatShaderCompilerErrors(Output.Errors);
			ASSERT_EQ(Output.CompiledShaders.size(), 2u) << Name;
			const FShaderCode& Fragment = Output.CompiledShaders[1];
			EXPECT_EQ(Fragment.SourceEntryPoint, "GeometryFragmentMain")
				<< Name;
			EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0, 1, 2, 3})) << Name;
			EXPECT_TRUE(HasSpirvInputBuiltIn(Fragment, 17u)) << Name;
			ASSERT_TRUE(Fragment.Reflection.ResourceBindings.empty())
				<< Name;
			EXPECT_EQ(FindBinding(Fragment, "Lighting"), nullptr) << Name;
			EXPECT_EQ(FindBinding(Fragment, "EnvironmentIrradiance"), nullptr)
				<< Name;
			EXPECT_EQ(FindBinding(Fragment, "DirectionalShadowTexture"), nullptr)
				<< Name;
		}
	}

	TEST_F(FShaderReflectionTests, GBufferDebugDecodesAllAttachmentsAndDepth)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "GBufferDebug.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/GBufferDebug";
		Options.EntryPoints = {"VertexMain", "DebugFragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0}));
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 6u);
		ExpectResource(Fragment, "GBufferMaterial", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "GBufferNormals", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "GBufferSurface", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "BasePassColor", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "SceneDepth", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "Params", ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
	}

    TEST_F(FShaderReflectionTests, RegisteredGraphicsIsolatesIndependentStageAllocations)
    {
        FSlangShaderCompiler Compiler;
        FShaderCompileOptions VertexOptions, FragmentOptions;
        VertexOptions.Target = FragmentOptions.Target = VulkanShaderTarget;
        VertexOptions.EntryPoints = {"Main"}; FragmentOptions.EntryPoints = {"Main"};
        VertexOptions.Frequencies = {EShaderFrequency::Vertex};
        FragmentOptions.Frequencies = {EShaderFrequency::Fragment};
        FShaderType VertexType("IndependentVertex", "/Unit/Vertex", EShaderFrequency::Vertex, "Main");
        FShaderType FragmentType("IndependentFragment", "/Unit/Fragment", EShaderFrequency::Fragment, "Main");
        VertexType.ApplyParameterCompilationEnvironment(VertexOptions);
        FragmentType.ApplyParameterCompilationEnvironment(FragmentOptions);
        const auto Vertex = Compiler.CompileSource("IndependentVertex", "/Unit/Vertex.slang",
            "struct Data { float4 Position; }; ConstantBuffer<Data> Input; [shader(\"vertex\")] float4 Main() : SV_Position { return Input.Position; }", VertexOptions);
        const auto Fragment = Compiler.CompileSource("IndependentFragment", "/Unit/Fragment.slang",
            "Texture2D<float4> Input; [shader(\"fragment\")] float4 Main() : SV_Target { return Input.Load(int3(0,0,0)); }", FragmentOptions);
        ASSERT_TRUE(Vertex) << FormatShaderCompilerErrors(Vertex.Errors);
        ASSERT_TRUE(Fragment) << FormatShaderCompilerErrors(Fragment.Errors);
        const auto& V = Vertex.CompiledShaders[0].Reflection;
        const auto& F = Fragment.CompiledShaders[0].Reflection;
        ASSERT_EQ(V.ResourceBindings.size(), 1u); ASSERT_EQ(F.ResourceBindings.size(), 1u);
        EXPECT_EQ(V.ResourceBindings[0].BindingIndex, F.ResourceBindings[0].BindingIndex);
        EXPECT_EQ(V.ResourceBindings[0].SetIndex, 0u);
        EXPECT_EQ(F.ResourceBindings[0].SetIndex, 1u);
        const std::array Reflections{V, F};
        FPipelineLayoutDesc Layout;
        EXPECT_TRUE(BuildPipelineLayoutFromReflection(Reflections, Layout));
    }

	TEST_F(FShaderReflectionTests, ContactVisibilityUsesCompilerAllocationsAcrossStagesAndCookReplay)
    {
        const auto Path = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR) / "ContactShadow.slang";
        auto Loaded = FFileHelper::LoadFileToString(Path);
        ASSERT_TRUE(Loaded);
        std::string Source = *Loaded;
        FShaderCompileOptions Options;
        Options.Target = VulkanShaderTarget;
        Options.VirtualShaderPath = "/Engine/ContactShadow";
        Options.EntryPoints = {"VertexMain", "ContactVisibilityFragmentMain", "ContactVisibilityComputeMain"};
        Options.Frequencies = {EShaderFrequency::Vertex, EShaderFrequency::Fragment, EShaderFrequency::Compute};
        FShaderType Type("ContactAllocationPolicy", Options.VirtualShaderPath, EShaderFrequency::Fragment, "ContactVisibilityFragmentMain");
        Type.ModifyCompilationEnvironment({}, Options);
        const auto Check = [&](const FShaderCompilerOutput& Output) {
            ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
            ASSERT_EQ(Output.CompiledShaders.size(), 3u);
            EXPECT_TRUE(Output.CompiledShaders[0].Reflection.ResourceBindings.empty());
            for (size_t Index : {1u, 2u}) {
                const auto& Shader = Output.CompiledShaders[Index];
                EXPECT_EQ(Shader.Reflection.ResourceBindings.size(), Index == 1 ? 5u : 6u);
                for (const char* Name : {"GBufferMaterial", "GBufferNormals", "GBufferSurface", "SceneDepth"}) {
                    const auto* Binding = FindBinding(Shader, Name);
                    ASSERT_NE(Binding, nullptr);
                    EXPECT_EQ(Binding->Type, ERHIBindingType::Texture);
                    EXPECT_EQ(Binding->ResourceShape, EShaderResourceShape::Texture2D);
                }
                const auto* Params = FindBinding(Shader, "Params");
                ASSERT_NE(Params, nullptr);
                EXPECT_EQ(Params->ConstantBufferSize, 192u);
                ASSERT_EQ(Params->ConstantMembers.size(), 6u);
                EXPECT_EQ(Params->ConstantMembers.back().Name, "Trace");
                EXPECT_EQ(Params->ConstantMembers.back().Offset, 176u);
                if (Index == 1) EXPECT_EQ(GetSpirvOutputLocations(Shader), (std::set<uint32>{0}));
                else {
                    const auto* Storage = FindBinding(Shader, "ContactVisibilityOutput");
                    ASSERT_NE(Storage, nullptr);
                    EXPECT_EQ(Storage->Type, ERHIBindingType::StorageImage);
                }
            }
        };
        FSlangShaderCompiler Compiler;
        const auto Original = Compiler.CompileSource("ContactShadow", Path.string(), Source, Options);
        Check(Original);
        ASSERT_TRUE(Original);
        const auto Declaration = Source.find("ConstantBuffer<ContactShadowUniform> Params;");
        ASSERT_NE(Declaration, std::string::npos);
        // Controlled relocation is a test input, never a feature ABI.
        Source.insert(Declaration, "[[vk::binding(11, 2)]] ");
        const auto Relocated = Compiler.CompileSource("ContactShadow", Path.string(), Source, Options);
        Check(Relocated);
        ASSERT_TRUE(Relocated);
        for (size_t Index : {1u, 2u}) {
            const auto* Binding = FindBinding(Relocated.CompiledShaders[Index], "Params");
            ASSERT_NE(Binding, nullptr);
            EXPECT_EQ(Binding->SetIndex, 2u);
            EXPECT_EQ(Binding->BindingIndex, 11u);
            EXPECT_NE(*Binding, *FindBinding(Original.CompiledShaders[Index], "Params"));
        }
        // Carry the actual compiler allocation through shared output and the cooked library.
        auto Shared = Testing::WriteShaderOutput(Options, Relocated);
        ASSERT_FALSE(Shared.GetValues().empty());
        auto Warm = ShaderSharedOutput::Assemble(Options, Shared);
        ASSERT_TRUE(Warm) << FormatShaderCompilerErrors(Warm.Errors);
        for (size_t Index = 0; Index < 3; ++Index)
            EXPECT_EQ(Warm.CompiledShaders[Index].Reflection.ResourceBindings,
                Relocated.CompiledShaders[Index].Reflection.ResourceBindings);
        FByteBuffer Payload;
        ASSERT_TRUE(ShaderCompiledOutput::Encode(Options, Warm, Payload));
        FShaderCompilerOutput Decoded;
        ASSERT_TRUE(ShaderCompiledOutput::Decode(Payload, Options, Decoded));
        FShaderRuntimeRequest Request{.TargetPlatform = EShaderTargetPlatform::Win64,
            .TargetProfile = EShaderTargetProfile::Game, .Category = EShaderRuntimeRequestCategory::GlobalSet,
            .Owner = "ContactAllocationTest", .Name = "Contact.Allocation"};
        for (size_t Index = 0; Index < 3; ++Index)
            Request.Members.push_back({std::format("Contact{}", Index), Options.EntryPoints[Index], Options.Frequencies[Index]});
        const FShaderCookedLibraryRecord Record{Request, {.HashLow = 17}, Decoded};
        FByteBuffer LibraryBytes;
        ASSERT_TRUE(EncodeShaderCookedLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
            std::span(&Record, 1), LibraryBytes));
        FShaderCookedLibrary Library;
        ASSERT_TRUE(FShaderCookedLibrary::OpenBytes(std::make_shared<const FByteBuffer>(LibraryBytes),
            EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, std::span(&Request, 1), Library));
        FShaderCompilerOutput Runtime;
        ASSERT_TRUE(Library.Load(Request, Runtime));
        Check(Runtime);
        for (size_t Index = 0; Index < 3; ++Index)
            EXPECT_EQ(Runtime.CompiledShaders[Index].Reflection.ResourceBindings, Relocated.CompiledShaders[Index].Reflection.ResourceBindings);
        for (const auto& Product : Runtime.CompiledShaders) {
            FPipelineLayoutDesc Layout;
            ASSERT_TRUE(BuildPipelineLayoutFromReflection(std::span(&Product.Reflection, 1), Layout));
            const auto Native = ShaderFormat::BuildMetalBindingMap(Product.Frequency,
                ShaderFormat::BuildLogicalBindings(Product.Reflection), false);
            ASSERT_TRUE(Native);
            for (const auto& Resource : Product.Reflection.ResourceBindings) {
                ASSERT_LT(Resource.SetIndex, Layout.BindingLayouts.size());
                const auto& Set = Layout.BindingLayouts[Resource.SetIndex].BindingLayouts;
                const auto Logical = std::ranges::find(Set, Resource.BindingIndex, &FBindingLayoutItem::Slot);
                ASSERT_NE(Logical, Set.end());
                EXPECT_EQ(Logical->Type, Resource.Type);
                const auto Mapped = std::ranges::find_if(Native->Bindings, [&](const auto& B) {
                    return B.SetIndex == Resource.SetIndex && B.BindingIndex == Resource.BindingIndex;
                });
                ASSERT_NE(Mapped, Native->Bindings.end());
                EXPECT_EQ(Mapped->Type, Resource.Type);
                EXPECT_EQ(Mapped->Count, Resource.ArraySize);
            }
        }
        struct FReplayParameters {
            FRHITexture* GBufferMaterial; FRHITexture* GBufferNormals; FRHITexture* GBufferSurface;
            FRHITexture* SceneDepth; FRHIUniformBufferRange Params; FRHITexture* ContactVisibilityOutput;
        };
        const std::array Members{
            MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, FRHITexture*>("GBufferMaterial", offsetof(FReplayParameters, GBufferMaterial)),
            MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, FRHITexture*>("GBufferNormals", offsetof(FReplayParameters, GBufferNormals)),
            MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, FRHITexture*>("GBufferSurface", offsetof(FReplayParameters, GBufferSurface)),
            MakeShaderParameterMemberMetadata<ERHIBindingType::Texture, FRHITexture*>("SceneDepth", offsetof(FReplayParameters, SceneDepth)),
            MakeShaderParameterMemberMetadata<ERHIBindingType::UniformBuffer, FRHIUniformBufferRange>("Params", offsetof(FReplayParameters, Params)),
            MakeShaderParameterMemberMetadata<ERHIBindingType::StorageImage, FRHITexture*, true>("ContactVisibilityOutput", offsetof(FReplayParameters, ContactVisibilityOutput))};
        const auto Metadata = MakeInlineShaderParametersMetadata<FReplayParameters>("Replay", Members);
        for (size_t Index : {1u, 2u}) {
            FShaderType ReplayType(std::format("ContactReplay{}", Index), "/Unit/Replay", Options.Frequencies[Index],
                Options.EntryPoints[Index], {}, nullptr, nullptr, nullptr, &Metadata);
            FShader Shader(&ReplayType, nullptr, Runtime.CompiledShaders[Index].Reflection);
            const auto Bound = Shader.InitializeParameterBindings();
            ASSERT_TRUE(Bound) << Bound.error();
            const auto Bindings = Shader.GetParameterBindings();
            const auto Params = std::ranges::find(Bindings, "Params", &FShaderParameterBinding::Name);
            ASSERT_NE(Params, Bindings.end());
            EXPECT_EQ(Params->SetIndex, 2u); EXPECT_EQ(Params->BindingIndex, 11u);
            EXPECT_EQ(Params->Offset, offsetof(FReplayParameters, Params));
            EXPECT_EQ(Bindings.size(), Index == 1 ? 5u : 6u);
        }
    }

	TEST_F(FShaderReflectionTests, DeferredDirectionalLightingPublishesNamedInputs)
	{
		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR)
			/ "DeferredDirectionalLighting.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/DeferredDirectionalLighting";
		Options.EntryPoints = {"VertexMain", "FragmentMain"};
		Options.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment
		};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput Output =
			Compiler.Compile(ShaderPath.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 2u);
		const FShaderCode& Fragment = Output.CompiledShaders[1];
		EXPECT_EQ(GetSpirvOutputLocations(Fragment), (std::set<uint32>{0}));
		ASSERT_EQ(Fragment.Reflection.ResourceBindings.size(), 21u);
		ExpectResource(Fragment, "GBufferModelData", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		for (uint32 Index = 0; Index <= 7; ++Index)
		{
			const std::array<const char*, 8> Names{
				"GBufferMaterial", "GBufferNormals", "GBufferSurface",
				"BasePassColor", "SceneDepth", "EnvironmentIrradiance",
				"EnvironmentPrefiltered", "EnvironmentBrdfLut"
			};
			ExpectResource(Fragment, Names[Index], ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		}
		ExpectResource(Fragment, "EnvironmentSampler", ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "DirectionalShadowTexture", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "DirectionalShadowSampler", ERHIBindingType::Sampler, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "View", ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "Lighting", ERHIBindingType::UniformBuffer, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "GroundTruthAmbientOcclusionRaw", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "GroundTruthAmbientOcclusionFiltered", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "ContactVisibility", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "GroundTruthAmbientOcclusionResolved", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "GroundTruthAmbientOcclusionSelector", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "VolumetricCloudVisibility", ERHIBindingType::Texture, EShaderStageFlags::Fragment);
		ExpectResource(Fragment, "LocalLights", ERHIBindingType::StorageBuffer, EShaderStageFlags::Fragment);
	}
	TEST_F(FShaderReflectionTests, ProductionDeferredDoesNotSampleItsSceneColorAttachment)
	{
		const auto Path = std::filesystem::path(DURIN_ENGINE_SHADER_SOURCE_DIR) / "DeferredDirectionalLighting.slang";
		FShaderCompileOptions Options;
		Options.VirtualShaderPath = "/Engine/DeferredDirectionalLighting";
		Options.EntryPoints = {"ProductionFragmentMain"};
		Options.Frequencies = {EShaderFrequency::Fragment};
		Options.Macros.emplace_back("DURIN_DEFERRED_PRODUCTION", "1");
		const auto Output = FSlangShaderCompiler().Compile(Path.string(), Options);
		ASSERT_TRUE(Output) << FormatShaderCompilerErrors(Output.Errors);
		ASSERT_EQ(Output.CompiledShaders.size(), 1u);
		EXPECT_EQ(FindBinding(Output.CompiledShaders[0], "BasePassColor"), nullptr);
	}

} // namespace Durin
