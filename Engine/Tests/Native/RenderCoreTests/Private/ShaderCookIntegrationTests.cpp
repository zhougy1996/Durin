#include "Shader/IShaderBuildModule.h"

#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Shader/ShaderData.h"
#include "Shader/MaterialShader.h"
#include "VertexFactory.h"
#include "gtest/gtest.h"

namespace Durin
{
	TEST(FShaderCookIntegrationTests,
		ProducesDeterministicCompleteGameLibraryThroughModule)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		FShaderOperationResult Error;
		FShaderOperationResult ShaderResult;
		std::string MountError;
		ASSERT_TRUE(FMountPaths::InitDefaultMountPoints(&MountError)) << MountError;
		ASSERT_TRUE(FModuleManager::Get().LoadModule("RenderCore"));
		const FModuleHandle RendererHandle = FPlatformMisc::LoadLibrary(
			std::format("{}{}-Renderer{}", FPlatformMisc::FLibraryPrefix,
				DURIN_RUNTIME_VARIANT,
				FPlatformMisc::FLibraryExtension));
		ASSERT_NE(RendererHandle, nullptr) << FPlatformMisc::GetLastLibraryError();
		ASSERT_TRUE(FModuleManager::Get().LoadModule("ShaderBuild"));

		std::string CancelledIdentity = "stale";
		const auto CancelledCapture = GetShaderCookInputIdentity(CancelledIdentity, [] { return true; });
		EXPECT_EQ(CancelledCapture.error().Code, EShaderError::Cancelled);
		EXPECT_TRUE(CancelledIdentity.empty());


		const std::array<std::string_view, 4> FragmentNames{
			"FSurfaceFragmentShader", "FGBufferFragmentShader", "FSurfaceMaskedShadowFragmentShader", "FSurfaceOpaqueShadowFragmentShader"};
		std::array<const FShaderType*, 4> FragmentTypes{};
		std::array<std::vector<FCompiledShader>, 3> MaterialStages;
		for (size_t Pass = 0; Pass < FragmentNames.size(); ++Pass)
		{
			const auto& Types = FShaderType::GetTypeList();
			const auto Found = std::ranges::find_if(Types, [&](const auto* Type) { return Type->GetName() == FragmentNames[Pass]; });
			ASSERT_NE(Found, Types.end());
			FragmentTypes[Pass] = *Found;
			if (Pass == 3) continue;
			FShaderMapBase Compiled;
			FShaderCompileOptions Options;
			Options.bForceRecompile = true;
			Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", Pass == 2 ? "1" : "0");
			Options.Macros.emplace_back("DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", std::to_string(std::bit_cast<uint32>(0.4f)));
			ASSERT_TRUE((ShaderResult = Compiled.InitializeFromShaderTypes(std::span(&FragmentTypes[Pass], 1), Options))) << FormatShaderError(ShaderResult.error());
			MaterialStages[Pass].push_back(Compiled.GetCode()->GetCompiledShader(0));
		}

