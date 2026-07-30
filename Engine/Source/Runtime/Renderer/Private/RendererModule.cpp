#include "RendererModule.h"
#include "Profiling/Profiling.h"

#include "CoreGlobals.h"
#include "DefaultTextures.h"
#include "RendererEditorAssistance.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/TextureCubeThumbnailRenderer.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "RenderResourceCreation.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Console/ConsoleCommand.h"

#include <glm/mat4x4.hpp>

namespace Durin
{
	// Groups shared owners until FSceneRenderer assumes their composition.
	struct FRendererModule::FSharedResources
	{
		FSharedResources()
			: SkyBoxRenderer(Coordinator, DefaultTextures)
			, TextureCubeThumbnailRenderer(Coordinator)
			, PostProcessRenderer(Coordinator, FullscreenGeometry)
		{
		}

		FRendererResourceCoordinator Coordinator;
		FDefaultTextureResources DefaultTextures;
		FFullscreenGeometryResources FullscreenGeometry;
		FSkyBoxRenderer SkyBoxRenderer;
		FTextureCubeThumbnailRenderer TextureCubeThumbnailRenderer;
		FPostProcessRenderer PostProcessRenderer;
	};

	namespace
	{
		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent
				? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}

		class FStaticMeshVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FStaticMeshVertexShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Vertex, "VertexMain");
		};

		class FStaticMeshFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(BaseColorTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(BaseColorSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FStaticMeshFragmentShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Fragment, "FragmentMain");
		};

		struct FStaticMeshTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
			glm::mat4 LocalToWorld{1.0f};
			glm::mat4 NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FStaticMeshLightingUniform
		{
			FVector4f LightDirection{-0.5f, -0.5f, -1.0f, 0.0f};
			FVector4f LightColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
			FVector4f ViewPositionAmbient{0.0f, 0.0f, 0.0f, 0.08f};
		};

		struct FStaticMeshMaterialUniform
		{
			FVector4f BaseColor{1.0f};
			FVector4f Params{0.35f, 32.0f, 1.0f, 0.0f};
		};

		struct FStaticMeshRendererState
		{
			struct FBaseResources
			{
				FSamplerRHIRef BaseColorSampler;
			};

			struct FShaderMapPayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FStaticMeshVertexShader> VertexShader;
				TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			};

			struct FPipelinePayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FStaticMeshVertexShader> VertexShader;
				TShaderRef<FStaticMeshFragmentShader> FragmentShader;
				FGraphicsPipelineStateRHIRef SolidPipelineState;
				FGraphicsPipelineStateRHIRef WireframePipelineState;
			};

			TRenderResourceCreationSlot<FBaseResources> BaseResources{
				ERenderResourceGenerationDependency::Device};
			TRendererResourceSlotCache<
				FMaterialShaderMapIdentity,
				FShaderMapPayload>
				ShaderMaps{ERenderResourceGenerationDependency::Shader};
			TRendererResourceSlotCache<
				FMaterialPipelineIdentity,
				FPipelinePayload>
				Pipelines{
					ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		FStaticMeshRendererState GStaticMeshState;

		auto ConfigureRendererShaderCompileOptions(
			FShaderCompileOptions& CompileOptions) -> void
		{
			CompileOptions.bForceRecompile =
				GetRendererResourceCoordinator()
					.ShouldForceShaderRecompile_RenderThread();
		}

		auto GetStaticMeshIdentityText(
			const FMaterialShaderMapIdentity& Identity) -> std::string
		{
			return std::format(
				"schema={},blend={},shading={},mask-bits={}",
				Identity.SchemaVersion,
				static_cast<uint8>(Identity.BlendMode),
				static_cast<uint8>(Identity.ShadingModel),
				std::bit_cast<uint32>(Identity.OpacityMaskThreshold));
		}

		auto GetStaticMeshIdentityText(
			const FMaterialPipelineIdentity& Identity) -> std::string
		{
			return std::format(
				"{},two-sided={},depth-write={}",
				GetStaticMeshIdentityText(Identity.ShaderMap),
				Identity.bTwoSided,
				static_cast<uint8>(Identity.DepthWritePolicy));
		}

		auto ReportStaticMeshCreateDiagnostic(
			const FRenderResourceCreateDiagnostic& Diagnostic) -> void
		{
			ReportRendererResourceCreateDiagnostic(Diagnostic);
		}

		auto MakeStaticMeshCreateError(
			ERenderResourceCreateErrorCategory Category,
			std::string Context,
			std::string Identity,
			std::string Message,
			ERenderResourceGenerationDependency RetryDependencies)
			-> FRenderResourceCreateError
		{
			return MakeRendererResourceCreateError(
				Category,
				std::move(Context),
				std::move(Identity),
				std::move(Message),
				RetryDependencies);
		}

		auto EnsureStaticMeshBaseResources()
			-> FStaticMeshRendererState::FBaseResources*
		{
			using FResult =
				TRenderResourceCreateResult<
					FStaticMeshRendererState::FBaseResources>;
			return GStaticMeshState.BaseResources.Resolve(
				GetRendererResourceCoordinator().GetGeneration_RenderThread(),
				[]() -> FResult {
					FStaticMeshRendererState::FBaseResources Candidate;
					Candidate.BaseColorSampler =
						RHICreateSampler(FRHISamplerDesc::LinearRepeat());
					if (Candidate.BaseColorSampler == nullptr)
					{
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"StaticMeshBaseResources",
							"base-color-sampler",
							"RHI sampler creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					}
					return FResult::Success(std::move(Candidate));
				},
				ReportStaticMeshCreateDiagnostic);
		}

		auto GetOrCreateStaticMeshShaderMap(
			const FMaterialShaderMapIdentity& Identity)
			-> FStaticMeshRendererState::FShaderMapPayload*
		{
			using FResult =
				TRenderResourceCreateResult<
					FStaticMeshRendererState::FShaderMapPayload>;
			auto& Entry = GStaticMeshState.ShaderMaps.FindOrAdd(Identity);
			return Entry.Slot.Resolve(
				GetRendererResourceCoordinator().GetGeneration_RenderThread(),
				[&Identity]() -> FResult {
					FShaderCompileOptions CompileOptions;
					ConfigureRendererShaderCompileOptions(CompileOptions);
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_BLEND_MODE",
						std::to_string(
							static_cast<uint8>(Identity.BlendMode)));
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_SHADING_MODEL",
						std::to_string(
							static_cast<uint8>(Identity.ShadingModel)));
					CompileOptions.Macros.emplace_back(
						"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
						std::to_string(std::bit_cast<uint32>(
							Identity.OpacityMaskThreshold)));
					FShaderType& VertexShaderType =
						FStaticMeshVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FStaticMeshFragmentShader::StaticType();
					std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType,
						&FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes,
							CompileOptions,
							ErrorMessage))
					{
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"StaticMeshShaderMap",
							GetStaticMeshIdentityText(Identity),
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					}

					auto* VertexShader =
						static_cast<FStaticMeshVertexShader*>(
							ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader =
						static_cast<FStaticMeshFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
					{
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"StaticMeshShaderMap",
							GetStaticMeshIdentityText(Identity),
							"Compiled shader map did not contain both typed shaders.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					}

					FStaticMeshRendererState::FShaderMapPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader =
						TShaderRef<FStaticMeshVertexShader>(
							VertexShader,
							Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FStaticMeshFragmentShader>(
							FragmentShader,
							Candidate.ShaderMap.get());
					return FResult::Success(std::move(Candidate));
				},
				ReportStaticMeshCreateDiagnostic);
		}

		auto GetOrCreateStaticMeshPipeline(
			const FMaterialPipelineIdentity& Identity,
			const FLocalVertexFactory& VertexFactory)
			-> FStaticMeshRendererState::FPipelinePayload*
		{
			FStaticMeshRendererState::FBaseResources* BaseResources =
				EnsureStaticMeshBaseResources();
			if (BaseResources == nullptr)
			{
				return nullptr;
			}
			FStaticMeshRendererState::FShaderMapPayload* ShaderMapPayload =
				GetOrCreateStaticMeshShaderMap(Identity.ShaderMap);
			if (ShaderMapPayload == nullptr)
			{
				return nullptr;
			}

			using FResult =
				TRenderResourceCreateResult<
					FStaticMeshRendererState::FPipelinePayload>;
			auto& Entry = GStaticMeshState.Pipelines.FindOrAdd(Identity);
			FRenderResourceGeneration PipelineGeneration =
				GetRendererResourceCoordinator().GetGeneration_RenderThread();
			const auto* ShaderMapEntry =
				GStaticMeshState.ShaderMaps.Find(Identity.ShaderMap);
			check(ShaderMapEntry);
			PipelineGeneration.Shader =
				ShaderMapEntry->Slot.GetPayloadGeneration().Shader;
			return Entry.Slot.Resolve(
				PipelineGeneration,
				[&Identity,
				 &Entry,
				 ShaderMapPayload,
				 &VertexFactory]() -> FResult {
					FStaticMeshRendererState::FPipelinePayload Candidate;
					Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
					Candidate.VertexShader = ShaderMapPayload->VertexShader;
					Candidate.FragmentShader = ShaderMapPayload->FragmentShader;

					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RenderTargetLayouts::MakeSceneTargets();
					Initializer.BoundShaders.VertexShader =
						Candidate.VertexShader.GetRHIShader();
					Initializer.BoundShaders.FragmentShader =
						Candidate.FragmentShader.GetRHIShader();
					Initializer.VertexDeclaration =
						VertexFactory.GetDeclaration();
					Initializer.bEnableAlphaBlend = false;
					Initializer.bEnableDepthTest = true;
					Initializer.bEnableDepthWrite = true;
					Initializer.bEnableBackFaceCulling = false;
					Initializer.PipelineLayout =
						Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.SolidPipelineState =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							FName(std::format(
								"StaticMeshSolidPipeline_{}",
								Entry.Index)),
							Initializer);
					if (Candidate.SolidPipelineState == nullptr)
					{
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"StaticMeshPipeline",
							GetStaticMeshIdentityText(Identity),
							"Solid graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					}
					Initializer.PolygonMode =
						FGraphicsPipelineStateInitializer::EPolygonMode::Line;
					Initializer.bEnableBackFaceCulling = false;
					Candidate.WireframePipelineState =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							FName(std::format(
								"StaticMeshWireframePipeline_{}",
								Entry.Index)),
							Initializer);
					if (Candidate.WireframePipelineState == nullptr)
					{
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"StaticMeshPipeline",
							GetStaticMeshIdentityText(Identity),
							"Wireframe graphics pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					}
					return FResult::Success(std::move(Candidate));
				},
				ReportStaticMeshCreateDiagnostic);
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}

		auto DrawStaticMeshProxy(FRHICommandListImmediate& CommandList, const FSceneView& View, const FDirectionalLightSceneData& Light, ERenderMode RenderMode, ERasterMode RasterMode, const FStaticMeshSceneProxy& Proxy) -> void
		{
			const FStaticMeshRenderData* RenderData = Proxy.GetRenderData();
			if (RenderData == nullptr || RenderData->LODResources.empty())
			{
				return;
			}

			if (!RenderData->IsReadyForRendering())
			{
				return;
			}

			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const FLocalVertexFactory& VertexFactory =
				RenderData->LODVertexFactories[0].VertexFactory;
			FStaticMeshTransformUniform TransformUniform;
			TransformUniform.LocalToClip = ToShaderMatrix(View.ViewProjectionMatrix * Proxy.GetLocalToWorld());
			TransformUniform.LocalToWorld = ToShaderMatrix(Proxy.GetLocalToWorld());
			TransformUniform.NormalToWorld = ToShaderMatrix(glm::transpose(glm::inverse(Proxy.GetLocalToWorld())));
			const float TransformDeterminant = glm::determinant(glm::mat3(glm::mat4(Proxy.GetLocalToWorld())));
			TransformUniform.TransformParams.x = TransformDeterminant < 0.0f ? -1.0f : 1.0f;
			const FRHIUniformBufferRange TransformUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&TransformUniform, sizeof(TransformUniform));

			FStaticMeshLightingUniform LightingUniform;
			LightingUniform.LightDirection =
				FVector4f(FVector3f(Light.Direction), Light.RimLightIntensity);
			LightingUniform.LightColorIntensity = FVector4f(Light.Color, Light.Intensity);
			LightingUniform.ViewPositionAmbient = FVector4f(FVector3f(View.ViewLocation), Light.AmbientIntensity);
			const FRHIUniformBufferRange LightingUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&LightingUniform, sizeof(LightingUniform));

			VertexFactory.BindStreams(CommandList);
			CommandList.BindIndexBuffer(
				LOD.IndexBuffer.GetRHI(), 0);
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			for (const FStaticMeshSection& Section : LOD.Sections)
			{
				if (Section.IndexCount == 0
					|| static_cast<uint64>(Section.FirstIndex)
						+ Section.IndexCount
						> Indices.size())
				{
					continue;
				}
				const FMaterialRenderData& Material =
					Proxy.ResolveMaterialRenderData_RenderThread(
						Section.MaterialSlotIndex);
				FStaticMeshRendererState::FPipelinePayload* Pipeline =
					GetOrCreateStaticMeshPipeline(
						Material.PipelineIdentity,
						VertexFactory);
				if (Pipeline == nullptr) continue;
				const FGraphicsPipelineStateRHIRef PipelineState =
					RasterMode == ERasterMode::Wireframe
						? Pipeline->WireframePipelineState
						: Pipeline->SolidPipelineState;
				CommandList.SetGraphicsPipelineState(*PipelineState);

				FStaticMeshVertexShader::FParameters VertexShaderParameters;
				VertexShaderParameters.Transform = TransformUniformBuffer;
				SetShaderParameters(
					CommandList,
					Pipeline->VertexShader,
					VertexShaderParameters);

				FStaticMeshMaterialUniform MaterialUniform;
				MaterialUniform.BaseColor = Material.BaseColor;
				MaterialUniform.Params = FVector4f(Material.SpecularStrength, Material.Shininess, RenderMode == ERenderMode::Lit ? 1.0f : 0.0f, 0.0f);
				const FRHIUniformBufferRange MaterialUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&MaterialUniform, sizeof(MaterialUniform));
				FStaticMeshFragmentShader::FParameters FragmentShaderParameters;
				FragmentShaderParameters.Lighting = LightingUniformBuffer;
				FragmentShaderParameters.Material = MaterialUniformBuffer;
				FragmentShaderParameters.BaseColorTexture = ResolveTexture_RenderThread(Material.BaseColorTexture, EDefaultTexture::White);
				FragmentShaderParameters.BaseColorSampler =
					GStaticMeshState.BaseResources.GetPayload()
						->BaseColorSampler;
				SetShaderParameters(
					CommandList,
					Pipeline->FragmentShader,
					FragmentShaderParameters);
				CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			}
		}

		auto ForEachStaticMeshProxy(IScene* Scene, const std::function<void(FStaticMeshSceneProxy&)>& Function) -> void
		{
			auto* RendererScene = dynamic_cast<FScene*>(Scene);
			if (RendererScene == nullptr)
			{
				return;
			}

			for (PrimitiveSceneProxy* Proxy : RendererScene->GetPrimitiveSceneProxies())
			{
				if (auto* StaticMeshProxy = dynamic_cast<FStaticMeshSceneProxy*>(Proxy))
				{
					Function(*StaticMeshProxy);
				}
			}
		}

		auto ForEachTextureCubeThumbnailProxy(
			IScene* Scene,
			const std::function<void(FTextureCubePreviewSceneProxy&)>& Function) -> void
		{
			auto* RendererScene = dynamic_cast<FScene*>(Scene);
			if (RendererScene == nullptr) return;
			for (PrimitiveSceneProxy* Proxy : RendererScene->GetPrimitiveSceneProxies())
			{
				if (auto* TextureCubeProxy =
						dynamic_cast<FTextureCubePreviewSceneProxy*>(Proxy))
					Function(*TextureCubeProxy);
			}
		}
	}

	static auto ApplyRendererResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause,
		FRendererResourceCoordinator& Coordinator,
		FDefaultTextureResources& DefaultTextures,
		FFullscreenGeometryResources& FullscreenGeometry,
		FSkyBoxRenderer& SkyBoxRenderer,
		FTextureCubeThumbnailRenderer& TextureCubeThumbnailRenderer,
		FPostProcessRenderer& PostProcessRenderer) -> void
	{
		check(IsInRenderingThread());
		Coordinator.Apply_RenderThread(
			Cause,
			{
				.InvalidateShaderResources =
					[](bool bForceRecompile) {
						RendererEditorAssistance::InvalidateShaderResources(
							bForceRecompile);
					},
				.ReleaseDeviceResources =
					[&DefaultTextures,
					 &FullscreenGeometry,
					 &SkyBoxRenderer,
					 &TextureCubeThumbnailRenderer,
					 &PostProcessRenderer] {
						DefaultTextures.ReleaseResources_RenderThread();
						GStaticMeshState = {};
						TextureCubeThumbnailRenderer.
							ReleaseResources_RenderThread();
						SkyBoxRenderer.ReleaseResources_RenderThread();
						PostProcessRenderer.ReleaseResources_RenderThread();
						RendererEditorAssistance::
							InvalidateDeviceResources();
						FullscreenGeometry.ReleaseResources_RenderThread();
					},
				.RecreateStartupResources =
					[&CommandList, &DefaultTextures] {
						check(GDynamicRHI != nullptr);
						DefaultTextures.Initialize_RenderThread(CommandList);
					},
				.RetryFailedResources =
					[&FullscreenGeometry] {
						RendererEditorAssistance::RetryFailedResources();
						FullscreenGeometry.
							RetryFailedResources_RenderThread();
					},
			});
	}

	static auto EnqueueRendererResourceInvalidation(
		ERendererResourceInvalidationCause Cause,
		FRendererResourceCoordinator* Coordinator,
		FDefaultTextureResources* DefaultTextures,
		FFullscreenGeometryResources* FullscreenGeometry,
		FSkyBoxRenderer* SkyBoxRenderer,
		FTextureCubeThumbnailRenderer* TextureCubeThumbnailRenderer,
		FPostProcessRenderer* PostProcessRenderer) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[Cause,
			 Coordinator,
			 DefaultTextures,
			 FullscreenGeometry,
			 SkyBoxRenderer,
			 TextureCubeThumbnailRenderer,
			 PostProcessRenderer](FRHICommandListImmediate& CommandList) {
				ApplyRendererResourceInvalidation_RenderThread(
					CommandList,
					Cause,
					*Coordinator,
					*DefaultTextures,
					*FullscreenGeometry,
					*SkyBoxRenderer,
					*TextureCubeThumbnailRenderer,
					*PostProcessRenderer);
			});
	}

	FRendererModule::FRendererModule() = default;

	FRendererModule::~FRendererModule() = default;

	auto FRendererModule::StartupModule() -> void
	{
		check(SharedResources == nullptr);
		SharedResources = std::make_unique<FSharedResources>();
		SetActiveRendererResourceCoordinator(
			&SharedResources->Coordinator);
		SetActiveDefaultTextureResources(
			&SharedResources->DefaultTextures);
		SetActiveFullscreenGeometryResources(
			&SharedResources->FullscreenGeometry);

		const bool bCommandsRegistered =
			SharedResources->Coordinator.Start(
				FConsoleCommandRegistry::Get(),
				[Coordinator = &SharedResources->Coordinator,
				 DefaultTextures = &SharedResources->DefaultTextures,
				 FullscreenGeometry = &SharedResources->FullscreenGeometry,
				 SkyBoxRenderer = &SharedResources->SkyBoxRenderer,
				 TextureCubeThumbnailRenderer =
					 &SharedResources->TextureCubeThumbnailRenderer,
				 PostProcessRenderer =
					 &SharedResources->PostProcessRenderer](
					ERendererResourceInvalidationCause Cause) {
					EnqueueRendererResourceInvalidation(
						Cause,
						Coordinator,
						DefaultTextures,
						FullscreenGeometry,
						SkyBoxRenderer,
						TextureCubeThumbnailRenderer,
						PostProcessRenderer);
				});
		checkf(
			bCommandsRegistered,
			"Failed to register renderer resource invalidation commands");
		if (!bCommandsRegistered)
		{
			DURIN_ERROR(
				"Failed to register renderer resource invalidation commands");
		}
		if (GDynamicRHI != nullptr)
		{
			FDefaultTextureResources* DefaultTextures =
				&SharedResources->DefaultTextures;
			ENQUEUE_RENDER_COMMAND(InitializeDefaultTextures)(
				[DefaultTextures](FRHICommandListImmediate& CommandList) {
					DefaultTextures->Initialize_RenderThread(CommandList);
				});
		}
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		if (SharedResources == nullptr)
		{
			return;
		}
		SharedResources->Coordinator.Stop();
		FSharedResources* Resources = SharedResources.get();
		ENQUEUE_RENDER_COMMAND(ReleaseRendererResources)(
			[Resources](FRHICommandListImmediate&) {
				check(IsInRenderingThread());
				Resources->DefaultTextures.ReleaseResources_RenderThread();
				GStaticMeshState = {};
				Resources->Coordinator.ReleaseResources_RenderThread();
				Resources->TextureCubeThumbnailRenderer.
					ReleaseResources_RenderThread();
				Resources->SkyBoxRenderer.ReleaseResources_RenderThread();
				RendererEditorAssistance::ReleaseResources();
				Resources->PostProcessRenderer.
					ReleaseResources_RenderThread();
				Resources->FullscreenGeometry.ReleaseResources_RenderThread();
			});
		FlushRenderingCommands();
		SetActiveFullscreenGeometryResources(nullptr);
		SetActiveDefaultTextureResources(nullptr);
		SetActiveRendererResourceCoordinator(nullptr);
		SharedResources.reset();
	}

	auto FRendererModule::CreateScene() -> std::unique_ptr<IScene>
	{
		check(IsInGameThread());
		return std::make_unique<FScene>();
	}

	auto FRendererModule::RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderView");
		const uint32 Width = OutputTarget != nullptr ? OutputTarget->GetSizeX() : 0;
		const uint32 Height = OutputTarget != nullptr ? OutputTarget->GetSizeY() : 0;
		if (OutputTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			RendererEditorAssistance::AnalyzeRequest(View, ViewportOutput);

		SharedResources->PostProcessRenderer.EnsureResources_RenderThread(
			CommandList);
		// Sky resources include a static index upload, so initialize them before
		// entering the Scene Color render pass.
		SharedResources->SkyBoxRenderer.EnsureResources_RenderThread();
		FPostProcessRenderer::FSceneTargets* SceneTargets =
			SharedResources->PostProcessRenderer.
				EnsureSceneTargets_RenderThread(Width, Height);
		if (SceneTargets == nullptr || SceneTargets->Color == nullptr || SceneTargets->Depth == nullptr)
		{
			return;
		}
		FRHITexture* SceneColor = SceneTargets->Color;

		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint > 0.0f)
		{
			uint32 ContentWidth = Width;
			uint32 ContentHeight = static_cast<uint32>(std::round(ContentWidth / RenderView.AspectRatioConstraint));
			if (ContentHeight > Height)
			{
				ContentHeight = Height;
				ContentWidth = static_cast<uint32>(std::round(ContentHeight * RenderView.AspectRatioConstraint));
			}
			RenderView.ViewportWidth = std::max(1u, ContentWidth);
			RenderView.ViewportHeight = std::max(1u, ContentHeight);
			RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
			RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		}
		FRHIRenderPassInfo ScenePassInfo{};
		ScenePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeSceneTargets();
		ScenePassInfo.ColorRenderTargets[0] = SceneColor;
		ScenePassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
		ScenePassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
		ScenePassInfo.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		CommandList.BeginRenderPass(ScenePassInfo, "SceneColorRenderPass");
		RenderScene(CommandList, Scene, RenderView, SceneColor);
		CommandList.EndRenderPass();

		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
		{
			PreparedEditorAssistance = RendererEditorAssistance::Prepare(
				CommandList, RenderView, EditorAssistanceRequest);
		}
		const bool bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();
		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = bHasEditorAssistance
			? RenderTargetLayouts::MakeScenePostProcessOutput()
			: RenderTargetLayouts::MakeFinalScenePostProcessOutput(
				ViewportOutput);
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
		CommandList.BeginRenderPass(PostProcessPassInfo, bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass");
		SharedResources->PostProcessRenderer.Draw_RenderThread(
			CommandList,
			SceneColor,
			Width,
			Height,
			bPresentOutput,
			View.Settings.bEnableFXAA,
			bHasEditorAssistance);
		CommandList.EndRenderPass();
		if (!bHasEditorAssistance) return;

		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
		CommandList.BeginRenderPass(EditorAssistancePassInfo,
			bPresentOutput ? "EditorAssistancePresentRenderPass" : "EditorAssistanceOffscreenRenderPass");
		RendererEditorAssistance::Draw(
			CommandList, RenderView, PreparedEditorAssistance);
		CommandList.EndRenderPass();
	}

	auto FRendererModule::RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* RenderTarget) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderScene");
		const uint32 Width = View.ViewportWidth;
		const uint32 Height = View.ViewportHeight;
		if (Scene == nullptr || RenderTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}

		CommandList.SetViewport(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), 0.0f,
			static_cast<float>(View.ViewportX + Width), static_cast<float>(View.ViewportY + Height), 1.0f);
		CommandList.SetScissor(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), static_cast<float>(Width), static_cast<float>(Height));

		FSkyBoxSceneData SkyBox;
		if (Scene->GetActiveSkyBox_RenderThread(SkyBox))
		{
			SharedResources->SkyBoxRenderer.Draw_RenderThread(
				CommandList,
				View,
				SkyBox);
		}

		if (!EnsureStaticMeshBaseResources()) return;

		const ERenderMode RenderMode = View.Settings.RenderMode;
		const ERasterMode RasterMode = View.Settings.RasterMode;
		FDirectionalLightSceneData Light;
		Scene->GetDirectionalLight(Light);
		ForEachStaticMeshProxy(Scene, [&CommandList, &View, &Light, RenderMode, RasterMode](FStaticMeshSceneProxy& Proxy) {
			if (RenderMode == ERenderMode::Unlit || RenderMode == ERenderMode::Lit)
			{
				DrawStaticMeshProxy(CommandList, View, Light, RenderMode, RasterMode, Proxy);
			}
		});
		ForEachTextureCubeThumbnailProxy(
			Scene,
			[this, &CommandList, &View](
				FTextureCubePreviewSceneProxy& Proxy) {
				SharedResources->TextureCubeThumbnailRenderer.
					DrawProxy_RenderThread(
						CommandList,
						View,
						Proxy,
						SharedResources->SkyBoxRenderer);
			});
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
}
