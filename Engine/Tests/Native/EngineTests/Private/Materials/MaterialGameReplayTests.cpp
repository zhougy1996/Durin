#include "CoreMinimal.h"
#include "EngineTestSupport.h"
#include "MaterialResourceTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialShaderMap.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/ShaderData.h"
#include "RendererModule.h"
#include "HAL/PlatformProcess.h"

#include <gtest/gtest.h>

// Consumes the Editor Cook target's exported corpus in a runtime-only process.
TEST(FMaterialGameReplayTests, IsolatedCorpusLoadsAndSharesWithoutCompilerModules)
{
	using namespace Durin;
	// The registered runner can exercise a copy beside an exported Game's DLLs.
	if (const auto* Executable=std::getenv("DURIN_MATERIAL_GAME_REPLAY_EXECUTABLE"))
	{
		const auto Installed=std::filesystem::canonical(Executable);
		if (!std::filesystem::equivalent(Installed,FPlatformProcess::ExecutablePath()))
		{
			std::cout << "Isolated Game replay: " << Installed.generic_string() << '\n';
			FProcessLaunchOptions Options;
			Options.Arguments={"--gtest_filter=FMaterialGameReplayTests.*","--gtest_brief=1"};
			Options.WorkingDirectory=Installed.parent_path();
			std::string Output;
			const auto Deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
			FProcessExecutionControl Control;
			Control.IsCancelled=[&] { return std::chrono::steady_clock::now()>=Deadline; };
			Control.OnOutput=[&](std::string_view Text) { Output.append(Text); std::cout << Text; };
			const auto Result=FPlatformProcess::ExecuteControlledProcess(Installed.generic_string(),Options,Control);
			ASSERT_TRUE(Result) << Result.error().ToString();
			EXPECT_EQ(*Result,0) << Output;
			return;
		}
	}
	const auto* Directory=std::getenv("DURIN_MATERIAL_GAME_CORPUS_DIR");
	ASSERT_NE(Directory,nullptr) << "Export MaterialCookTests' corpus before running Game replay.";
	const auto Root=std::filesystem::absolute(Directory);
	const auto Evidence=FFileHelper::LoadFileToString(Root/"cook-contracts.csv");
	ASSERT_TRUE(Evidence);
	std::istringstream Rows(*Evidence);
	std::string Row;
	ASSERT_TRUE(bool(std::getline(Rows,Row)));
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("MaterialGameReplay");
	Lifecycle.Start(Renderer);
	const std::shared_ptr<void> Cleanup(nullptr,[&](void*) {
		ShutdownAssetRuntime();
		CollectGarbage(EObjectFlags::Standalone);
		Lifecycle.Shutdown();
		ShutdownRenderingThread();
		ShutdownShaderData();
	});
	ASSERT_TRUE(InitializeShaderData(FShaderDataConfiguration::Cooked(Root)));
	ShutdownAssetRuntime();
	const std::array Mounts{FMountPoint{.VirtualRoot="/Game/",.Owner=EMountOwner::Test,
		.Root=Root/"Cooked/Game",.bAutoScan=true}};
	Testing::FScopedMountRegistryFixture CookMounts(Mounts);
	ASSERT_TRUE(CookMounts.IsValid());
	auto Runtime=FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Root/"Cooked",Runtime));
	const auto Initialized=InitializeAssetRuntime(std::move(Runtime));
	ASSERT_TRUE(Initialized) << Initialized.error().ToString();
	{
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		const std::array Names{"M_Specular","M_ClearCoat","M_Unlit","M_PostNeutral","M_PostColor","M_PostDepth","M_Cube","M_CubeWPO"};
		for (const auto* Name:Names)
		{
			SCOPED_TRACE(Name);
			ASSERT_TRUE(bool(std::getline(Rows,Row)));
			std::istringstream Cells(Row);
			std::array<std::string,10> Expected;
			for (auto& Cell:Expected) ASSERT_TRUE(bool(std::getline(Cells,Cell,',')));
			ASSERT_EQ(Expected[0],Name);
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate(std::string("/Game/MaterialContracts/")+Name,Path));
			auto* Material=LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
			ASSERT_NE(Material,nullptr);
			Material->CacheResourceShadersForRendering(ERHIFeatureLevel::SM5,GetMaterialQualityLevel(),EMaterialShaderPrecompileMode::Synchronous);
			FShaderCompilingManager::Get().FinishCompilationForObjects(std::array<DObject*,1>{Material});
			const auto Program=Material->GetAcceptedCompiledProgram();
			ASSERT_TRUE(Program) << Material->GetMaterialCookDiagnostic();
			EXPECT_TRUE(Program->GetDebugData().IR.Nodes.empty());
			EXPECT_TRUE(Program->GetDebugData().GeneratedSource.empty());
			const auto& Map=Program->GetShaderMap();
			EXPECT_EQ(Map.ShaderMapId.ToString(),Expected[2]);
			EXPECT_EQ(Map.Entries.size(),std::stoull(Expected[5]));
			uint64 ShaderBytes=0,Bindings=0;
			for (const auto& Shader:Map.CompiledShaders) ShaderBytes+=Shader.Code.size();
			for (const auto& Entry:Map.Entries) Bindings+=Entry.Bindings.size();
			EXPECT_EQ(ShaderBytes,std::stoull(Expected[7]));
			EXPECT_EQ(Bindings,std::stoull(Expected[9]));
			ASSERT_FALSE(Map.Entries.empty());
			for (const auto& Entry:Map.Entries)
			{
				EXPECT_NE(Entry.Shader.ShaderType,"FHitProxyFragmentShader");
				EXPECT_NE(Entry.ParameterSchemaIdentity,FXxHash128{});
				EXPECT_NE(Entry.ParameterMapIdentity,FXxHash128{});
			}
			const auto Data=Testing::CaptureMaterialRenderData(*Material);
			ASSERT_FALSE(Data.IsError());
			auto* Instance=DMaterialInstance::CreateDynamic(Material,Material,"ReplayInstance");
			ASSERT_NE(Instance,nullptr);
			EXPECT_EQ(Instance->GetAcceptedCompiledProgram(),Program);
			const auto Field=std::ranges::find(Map.Layout.Fields,EMaterialRenderValueType::Vector4,&FMaterialRenderField::Type);
			if (Field!=Map.Layout.Fields.end())
			{
				ASSERT_TRUE(Instance->SetParameterValueByInfo(Program->FieldParameters[Field - Map.Layout.Fields.begin()],FMaterialParameterValue(FVector4(.8,1,2,1))));
				const auto Updated=Testing::CaptureMaterialRenderData(*Instance);
				float Value;
				std::memcpy(&Value,Updated.Representation.GetUniformPayload().data()+Field->Offset,sizeof(Value));
				EXPECT_FLOAT_EQ(Value,.8f);
				EXPECT_EQ(Instance->GetAcceptedCompiledProgram(),Program);
			}
			ASSERT_TRUE(UnloadPackage(Path));
		}
	}
	EXPECT_EQ(GetShaderDataDomain(),EShaderDataDomain::Cooked);
	EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("ShaderBuild"));
	EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("ShaderFormat"));
}