		Durin::FByteBuffer First;
		Durin::FByteBuffer Second;
		ASSERT_TRUE(([&] {
			return (Error = BuildCookedShaderLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, First))
				&& (Error = BuildCookedShaderLibrary(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Second));
		})()) << FormatShaderError(Error.error());
		EXPECT_EQ(First, Second);

		std::vector<FShaderRuntimeRequest> Requests;
		ASSERT_TRUE((Error = FreezeShaderRuntimeInventory(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Requests))) << FormatShaderError(Error.error());
		const size_t FactoryRequests = std::ranges::count_if(Requests,
			[](const auto& Request) { return Request.Owner == "MeshVertexFactory"; });
		EXPECT_EQ(FactoryRequests, 6u);
		EXPECT_GT(Requests.size(), FactoryRequests);
		FShaderCookedLibrary Library;
		ASSERT_TRUE((Error = FShaderCookedLibrary::OpenBytes(std::make_shared<const Durin::FByteBuffer>(First), EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Requests, Library))) << FormatShaderError(Error.error());
		EXPECT_EQ(Library.GetRecordCount(), Requests.size());

		std::vector<const FShaderType*> RuntimeTypes;
		ASSERT_TRUE((Error = GetShaderRuntimeRequestBuildTypes(Requests.front(), RuntimeTypes))) << FormatShaderError(Error.error());
		const std::filesystem::path CookRoot =
			Testing::GetTestWorkDirectory() / "CookedShaderRuntime";
		std::filesystem::create_directories(
			CookRoot / std::filesystem::path(ShaderCookedLibraryRelativePath).parent_path());
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(
			First, CookRoot / ShaderCookedLibraryRelativePath));
		EXPECT_FALSE(FModuleManager::Get().UnloadModule("ShaderBuild"));
		EXPECT_NE(IShaderBuildModule::Get(), nullptr);
		FModuleManager::Get().ShutdownModule("ShaderBuild");
		EXPECT_EQ(IShaderBuildModule::Get(), nullptr);
		ASSERT_TRUE((Error = InitializeShaderData(FShaderDataConfiguration::Cooked(
				std::filesystem::absolute(CookRoot).lexically_normal())))) << FormatShaderError(Error.error());
		for (const auto& Request : Requests)
		{
			ASSERT_TRUE((Error = GetShaderRuntimeRequestBuildTypes(Request, RuntimeTypes))) << FormatShaderError(Error.error());
			FShaderCompilerOutput RuntimeOutput;
			ASSERT_TRUE((Error = LoadCookedShaderRuntimeRequest(Request.Name, RuntimeTypes, RuntimeOutput))) << Request.Name << ": " << FormatShaderError(Error.error());
			EXPECT_TRUE(RuntimeOutput);
			if (Request.Owner == "MeshVertexFactory")
			{
				const auto& Factories = FVertexFactoryType::GetTypeList();
				const auto Factory = std::ranges::find_if(Factories, [&](const auto* Type) {
					return Request.Name.starts_with(std::format("MeshVertex.{}.", Type->GetName()));
				});
				ASSERT_NE(Factory, Factories.end());
				FMaterialShaderMap Map;
				ASSERT_TRUE((ShaderResult = FMaterialShaderMap::TryCompile({
					.Identity = {.ProgramIdentity = {.Digest = {.HashLow = 17, .HashHigh = 23}}},
					.Target = "vulkan-spirv-1.5",
					.VertexFactoryType = *Factory,
					.ShaderTypes = RuntimeTypes,
					.FixedShaderRuntimeRequest = Request.Name,
					.bCreateRHIShaders = false}, Map))) << Request.Name << ": " << FormatShaderError(ShaderResult.error());
				EXPECT_TRUE(Map);
				const size_t Pass = static_cast<size_t>(Request.Name.back() - '0');
				ASSERT_LT(Pass, 3u);
				const FMaterialProgramIdentity Program{.Digest = {.HashLow = 17, .HashHigh = 23}};
				const std::array<const FShaderType*, 2> ComposedTypes{RuntimeTypes.front(), FragmentTypes[Pass]};
				ASSERT_TRUE((ShaderResult = FMaterialShaderMap::TryCompile({
					.Identity = {.ProgramIdentity = Program}, .Target = "vulkan-spirv-1.5",
					.VertexFactoryType = *Factory, .MeshPassKey = static_cast<uint32>(Pass),
					.ShaderTypes = ComposedTypes, .FixedShaderRuntimeRequest = Request.Name,
					.GeneratedStages = MaterialStages[Pass], .CompiledProgramIdentity = Program,
					.CompiledTarget = "vulkan-spirv-1.5", .bCreateRHIShaders = false}, Map))) << Request.Name << ": " << FormatShaderError(ShaderResult.error());
				if (Pass == 2)
				{
					const std::array<const FShaderType*, 2> OpaqueTypes{RuntimeTypes.front(), FragmentTypes[3]};
					ASSERT_TRUE((ShaderResult = FMaterialShaderMap::TryCompile({
						.Identity = {.ProgramIdentity = Program}, .Target = "vulkan-spirv-1.5",
						.VertexFactoryType = *Factory, .MeshPassKey = 2,
						.ShaderTypes = OpaqueTypes, .FixedShaderRuntimeRequest = Request.Name,
						.FixedFragmentRuntimeRequest = "Surface.OpaqueShadow",
						.bCreateRHIShaders = false}, Map))) << FormatShaderError(ShaderResult.error());
				}
			}
		}
		ShutdownShaderData();
		FPlatformMisc::FreeLibrary(RendererHandle);
	}
}
