#include "Renderers/StaticMeshRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "RendererResourceSlotCache.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

#include <glm/mat4x4.hpp>

#include <array>
#include <bit>
#include <format>
#include <string>
#include <utility>

namespace Durin
{
	namespace
	{
		class FStaticMeshVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FStaticMeshVertexShader,
				FShader,
				"/Engine/StaticMesh",
				EShaderFrequency::Vertex,
				"VertexMain");
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

			DURIN_DECLARE_SHADER(
				FStaticMeshFragmentShader,
				FShader,
				"/Engine/StaticMesh",
				EShaderFrequency::Fragment,
				"FragmentMain");
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

		auto GetIdentityText(
			const FMaterialShaderMapIdentity& Identity) -> std::string
		{
			return std::format(
				"schema={},blend={},shading={},mask-bits={}",
				Identity.SchemaVersion,
				static_cast<uint8>(Identity.BlendMode),
				static_cast<uint8>(Identity.ShadingModel),
				std::bit_cast<uint32>(Identity.OpacityMaskThreshold));
		}

		auto GetIdentityText(
			const FMaterialPipelineIdentity& Identity) -> std::string
		{
			return std::format(
				"{},two-sided={},depth-write={}",
				GetIdentityText(Identity.ShaderMap),
				Identity.bTwoSided,
				static_cast<uint8>(Identity.DepthWritePolicy));
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}
	} // namespace

	struct FStaticMeshRenderer::FState
	{
		struct FBaseResources
		{
			FVertexDeclarationRHIRef VertexDeclaration;
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

	FStaticMeshRenderer::FStaticMeshRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FDefaultTextureResources& InDefaultTextures)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, State(std::make_unique<FState>())
	{
	}

	FStaticMeshRenderer::~FStaticMeshRenderer() = default;

	auto FStaticMeshRenderer::EnsureResources_RenderThread() -> bool
	{
		check(IsInRenderingThread());
		using FResult =
			TRenderResourceCreateResult<FState::FBaseResources>;
		return State->BaseResources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[]() -> FResult {
				FState::FBaseResources Candidate;
				const FVertexDeclarationElementList VertexDeclElements =
					GetStaticMeshVertexDeclarationElements();
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(
						VertexDeclElements);
				if (Candidate.VertexDeclaration == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"StaticMeshBaseResources",
							"vertex-declaration",
							"RHI vertex declaration creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.BaseColorSampler =
					RHICreateSampler(FRHISamplerDesc::LinearRepeat());
				if (Candidate.BaseColorSampler == nullptr)
				{
					return FResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"StaticMeshBaseResources",
							"base-color-sampler",
							"RHI sampler creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic)
			!= nullptr;
	}

	auto FStaticMeshRenderer::DrawProxy_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FDirectionalLightSceneData& Light,
		ERenderMode RenderMode,
		ERasterMode RasterMode,
		const FStaticMeshSceneProxy& Proxy) -> void
	{
		check(IsInRenderingThread());
		const FStaticMeshRenderData* RenderData = Proxy.GetRenderData();
		if (RenderData == nullptr || RenderData->LODResources.empty()
			|| !RenderData->IsReadyForRendering())
		{
			return;
		}

		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		FStaticMeshTransformUniform TransformUniform;
		TransformUniform.LocalToClip = ToShaderMatrix(
			View.ViewProjectionMatrix * Proxy.GetLocalToWorld());
		TransformUniform.LocalToWorld =
			ToShaderMatrix(Proxy.GetLocalToWorld());
		TransformUniform.NormalToWorld = ToShaderMatrix(
			glm::transpose(glm::inverse(Proxy.GetLocalToWorld())));
		const float TransformDeterminant = glm::determinant(
			glm::mat3(glm::mat4(Proxy.GetLocalToWorld())));
		TransformUniform.TransformParams.x =
			TransformDeterminant < 0.0f ? -1.0f : 1.0f;
		const FRHIUniformBufferRange TransformUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform,
				sizeof(TransformUniform));

		FStaticMeshLightingUniform LightingUniform;
		LightingUniform.LightDirection = FVector4f(
			FVector3f(Light.Direction),
			Light.RimLightIntensity);
		LightingUniform.LightColorIntensity =
			FVector4f(Light.Color, Light.Intensity);
		LightingUniform.ViewPositionAmbient = FVector4f(
			FVector3f(View.ViewLocation),
			Light.AmbientIntensity);
		const FRHIUniformBufferRange LightingUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&LightingUniform,
				sizeof(LightingUniform));

		CommandList.BindVertexBuffer(
			0,
			LOD.VertexBuffers.PositionVertexBuffer.GetRHI(),
			0);
		CommandList.BindVertexBuffer(
			1,
			LOD.VertexBuffers.StaticMeshVertexBuffer.GetRHI(),
			0);
		CommandList.BindIndexBuffer(LOD.IndexBuffer.GetRHI(), 0);
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

			FState::FBaseResources* BaseResources =
				State->BaseResources.GetPayload();
			if (BaseResources == nullptr)
			{
				continue;
			}

			using FShaderMapResult =
				TRenderResourceCreateResult<FState::FShaderMapPayload>;
			auto& ShaderMapEntry =
				State->ShaderMaps.FindOrAdd(
					Material.PipelineIdentity.ShaderMap);
			FState::FShaderMapPayload* ShaderMapPayload =
				ShaderMapEntry.Slot.Resolve(
					Coordinator.GetGeneration_RenderThread(),
					[this, &Material]() -> FShaderMapResult {
						const FMaterialShaderMapIdentity& Identity =
							Material.PipelineIdentity.ShaderMap;
						FShaderCompileOptions CompileOptions;
						CompileOptions.bForceRecompile =
							Coordinator.
								ShouldForceShaderRecompile_RenderThread();
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
						const std::array<const FShaderType*, 2> ShaderTypes = {
							&VertexShaderType,
							&FragmentShaderType};
						auto ShaderMap = std::make_shared<FShaderMapBase>();
						std::string ErrorMessage;
						if (!ShaderMap->InitializeFromShaderTypes(
								ShaderTypes,
								CompileOptions,
								ErrorMessage))
						{
							return FShaderMapResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::
										ShaderCompile,
									"StaticMeshShaderMap",
									GetIdentityText(Identity),
									std::move(ErrorMessage),
									ERenderResourceGenerationDependency::Shader
										| ERenderResourceGenerationDependency::
											Manual));
						}

						auto* VertexShader =
							static_cast<FStaticMeshVertexShader*>(
								ShaderMap->GetShader(&VertexShaderType));
						auto* FragmentShader =
							static_cast<FStaticMeshFragmentShader*>(
								ShaderMap->GetShader(&FragmentShaderType));
						if (VertexShader == nullptr
							|| FragmentShader == nullptr)
						{
							return FShaderMapResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::
										ShaderBinding,
									"StaticMeshShaderMap",
									GetIdentityText(Identity),
									"Compiled shader map did not contain both typed shaders.",
									ERenderResourceGenerationDependency::Shader
										| ERenderResourceGenerationDependency::
											Manual));
						}

						FState::FShaderMapPayload Candidate;
						Candidate.ShaderMap = std::move(ShaderMap);
						Candidate.VertexShader =
							TShaderRef<FStaticMeshVertexShader>(
								VertexShader,
								Candidate.ShaderMap.get());
						Candidate.FragmentShader =
							TShaderRef<FStaticMeshFragmentShader>(
								FragmentShader,
								Candidate.ShaderMap.get());
						return FShaderMapResult::Success(
							std::move(Candidate));
					},
					ReportRendererResourceCreateDiagnostic);
			if (ShaderMapPayload == nullptr)
			{
				continue;
			}

			using FPipelineResult =
				TRenderResourceCreateResult<FState::FPipelinePayload>;
			auto& PipelineEntry = State->Pipelines.FindOrAdd(
				Material.PipelineIdentity);
			FRenderResourceGeneration PipelineGeneration =
				Coordinator.GetGeneration_RenderThread();
			PipelineGeneration.Shader =
				ShaderMapEntry.Slot.GetPayloadGeneration().Shader;
			FState::FPipelinePayload* Pipeline =
				PipelineEntry.Slot.Resolve(
					PipelineGeneration,
					[&Material,
					 &PipelineEntry,
					 BaseResources,
					 ShaderMapPayload]() -> FPipelineResult {
						const FMaterialPipelineIdentity& Identity =
							Material.PipelineIdentity;
						FState::FPipelinePayload Candidate;
						Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
						Candidate.VertexShader =
							ShaderMapPayload->VertexShader;
						Candidate.FragmentShader =
							ShaderMapPayload->FragmentShader;

						FGraphicsPipelineStateInitializer Initializer;
						Initializer.RenderTargetLayout =
							RenderTargetLayouts::MakeSceneTargets();
						Initializer.BoundShaders.VertexShader =
							Candidate.VertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							Candidate.FragmentShader.GetRHIShader();
						Initializer.VertexDeclaration =
							BaseResources->VertexDeclaration;
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
									PipelineEntry.Index)),
								Initializer);
						if (Candidate.SolidPipelineState == nullptr)
						{
							return FPipelineResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::
										GraphicsPipeline,
									"StaticMeshPipeline",
									GetIdentityText(Identity),
									"Solid graphics pipeline creation returned null.",
									ERenderResourceGenerationDependency::Shader
										| ERenderResourceGenerationDependency::
											Device
										| ERenderResourceGenerationDependency::
											Manual));
						}
						Initializer.PolygonMode =
							FGraphicsPipelineStateInitializer::
								EPolygonMode::Line;
						Initializer.bEnableBackFaceCulling = false;
						Candidate.WireframePipelineState =
							GDynamicRHI->RHICreateGraphicsPipelineState(
								FName(std::format(
									"StaticMeshWireframePipeline_{}",
									PipelineEntry.Index)),
								Initializer);
						if (Candidate.WireframePipelineState == nullptr)
						{
							return FPipelineResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::
										GraphicsPipeline,
									"StaticMeshPipeline",
									GetIdentityText(Identity),
									"Wireframe graphics pipeline creation returned null.",
									ERenderResourceGenerationDependency::Shader
										| ERenderResourceGenerationDependency::
											Device
										| ERenderResourceGenerationDependency::
											Manual));
						}
						return FPipelineResult::Success(
							std::move(Candidate));
					},
					ReportRendererResourceCreateDiagnostic);
			if (Pipeline == nullptr)
			{
				continue;
			}

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
			MaterialUniform.Params = FVector4f(
				Material.SpecularStrength,
				Material.Shininess,
				RenderMode == ERenderMode::Lit ? 1.0f : 0.0f,
				0.0f);
			const FRHIUniformBufferRange MaterialUniformBuffer =
				CommandList.AllocateDynamicUniformBuffer(
					&MaterialUniform,
					sizeof(MaterialUniform));
			FStaticMeshFragmentShader::FParameters FragmentShaderParameters;
			FragmentShaderParameters.Lighting = LightingUniformBuffer;
			FragmentShaderParameters.Material = MaterialUniformBuffer;
			FRHITexture* BaseColorTexture =
				Material.BaseColorTexture != nullptr
				? Material.BaseColorTexture
					  ->GetReferencedTexture_RenderThread()
				: nullptr;
			FragmentShaderParameters.BaseColorTexture =
				BaseColorTexture != nullptr
					? BaseColorTexture
					: DefaultTextures.Get_RenderThread(
						EDefaultTexture::White);
			FragmentShaderParameters.BaseColorSampler =
				BaseResources->BaseColorSampler;
			SetShaderParameters(
				CommandList,
				Pipeline->FragmentShader,
				FragmentShaderParameters);
			CommandList.DrawIndexed(
				Section.IndexCount,
				Section.FirstIndex,
				0);
		}
	}

	auto FStaticMeshRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->BaseResources.Reset();
		State->ShaderMaps.Reset();
		State->Pipelines.Reset();
	}
} // namespace Durin
