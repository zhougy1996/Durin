#include "Shader/ShaderBuildProvider.h"

#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Shader/ShaderData.h"
#include "gtest/gtest.h"

namespace Durin
{
	TEST(FShaderCookIntegrationTests,
		ProducesDeterministicCompleteGameLibraryThroughProvider)
	{
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		std::string Error;
		ASSERT_TRUE(PathUtilities::InitDefaultMountPoints(&Error)) << Error;
		ASSERT_TRUE(FModuleManager::Get().LoadModule("RenderCore"));
		const FModuleHandle RendererHandle = FPlatformMisc::LoadLibrary(
			std::format("{}-Renderer{}", DURIN_RUNTIME_VARIANT,
				FPlatformMisc::FLibraryExtension));
		ASSERT_NE(RendererHandle, nullptr) << FPlatformMisc::GetLastLibraryError();
		ASSERT_TRUE(FModuleManager::Get().LoadModule("ShaderBuild"));

		std::vector<std::byte> First;
		std::vector<std::byte> Second;
		ASSERT_TRUE(BuildCookedShaderLibrary(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			First, Error)) << Error;
		ASSERT_TRUE(BuildCookedShaderLibrary(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			Second, Error)) << Error;
		EXPECT_EQ(First, Second);

		std::vector<FShaderRuntimeRequest> Requests;
		ASSERT_TRUE(FreezeShaderRuntimeInventory(
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			Requests, Error)) << Error;
		EXPECT_EQ(Requests.size(), 15u);
		FShaderCookedLibrary Library;
		ASSERT_TRUE(FShaderCookedLibrary::OpenBytes(
			std::make_shared<const std::vector<std::byte>>(First),
			EShaderTargetPlatform::Win64, EShaderTargetProfile::Game,
			Requests, Library, Error)) << Error;
		EXPECT_EQ(Library.GetRecordCount(), Requests.size());

		std::vector<const FShaderType*> RuntimeTypes;
		ASSERT_TRUE(GetShaderRuntimeRequestBuildTypes(
			Requests.front(), RuntimeTypes, Error)) << Error;
		const std::filesystem::path CookRoot =
			Testing::GetTestWorkDirectory() / "CookedShaderRuntime";
		std::filesystem::create_directories(
			CookRoot / std::filesystem::path(ShaderCookedLibraryRelativePath).parent_path());
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(
			First, CookRoot / ShaderCookedLibraryRelativePath));
		EXPECT_TRUE(FModuleManager::Get().UnloadModule("ShaderBuild").Succeeded());
		ShutdownShaderData();
		ASSERT_TRUE(InitializeShaderData(
			FShaderDataConfiguration::Cooked(
				std::filesystem::absolute(CookRoot).lexically_normal()), Error)) << Error;
		FShaderCompilerOutput RuntimeOutput;
		ASSERT_TRUE(LoadCookedShaderRuntimeRequest(
			Requests.front().Name, RuntimeTypes, RuntimeOutput, Error)) << Error;
		EXPECT_TRUE(RuntimeOutput);
		ShutdownShaderData();
		FPlatformMisc::FreeLibrary(RendererHandle);
	}
}
