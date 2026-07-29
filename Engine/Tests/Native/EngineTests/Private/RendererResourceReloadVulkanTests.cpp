#include <gtest/gtest.h>

#include "Console/ConsoleCommand.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "RendererResourceInvalidation.h"
#include "Shader/Shader.h"
#include "Shader/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		class FReloadTestVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FReloadTestVertexShader,
				FShader,
				"/RendererReloadTests/Reloadable",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FReloadTestFragmentShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FReloadTestFragmentShader,
				FShader,
				"/RendererReloadTests/Reloadable",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};

		struct FReloadTestPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FReloadTestVertexShader> VertexShader;
			TShaderRef<FReloadTestFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
		};

		auto WriteReloadShader(
			const std::filesystem::path& Path,
			std::string_view ColorExpression) -> void
		{
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream << R"([shader("vertex")]
float4 VertexMain(uint vertexID : SV_VertexID) : SV_Position
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };
    return float4(positions[vertexID], 0.0, 1.0);
}

[shader("fragment")]
float4 FragmentMain() : SV_Target
{
    return )" << ColorExpression << R"(;
}
)";
			ASSERT_TRUE(Stream.good());
		}

		auto WriteBrokenReloadShader(
			const std::filesystem::path& Path) -> void
		{
			std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
			Stream << "[shader(\"vertex\")] broken shader source\n";
			ASSERT_TRUE(Stream.good());
		}

		auto MakeReloadRenderTargetLayout() -> FRHIRenderTargetLayout
		{
			FRHIRenderTargetLayout Layout;
			Layout.NumColorRenderTargets = 1;
			auto& Color = Layout.ColorAttachments[0].RenderTarget;
			Color.Format = EPixelFormat::SRGBA8_UNORM;
			Color.LoadAction = ERHIRenderTargetLoadAction::Clear;
			Color.StoreAction = ERHIRenderTargetStoreAction::Store;
			Color.InitialLayout = ERHITextureLayout::Undefined;
			Color.InitialAccess = ERHIAccess::None;
			Color.FinalLayout = ERHITextureLayout::ShaderReadOnly;
			Color.FinalAccess = ERHIAccess::ShaderRead;
			return Layout;
		}

		auto MakeReloadError(
			ERenderResourceCreateErrorCategory Category,
			std::string Message) -> FRenderResourceCreateError
		{
			return {
				.Category = Category,
				.Context = "RendererReloadVulkanTest",
				.Identity = "controlled-shader",
				.Message = std::move(Message),
				.RetryDependencies =
					ERenderResourceGenerationDependency::Shader
						| ERenderResourceGenerationDependency::Device
						| ERenderResourceGenerationDependency::Manual,
			};
		}

		auto ExpectReloadColor(
			const std::vector<uint8>& Pixels,
			uint8 ExpectedRed,
			uint8 ExpectedGreen) -> void
		{
			ASSERT_EQ(Pixels.size(), 17u * 17u * 4u);
			const size_t Center = (8u * 17u + 8u) * 4u;
			EXPECT_NEAR(Pixels[Center], ExpectedRed, 8);
			EXPECT_NEAR(Pixels[Center + 1], ExpectedGreen, 8);
			EXPECT_NEAR(Pixels[Center + 2], 0, 8);
			EXPECT_NEAR(Pixels[Center + 3], 255, 8);
		}
	}

	TEST(FRendererResourceReloadVulkanTests,
		BrokenRefreshRetainsPipelineAndChangedReloadRecoversInProcess)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}

		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "RendererResourceReload";
		Testing::RemoveTestWorkDirectory(Root);
		const std::filesystem::path SourceRoot = Root / "Source";
		const std::filesystem::path CacheRoot = Root / "Cache";
		std::filesystem::create_directories(SourceRoot);
		std::filesystem::create_directories(CacheRoot);
		const std::filesystem::path ShaderPath =
			SourceRoot / "Reloadable.slang";
		WriteReloadShader(ShaderPath, "float4(1.0, 0.0, 0.0, 1.0)");
		FShaderPaths::RegisterMountPoint(
			"/RendererReloadTests/",
			SourceRoot.generic_string(),
			CacheRoot.generic_string());

		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit();
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		struct FBeginReloadValidationFrame
		{
			static constexpr auto GetName() -> const char*
			{
				return "BeginReloadValidationFrame";
			}
		};
		EnqueueRenderCommand<FBeginReloadValidationFrame>(
			[](FRHICommandListImmediate& CommandList) {
				CommandList.SwitchPipeline(ERHIPipeline::Graphics);
				GDynamicRHI->RHIBeginFrame();
			});

		FRendererModule Renderer;
		Renderer.StartupModule();
		TRenderResourceCreationSlot<FReloadTestPayload> Slot{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		std::vector<FRenderResourceCreateDiagnostic> Diagnostics;
		int Attempts = 0;
		std::vector<bool> ForceFlags;

		struct FResolveObservation
		{
			FReloadTestPayload* Payload = nullptr;
			FRendererResourceInvalidationSnapshot Snapshot;
			ERenderResourceAvailability Availability =
				ERenderResourceAvailability::Uninitialized;
		};
		auto Resolve = [&]() {
			auto Observation = std::make_shared<FResolveObservation>();
			struct FResolveReloadValidationResource
			{
				static constexpr auto GetName() -> const char*
				{
					return "ResolveReloadValidationResource";
				}
			};
			EnqueueRenderCommand<FResolveReloadValidationResource>(
				[&Slot, &Diagnostics, &Attempts, &ForceFlags, Observation](
					FRHICommandListImmediate&) {
					Observation->Snapshot =
						GetRendererResourceInvalidationSnapshot_RenderThread();
					using FResult =
						TRenderResourceCreateResult<FReloadTestPayload>;
					Observation->Payload = Slot.Resolve(
						Observation->Snapshot.Generation,
						[&Attempts, &ForceFlags,
						 Snapshot = Observation->Snapshot]() -> FResult {
							++Attempts;
							ForceFlags.push_back(
								Snapshot.bForceShaderRecompile);
							FShaderCompileOptions CompileOptions;
							CompileOptions.bForceRecompile =
								Snapshot.bForceShaderRecompile;
							FShaderType& VertexShaderType =
								FReloadTestVertexShader::StaticType();
							FShaderType& FragmentShaderType =
								FReloadTestFragmentShader::StaticType();
							std::array<const FShaderType*, 2> ShaderTypes = {
								&VertexShaderType,
								&FragmentShaderType,
							};
							auto ShaderMap =
								std::make_shared<FShaderMapBase>();
							std::string ErrorMessage;
							if (!ShaderMap->InitializeFromShaderTypes(
									ShaderTypes,
									CompileOptions,
									ErrorMessage))
							{
								return FResult::Failure(MakeReloadError(
									ERenderResourceCreateErrorCategory::
										ShaderCompile,
									std::move(ErrorMessage)));
							}
							auto* VertexShader =
								static_cast<FReloadTestVertexShader*>(
									ShaderMap->GetShader(&VertexShaderType));
							auto* FragmentShader =
								static_cast<FReloadTestFragmentShader*>(
									ShaderMap->GetShader(&FragmentShaderType));
							if (VertexShader == nullptr
								|| FragmentShader == nullptr)
							{
								return FResult::Failure(MakeReloadError(
									ERenderResourceCreateErrorCategory::
										ShaderBinding,
									"Compiled shader map is missing a typed "
									"shader."));
							}

							FReloadTestPayload Candidate;
							Candidate.ShaderMap = std::move(ShaderMap);
							Candidate.VertexShader =
								TShaderRef<FReloadTestVertexShader>(
									VertexShader,
									Candidate.ShaderMap.get());
							Candidate.FragmentShader =
								TShaderRef<FReloadTestFragmentShader>(
									FragmentShader,
									Candidate.ShaderMap.get());
							FVertexDeclarationElementList Elements{};
							Candidate.VertexDeclaration =
								GDynamicRHI->RHICreateVertexDeclaration(
									Elements);
							FGraphicsPipelineStateInitializer Initializer;
							Initializer.RenderTargetLayout =
								MakeReloadRenderTargetLayout();
							Initializer.BoundShaders.VertexShader =
								Candidate.VertexShader.GetRHIShader();
							Initializer.BoundShaders.FragmentShader =
								Candidate.FragmentShader.GetRHIShader();
							Initializer.VertexDeclaration =
								Candidate.VertexDeclaration;
							Initializer.bEnableAlphaBlend = false;
							Initializer.bEnableBackFaceCulling = false;
							Initializer.bEnableDepthTest = false;
							Initializer.bEnableDepthWrite = false;
							Initializer.PipelineLayout =
								Candidate.ShaderMap
									->GetMergedPipelineLayout();
							Candidate.PipelineState =
								GDynamicRHI
									->RHICreateGraphicsPipelineState(
										FName(std::format(
											"RendererReloadTestPipeline_{}",
											Snapshot.Generation.Shader)),
										Initializer);
							if (Candidate.VertexDeclaration == nullptr
								|| Candidate.PipelineState == nullptr)
							{
								return FResult::Failure(MakeReloadError(
									ERenderResourceCreateErrorCategory::
										GraphicsPipeline,
									"RHI pipeline creation returned null."));
							}
							return FResult::Success(
								std::move(Candidate));
						},
						[&Diagnostics](
							FRenderResourceCreateDiagnostic Diagnostic) {
							Diagnostics.push_back(
								std::move(Diagnostic));
						});
					Observation->Availability =
						Slot.GetAvailability();
				});
			FlushRenderingCommands();
			return Observation;
		};
		auto RenderPipeline =
			[](FGraphicsPipelineStateRHIRef PipelineState) {
				auto Pixels = std::make_shared<std::vector<uint8>>();
				struct FRenderReloadValidationResource
				{
					static constexpr auto GetName() -> const char*
					{
						return "RenderReloadValidationResource";
					}
				};
				EnqueueRenderCommand<FRenderReloadValidationResource>(
					[PipelineState = std::move(PipelineState), Pixels](
						FRHICommandListImmediate& CommandList) {
						FRHITextureCreateDesc ColorDesc =
							FRHITextureCreateDesc::Create2D(
								"RendererReloadValidationColor",
								17,
								17,
								EPixelFormat::SRGBA8_UNORM)
								.SetFlags(
									ETextureCreateFlags::RenderTargetable
									| ETextureCreateFlags::ShaderResource
									| ETextureCreateFlags::CPUReadback);
						FTextureRHIRef Color =
							GDynamicRHI->RHICreateTexture(
								CommandList,
								ColorDesc);
						ASSERT_NE(Color, nullptr);
						const std::array<uint32, 3> Indices = {0, 1, 2};
						FRHIBufferCreateDesc IndexDesc =
							FRHIBufferCreateDesc::CreateIndex(
								"RendererReloadValidationIndex",
								sizeof(Indices),
								sizeof(uint32));
						IndexDesc.Usage |= EBufferUsageFlags::Static;
						IndexDesc.InitialData = {
							Indices.data(),
							sizeof(Indices),
						};
						FBufferRHIRef IndexBuffer =
							GDynamicRHI->RHICreateBuffer(
								CommandList,
								IndexDesc);
						ASSERT_NE(IndexBuffer, nullptr);
						FRHIRenderPassInfo PassInfo{};
						PassInfo.RenderTargetLayout =
							MakeReloadRenderTargetLayout();
						PassInfo.ColorRenderTargets[0] = Color;
						PassInfo.ColorClearValues[0] =
							FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
						CommandList.BeginRenderPass(
							PassInfo,
							"RendererReloadValidationPass");
						CommandList.SetViewport(
							0.0f,
							0.0f,
							0.0f,
							17.0f,
							17.0f,
							1.0f);
						CommandList.SetScissor(
							0.0f,
							0.0f,
							17.0f,
							17.0f);
						CommandList.SetGraphicsPipelineState(
							*PipelineState);
						CommandList.BindIndexBuffer(
							IndexBuffer,
							0);
						CommandList.DrawIndexed(3, 0, 0);
						CommandList.EndRenderPass();
						ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
							CommandList,
							Color,
							0,
							0,
							*Pixels));
					});
				FlushRenderingCommands();
				return Pixels;
			};

		const auto Initial = Resolve();
		ASSERT_NE(Initial->Payload, nullptr);
		ASSERT_NE(Initial->Payload->PipelineState, nullptr);
		FRHIGraphicsPipelineState* InitialPipeline =
			Initial->Payload->PipelineState;
		EXPECT_EQ(Attempts, 1);
		EXPECT_TRUE(Diagnostics.empty());
		ASSERT_EQ(ForceFlags.size(), 1);
		EXPECT_FALSE(ForceFlags.back());
		ExpectReloadColor(
			*RenderPipeline(Initial->Payload->PipelineState),
			255,
			0);

		WriteBrokenReloadShader(ShaderPath);
		const FConsoleCommandResult BrokenChanged =
			FConsoleCommandRegistry::Get().Execute(
				"renderer.reload-shaders changed");
		ASSERT_TRUE(BrokenChanged.bSuccess) << BrokenChanged.Message;
		FlushRenderingCommands();
		const auto FailedRefresh = Resolve();
		ASSERT_NE(FailedRefresh->Payload, nullptr);
		EXPECT_EQ(
			FailedRefresh->Payload->PipelineState.GetReference(),
			InitialPipeline);
		EXPECT_EQ(
			FailedRefresh->Availability,
			ERenderResourceAvailability::StaleReady);
		EXPECT_EQ(Attempts, 2);
		ASSERT_EQ(Diagnostics.size(), 1);
		ASSERT_TRUE(Diagnostics.front().Error.has_value());
		EXPECT_TRUE(Diagnostics.front().Error->bRetainedFallback);
		ExpectReloadColor(
			*RenderPipeline(FailedRefresh->Payload->PipelineState),
			255,
			0);

		const auto SuppressedRefresh = Resolve();
		ASSERT_NE(SuppressedRefresh->Payload, nullptr);
		EXPECT_EQ(
			SuppressedRefresh->Payload->PipelineState.GetReference(),
			InitialPipeline);
		EXPECT_EQ(Attempts, 2);
		EXPECT_EQ(Diagnostics.size(), 1);

		WriteReloadShader(ShaderPath, "float4(0.0, 1.0, 0.0, 1.0)");
		std::filesystem::last_write_time(
			ShaderPath,
			std::filesystem::last_write_time(ShaderPath)
				+ std::chrono::seconds(2));
		const FConsoleCommandResult CorrectedChanged =
			FConsoleCommandRegistry::Get().Execute(
				"renderer.reload-shaders changed");
		ASSERT_TRUE(CorrectedChanged.bSuccess)
			<< CorrectedChanged.Message;
		FlushRenderingCommands();
		const auto Recovered = Resolve();
		ASSERT_NE(Recovered->Payload, nullptr);
		EXPECT_NE(
			Recovered->Payload->PipelineState.GetReference(),
			InitialPipeline);
		EXPECT_EQ(
			Recovered->Availability,
			ERenderResourceAvailability::Ready);
		EXPECT_EQ(Attempts, 3);
		ASSERT_EQ(Diagnostics.size(), 2);
		EXPECT_EQ(
			Diagnostics.back().Kind,
			ERenderResourceCreateDiagnosticKind::Recovery);
		ExpectReloadColor(
			*RenderPipeline(Recovered->Payload->PipelineState),
			0,
			255);

		const FConsoleCommandResult ForcedAll =
			FConsoleCommandRegistry::Get().Execute(
				"renderer.reload-shaders all");
		ASSERT_TRUE(ForcedAll.bSuccess) << ForcedAll.Message;
		FlushRenderingCommands();
		const auto Forced = Resolve();
		ASSERT_NE(Forced->Payload, nullptr);
		EXPECT_TRUE(Forced->Snapshot.bForceShaderRecompile);
		EXPECT_EQ(Attempts, 4);
		ASSERT_EQ(ForceFlags.size(), 4);
		EXPECT_TRUE(ForceFlags.back());

		struct FReleaseReloadValidationResource
		{
			static constexpr auto GetName() -> const char*
			{
				return "ReleaseReloadValidationResource";
			}
		};
		EnqueueRenderCommand<FReleaseReloadValidationResource>(
			[&Slot](FRHICommandListImmediate& CommandList) {
				Slot.Reset();
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
			});
		FlushRenderingCommands();
		Renderer.ShutdownModule();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(
			ERHIPipeline::None);
		RHIExit();
	}
} // namespace Durin
