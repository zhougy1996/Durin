#include "Materials/MaterialCookedProgram.h"
#include "Renderers/MaterialSceneTextures.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Renderers/SceneViewPreparation.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Materials/MaterialShaderMap.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderMapPreparation.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Shader/ShaderCompilerCore.h"
#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

#include "VulkanEngineTestSupport.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Rendering/SceneInterface.h"
#include "ShaderPipelineCache.h"
#include "Threading/TaskRuntime.h"

namespace Durin
{
	class FMaterialSceneInputVertexShader final : public FGlobalShader
	{
	public:
		DURIN_DECLARE_GLOBAL_SHADER(FMaterialSceneInputVertexShader, FGlobalShader, "/Engine/PostProcess", EShaderFrequency::Vertex, "VertexMain");
	};
                class FRejectSceneAllocation final : public FRDGAllocator
                {
                public:
                    auto Allocate(std::span<const FRDGAllocationRequest>, FRDGAllocatedResources&) -> FRDGAllocationResult override
                    { return std::unexpected(FRDGAllocationError{FRDGAllocationFailure{.Reason = ERDGAllocationError::PhysicalAllocationFailed}}); }
                };
	DURIN_IMPLEMENT_GLOBAL_SHADER(FMaterialSceneInputVertexShader);
}
TEST(FMaterialSceneTextureVulkanTests, SourceFreeInputsOwnRDGLifetimesAcrossFilteringSubrectResizeAndDepthConventions)
{
	using namespace Durin;
	using Op = EMaterialProgramOpcode; using Type = EMaterialProgramValueType;
	ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FTasks { ~FTasks() { ShutdownTaskScheduler(); } } Tasks;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true;
	FModuleManager::Get().LoadModuleChecked("ShaderBuild");
	FShaderMapBase VertexMap;
	const std::array<const FShaderType*,1> VertexTypes{&FMaterialSceneInputVertexShader::StaticType()};
	ASSERT_TRUE(InitializeShaderMapFromTypesSync(VertexMap, VertexTypes, {}));
	const auto VertexCode = VertexMap.GetCode()->GetCompiledShader(*VertexMap.FindShaderIndex(VertexTypes[0]));
	std::array<std::shared_ptr<const FMaterialProgram>, 6> Programs;
	for (uint32 Mode = 0; Mode < Programs.size(); ++Mode)
	{
		const bool Depth = Mode == 1 || Mode == 5;
		SCOPED_TRACE(Mode);
		MIR::FCompilerInput Input; Input.IR.Domain = EMaterialDomain::PostProcess;
		Input.StaticProperties.ShadingModel = EMaterialShadingModel::Unlit;
		const auto EnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Input.Environment);
		ASSERT_TRUE(EnvironmentResult) << EnvironmentResult.error();
		Input.IR.Nodes = {
			{.Opcode = Op::ScreenPosition, .ResultType = Type::Float2, .LegalStages = EMaterialEvaluationStage::Pixel,
				.SpatialKind = EMaterialSpatialKind::ScreenCoordinate, .CoordinateSpace = EMaterialCoordinateSpace::Screen},
			{.Opcode = Depth ? Op::SceneDepth : Op::SceneTexture, .ResultType = Depth ? Type::Float3 : Type::Float4,
				.LegalStages = EMaterialEvaluationStage::Pixel, .Inputs = {0},
				.Payload = MIR::FSceneTextureRead{Depth ? EMaterialSceneTextureId::SceneDepth : EMaterialSceneTextureId::PostProcessInput0,
					Depth || Mode == 2 ? EMaterialSceneTextureFilter::Point : EMaterialSceneTextureFilter::Linear}}};
		uint32 Color = 1;
		if (Mode >= 2) Input.IR.Nodes[0] = {.Opcode = Op::Constant, .ResultType = Type::Float2, .Payload = FMaterialProgramLiteral{Mode == 4 ? -1.f : .5f, Mode == 4 ? 2.f : .5f}};
		if (!Depth) { Color = 2; Input.IR.Nodes.push_back({.Opcode = Op::Swizzle, .ResultType = Type::Float3,
			.LegalStages = EMaterialEvaluationStage::Pixel, .Inputs = {1}, .Payload = MIR::FSwizzle{3,{0,1,2}}}); }
		auto& Root = Input.IR.GetAttribute(EMaterialProperty::MP_EmissiveColor); Root.bExpression = true; Root.ExpressionIndex = Color;
		const auto Compiled = MIR::Compile(Input); ASSERT_TRUE(Compiled) << FormatMaterialProgramDiagnostics(Compiled.Diagnostics);
		FByteBuffer Bytes; const std::array<const FMaterialProgram*,1> Family{&Compiled.Resource};
		const auto Encoded = EncodeMaterialCookedProgramFamily(Family,Input.StaticProperties,ECookTargetPlatform::Win64,ECookTargetProfile::Game,Bytes); ASSERT_TRUE(Encoded) << Encoded.error();
		FMaterialStaticProperties Properties;
		const auto Decoded = DecodeMaterialCookedProgramFamily(Bytes,ECookTargetPlatform::Win64,ECookTargetProfile::Game,Compiled.Resource.Quality,
			Compiled.Resource.FeatureLevel,Compiled.Resource.StaticBools,Properties,Programs[Mode]); ASSERT_TRUE(Decoded) << Decoded.error();
		ASSERT_TRUE(Programs[Mode]); EXPECT_TRUE(Programs[Mode]->GetDebugData().IR.Nodes.empty()); EXPECT_TRUE(Programs[Mode]->GetDebugData().GeneratedSource.empty());
	}
	FModuleManager::Get().ShutdownModule("ShaderBuild"); // GPU execution consumes cooked metadata and code only.
	RHIInit(Tests::GetVulkanEngineTestInitializationContext()); ASSERT_NE(GDynamicRHI, nullptr); InitRenderingThread();
	struct FCleanup { ~FCleanup() { ShutdownRenderingThread(); FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None); RHIExit(); FModuleManager::Get().ShutdownModule("ShaderBuild"); } } Cleanup;
	EnqueueRenderCommand("MaterialSceneInputsRDG", [&](FRHICommandListImmediate& Commands) {
		Commands.SwitchPipeline(ERHIPipeline::Graphics);
		FRendererResourceCoordinator Coordinator; FRendererRDGAllocator Allocator(Coordinator);
		FFullscreenGeometryResources Geometry; ASSERT_TRUE(Geometry.EnsureResources_RenderThread(Commands));
		for (const uint32 Extent : {8u, 12u})
		for (const uint32 MaterialMode : {0u,1u,2u,3u,4u,5u})
		for (const bool Reversed : {false, true})
		for (const bool Clear : {false, true})
		{
			const bool Depth = MaterialMode == 1 || MaterialMode == 5;
			SCOPED_TRACE(std::format("extent {} mode {} reversed {} clear {}", Extent, MaterialMode, Reversed, Clear));
			++GRenderFrameCounterRenderThread; GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
			const auto Input0 = GDynamicRHI->RHICreateTexture(Commands, FRHITextureCreateDesc::Create2D("MaterialInput0", Extent, Extent, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::DestinationCopy)); ASSERT_TRUE(Input0);
			std::vector<uint16> Pixels(Extent*Extent*4);
			for (uint32 Y=0; Y<Extent; ++Y) for (uint32 X=0; X<Extent; ++X)
			{
				const auto Pack = [&](uint32 C, float V) { Pixels[(Y*Extent+X)*4+C] = uint16(glm::packHalf2x16(FVector2f(V,0))); };
				Pack(0,float(X)/Extent); Pack(1,float(Y)/Extent); Pack(2,.25f); Pack(3,.5f);
			}
			Commands.UpdateTexture2D(Input0,0,0,{0,0,0,0,Extent,Extent},Extent*8,std::as_bytes(std::span(Pixels)));
			Commands.TransitionTextures(std::array{FRHITextureTransition{Input0,{ERHITextureAspect::Color,0,1,0,1},ERHIAccess::TransferWrite,ERHIAccess::GraphicsShaderRead}});
			const auto DepthTexture = GDynamicRHI->RHICreateTexture(Commands, FRHITextureCreateDesc::Create2D("MaterialDepth",Extent,Extent,EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::ShaderResource)); ASSERT_TRUE(DepthTexture);
			const float Device = Clear ? (Reversed ? 0.f : 1.f) : (Reversed ? .25f : .75f);
			FRHIRenderPassInfo DepthPass; DepthPass.RenderTargetLayout.bHasDepthStencil = true;
			DepthPass.RenderTargetLayout.DepthStencilAttachment.Format = EPixelFormat::D32;
			DepthPass.RenderTargetLayout.DepthStencilAttachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			DepthPass.RenderTargetLayout.DepthStencilAttachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
			DepthPass.DepthStencilRenderTarget = DepthTexture; DepthPass.DepthStencilClearValue = FClearValueBinding(Device,0u);
			Commands.BeginRenderPass(DepthPass,"MaterialDepthClear"); Commands.EndRenderPass();
			FRDGBuilder Graph;
			const auto ColorHandle = Graph.RegisterExternalTexture(Input0,"Scene.Input0",ERHIAccess::GraphicsShaderRead,ERHIAccess::GraphicsShaderRead);
			const auto DepthHandle = Graph.RegisterExternalTexture(DepthTexture,"Scene.Depth",ERHIAccess::GraphicsShaderRead,ERHIAccess::GraphicsShaderRead);
			FMaterialSceneTextureInputs Inputs;
			Inputs.PostProcessInput0 = FScreenPassTexture{ColorHandle,{2,1},{4,4}};
			Inputs.SceneDepth = FScreenPassTexture{DepthHandle,{1,2},{4,4}};
			Inputs.ClearDepth = Reversed ? 0.f : 1.f;
			Inputs.ProjectionRows = {FVector4f(1,0,0,0),FVector4f(0,1,0,0),FVector4f(.1f,0,Reversed ? .1f : -1.1f,Reversed ? 1.1f : -1.1f),FVector4f(0,0,-1,0)};
			const auto& Program = *Programs[MaterialMode]; const auto& Entry = Program.GetShaderMap().Entries[0];
			auto Missing = Inputs; Missing.PostProcessInput0.reset(); EXPECT_FALSE(PlanMaterialSceneTextures(Graph,Entry,Missing));
			Missing = Inputs; Missing.SceneDepth.reset(); if (Depth) EXPECT_FALSE(PlanMaterialSceneTextures(Graph,Entry,Missing));
			Missing = Inputs; Missing.PostProcessInput0->ViewSize = {0,4}; EXPECT_FALSE(PlanMaterialSceneTextures(Graph,Entry,Missing));
			FRDGBuilder Other; Missing = Inputs; Missing.PostProcessInput0->Texture = Other.CreateTexture({.Texture=FRHITextureCreateDesc::Create2D("Foreign",8,8,EPixelFormat::RGBA16_FLOAT)},"Foreign");
			EXPECT_FALSE(PlanMaterialSceneTextures(Graph,Entry,Missing));
			auto Planned = PlanMaterialSceneTextures(Graph,Entry,Inputs); ASSERT_TRUE(Planned) << Planned.error();
			auto Plan = std::move(*Planned); EXPECT_EQ(Plan.TextureCount,Depth ? 2u : 1u); EXPECT_TRUE(Plan.bLinearSampler); EXPECT_EQ(Plan.bPointSampler,Depth || MaterialMode == 2);
			EXPECT_NE(Plan.Output.Texture,ColorHandle); EXPECT_NE(Plan.Output.Texture,DepthHandle);
            if (Extent == 8 && Depth && !Reversed && !Clear)
            {
                FRejectSceneAllocation Reject;
                FRDGBuilder FailedGraph;
                auto FailedInputs = Inputs;
                FailedInputs.PostProcessInput0->Texture = FailedGraph.RegisterExternalTexture(Input0,"Input0",ERHIAccess::GraphicsShaderRead,ERHIAccess::GraphicsShaderRead);
                FailedInputs.SceneDepth->Texture = FailedGraph.RegisterExternalTexture(DepthTexture,"Depth",ERHIAccess::GraphicsShaderRead,ERHIAccess::GraphicsShaderRead);
                auto FailedPlan = PlanMaterialSceneTextures(FailedGraph,Entry,FailedInputs); ASSERT_TRUE(FailedPlan);
                bool Recorded = false;
                const auto FailedPass = FailedGraph.AddRasterPass("FailSceneAllocation",std::move(FailedPlan->Parameters),[&](FRHICommandListImmediate&,const FMaterialSceneTexturePassParameters&,const FRDGParameterResolver&) { Recorded = true; });
                FailedGraph.MarkPassRoot(FailedPass,"failure-proof");
                FTextureRHIRef FailedOutput; FailedGraph.QueueTextureExtraction(FailedPlan->Output.Texture,&FailedOutput,ERHIAccess::GraphicsShaderRead);
                const auto Failed = FailedGraph.Execute(Commands,&Reject);
                EXPECT_EQ(GetRDGExecutionStatus(Failed),ERDGExecutionStatus::PreparationFailed);
                EXPECT_FALSE(Recorded); EXPECT_FALSE(FailedOutput);
            }
			const auto& PixelCode = Program.GetShaderMap().CompiledShaders[0];
			const auto MakeShader = [&](const FShaderCode& Code) { auto Desc=FRHIShaderCreateDesc::Create("SceneInputShader",Code.Frequency,Code.Code,Code.CodeHash); Desc.SetEntryPoint(Code.BinaryEntryPoint.c_str()); return GDynamicRHI->RHICreateShader(Desc); };
			const auto Vertex=MakeShader(VertexCode), Fragment=MakeShader(PixelCode); ASSERT_TRUE(Vertex); ASSERT_TRUE(Fragment);
			FGraphicsPipelineStateInitializer Init; Init.BoundShaders={Vertex,Fragment};
			Init.VertexDeclaration=Geometry.GetVertexDeclaration_RenderThread();
			Init.RasterizerState.CullMode=ERHICullMode::None;
			Init.RenderTargetLayout.NumColorRenderTargets=1;
			Init.RenderTargetLayout.ColorAttachments[0].RenderTarget=MakeRDGAttachmentLayout(EPixelFormat::RGBA16_FLOAT,ERHIAccess::ColorAttachmentReadWrite,ERHIRenderTargetLoadAction::DontCare);
			const std::array Codes{VertexCode,PixelCode}; ASSERT_TRUE(BuildPipelineLayoutFromShaders(Codes,Init.PipelineLayout));
			const auto Pipeline=GDynamicRHI->RHICreateGraphicsPipelineState("MaterialSceneInput",Init); ASSERT_TRUE(Pipeline);
			const auto Uniform=Plan.Uniform;
			const auto Pass=Graph.AddRasterPass("Material.SceneTextures",std::move(Plan.Parameters),[&](FRHICommandListImmediate& Cmd,const FMaterialSceneTexturePassParameters& P,const FRDGParameterResolver& Resolver) {
				RendererPrivate::FMaterialViewUniform View; View.Viewport={0,0,4,4}; View.ReciprocalViewport={.25f,.25f,0,0};
				const auto ViewBuffer=Cmd.CreateUniformBufferRange(&View,sizeof(View));
				auto SceneBindings=ResolveMaterialSceneTextureBindings(Cmd,PixelCode.Reflection,P,Resolver,Uniform); ASSERT_TRUE(SceneBindings) << SceneBindings.error();
				auto Bindings=SceneBindings->Parameters;
				Bindings.push_back({.Resource=ViewBuffer.Buffer,.SetIndex=0,.BindingIndex=0,.Type=ERHIBindingType::UniformBuffer,.Offset=ViewBuffer.Offset,.Size=ViewBuffer.Size});
				Cmd.SetGraphicsPipelineState(*Pipeline); Cmd.SetViewport(0,0,0,4,4,1); Cmd.SetScissor(0,0,4,4); Cmd.SetShaderParameters(Fragment,Bindings); Cmd.BindVertexBuffer(0,Geometry.GetVertexBuffer_RenderThread(),0); Cmd.BindIndexBuffer(Geometry.GetIndexBuffer_RenderThread(),0); Cmd.DrawIndexed(3,0,0);
			}); Graph.MarkPassRoot(Pass,"pixel-proof");
			FTextureRHIRef Output; Graph.QueueTextureExtraction(Plan.Output.Texture,&Output,ERHIAccess::TransferRead);
			const auto Executed=Graph.Execute(Commands,&Allocator); ASSERT_TRUE(Executed) << ToString(Executed.error()); ASSERT_TRUE(Output);
			const auto Readback=GDynamicRHI->RHICreateTexture(Commands,FRHITextureCreateDesc::Create2D("SceneInputReadback",4,4,EPixelFormat::RGBA16_FLOAT).SetFlags(ETextureCreateFlags::CPUReadback|ETextureCreateFlags::DestinationCopy));
			Commands.TransitionTextures(std::array{FRHITextureTransition{Readback,{ERHITextureAspect::Color,0,1,0,1},ERHIAccess::Discard,ERHIAccess::TransferWrite}});
			FRHITextureCopyRegion Copy; Copy.Extent={4,4,1}; Commands.CopyTexture(Output,Readback,std::array{Copy});
			FByteBuffer Result; ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands,Readback,0,0,Result)); ASSERT_EQ(Result.size(),128u);
			for(uint32 Y=0;Y<4;++Y) for(uint32 X=0;X<4;++X) {
				std::array<uint16,4> Value; std::memcpy(Value.data(),Result.data()+(Y*4+X)*8,8);
				const auto Channel=[&](uint32 C) { return glm::unpackHalf2x16(Value[C]).x; };
				EXPECT_NEAR(Channel(0),Depth ? Device : (MaterialMode == 2 ? 4.f : MaterialMode == 3 ? 3.5f : MaterialMode == 4 ? 2.f : float(X+2))/Extent,.002f);
				EXPECT_NEAR(Channel(1),Depth ? (Clear ? 0.f : 1.1f/(.35f+(Reversed ? -.1f : .1f)*(MaterialMode == 5 ? .25f : (float(X)+.5f)/2.f-1.f))) : (MaterialMode == 2 ? 3.f : MaterialMode == 3 ? 2.5f : MaterialMode == 4 ? 4.f : float(Y+1))/Extent,.01f);
				EXPECT_NEAR(Channel(2),Depth ? (Clear ? 0.f : 1.f) : .25f,.002f); EXPECT_NEAR(Channel(3),.5f,.002f); }
			GDynamicRHI->RHIEndFrame_RenderThread(Commands);
		}
		Allocator.Release_RenderThread(); Geometry.ReleaseResources_RenderThread();
	}); FlushRenderingCommands();
}
