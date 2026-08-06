#include "Renderers/StaticMeshRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "RendererResourceSlotCache.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <array>
#include <bit>
#include <format>
#include <string>
#include <unordered_map>
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
				"/Engine/StaticMeshBasePass",
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
				DURIN_SHADER_PARAMETER_TEXTURE(NormalTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(MetallicTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(RoughnessTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(AmbientOcclusionTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(EmissiveTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(BaseColorSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(NormalSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(MetallicSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(RoughnessSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(AmbientOcclusionSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(EmissiveSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacitySampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentIrradiance);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentPrefiltered);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentBrdfLut);
				DURIN_SHADER_PARAMETER_SAMPLER(EnvironmentSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FStaticMeshFragmentShader,
				FShader,
				"/Engine/StaticMeshBasePass",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};

		struct FStaticMeshTransformUniform
		{
			FMatrix4f LocalToClip{1.0f};
			FMatrix4f LocalToWorld{1.0f};
			FMatrix4f NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FStaticMeshLightingUniform
		{
			FVector4f LightDirection{-0.5f, -0.5f, -1.0f, 0.0f};
			FVector4f LightColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
			FVector4f ViewPosition{0.0f};
		};

		struct FStaticMeshMaterialUniform
		{
			FVector4f BaseColor{1.0f};
			FVector4f EmissiveMetallic{0.0f};
			FVector4f NormalRoughness{0.0f, 0.0f, 1.0f, 0.5f};
			FVector4f SurfaceParams{1.0f, 1.0f, 1.0f, 0.0f};
			std::array<FVector4f, 8> UVTransforms{};
			FVector4f UVChannels0{0.0f};
			FVector4f UVChannels1{0.0f};
			FVector4f UVRotations0{0.0f};
			FVector4f UVRotations1{0.0f};
		};

		auto GetMaterialSamplerKey(const FMaterialSamplerState& State) -> size_t
		{
			return static_cast<size_t>(State.MinFilter) + 6 * (
				static_cast<size_t>(State.MagFilter) + 2 * (
					static_cast<size_t>(State.AddressU) + 3
						* static_cast<size_t>(State.AddressV)));
		}

		auto ToRHIAddress(EMaterialSamplerAddressMode Address)
			-> ESamplerAddressMode
		{
			switch (Address)
			{
			case EMaterialSamplerAddressMode::MirroredRepeat:
				return ESamplerAddressMode::MirroredRepeat;
			case EMaterialSamplerAddressMode::ClampToEdge:
				return ESamplerAddressMode::ClampToEdge;
			case EMaterialSamplerAddressMode::Repeat:
			default:
				return ESamplerAddressMode::Repeat;
			}
		}

		auto MakeMaterialSamplerDesc(const FMaterialSamplerState& State)
			-> FRHISamplerDesc
		{
			FRHISamplerDesc Result;
			const uint8 Min = static_cast<uint8>(State.MinFilter);
			Result.MinFilter = (Min & 1u) != 0
				? ESamplerFilter::Linear : ESamplerFilter::Nearest;
			Result.MagFilter = State.MagFilter == EMaterialSamplerMagFilter::Linear
				? ESamplerFilter::Linear : ESamplerFilter::Nearest;
			Result.MipmapMode = Min >= 4
				? ESamplerMipmapMode::Linear : ESamplerMipmapMode::Nearest;
			Result.MaxLod = Min < 2 ? 0.0f : 1000.0f;
			Result.AddressU = ToRHIAddress(State.AddressU);
			Result.AddressV = ToRHIAddress(State.AddressV);
			Result.AddressW = ESamplerAddressMode::Repeat;
			return Result;
		}

		auto GetIdentityText(
			const FMaterialShaderMapIdentity& Identity) -> std::string
		{
			return std::format(
				"layout-version={},layout-id={},blend={},shading={},mask-bits={}",
				Identity.RenderLayout.Version,
				Identity.RenderLayout.Id.ToString(),
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

		auto ToShaderMatrix(const FMatrix& Matrix) -> FMatrix4f
		{
			FMatrix4f Result(0.0f);
			for (uint32 Column = 0; Column < 4; ++Column)
			{
				for (uint32 Row = 0; Row < 4; ++Row)
				{
					Result[Column][Row] = static_cast<float>(Matrix[Row][Column]);
				}
			}
			return Result;
		}
	} // namespace

	struct FStaticMeshRenderer::FState
	{
		struct FBaseResources
		{
			std::unordered_map<
				size_t,
				TRenderResourceCreationSlot<FSamplerRHIRef>>
				MaterialSamplerCache;
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
		FDefaultTextureResources& InDefaultTextures,
		FEnvironmentLightingResources& InEnvironmentLighting)
		: Coordinator(InCoordinator)
		, DefaultTextures(InDefaultTextures)
		, EnvironmentLighting(InEnvironmentLighting)
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
				return FResult::Success(FState::FBaseResources{});
			},
			ReportRendererResourceCreateDiagnostic)
			!= nullptr;
	}

	auto FStaticMeshRenderer::DrawScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		IScene* Scene,
		const FSceneView& View,
		const FDirectionalLightSceneData& Light,
		ERenderMode RenderMode,
		ERasterMode RasterMode) -> void
	{
		check(IsInRenderingThread());
		if (RenderMode != ERenderMode::Unlit
			&& RenderMode != ERenderMode::Lit)
		{
			return;
		}
		auto* RendererScene = dynamic_cast<FScene*>(Scene);
		if (RendererScene == nullptr)
		{
			return;
		}
		for (PrimitiveSceneProxy* Proxy :
			RendererScene->GetPrimitiveSceneProxies())
		{
			if (auto* StaticMeshProxy =
					dynamic_cast<FStaticMeshSceneProxy*>(Proxy))
			{
				DrawProxy_RenderThread(
					CommandList,
					View,
					Light,
					RenderMode,
					RasterMode,
					*StaticMeshProxy);
			}
		}
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
		const FLocalVertexFactory& VertexFactory =
			RenderData->LODVertexFactories[0].VertexFactory;
		FStaticMeshTransformUniform TransformUniform;
		TransformUniform.LocalToClip = ToShaderMatrix(
			View.ViewProjectionMatrix * Proxy.GetLocalToWorld());
		TransformUniform.LocalToWorld =
			ToShaderMatrix(Proxy.GetLocalToWorld());
		TransformUniform.NormalToWorld = ToShaderMatrix(
			Math::Transpose(Math::Inverse(Proxy.GetLocalToWorld())));
		const float TransformDeterminant = glm::determinant(
			glm::mat3(FMatrix4f(Proxy.GetLocalToWorld())));
		TransformUniform.TransformParams.x =
			TransformDeterminant < 0.0f ? -1.0f : 1.0f;
		const FRHIUniformBufferRange TransformUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&TransformUniform,
				sizeof(TransformUniform));

		FStaticMeshLightingUniform LightingUniform;
		LightingUniform.LightDirection = FVector4f(
			FVector3f(Light.Direction),
			0.0f);
		LightingUniform.LightColorIntensity =
			FVector4f(Light.Color, Light.Intensity);
		LightingUniform.ViewPosition = FVector4f(
			FVector3f(View.ViewLocation),
			0.0f);
		const FRHIUniformBufferRange LightingUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(
				&LightingUniform,
				sizeof(LightingUniform));

		VertexFactory.BindStreams(CommandList);
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

			const FMaterialRenderData& ResolvedMaterial =
				Proxy.ResolveMaterialRenderData_RenderThread(
					Section.MaterialSlotIndex);
			FMaterialRenderV3Binding MaterialBinding;
			FMaterialRenderValidationDiagnostic BindingDiagnostic;
			const FMaterialRenderData* MaterialData = &ResolvedMaterial;
			bool bBindingValid = TryGetMaterialRenderV3Binding(
				ResolvedMaterial.Representation, MaterialBinding, BindingDiagnostic);
			if (!bBindingValid
				&& ResolvedMaterial.Representation.GetLayout().Identity.Version == 2)
			{
				FMaterialRenderV2Binding LegacyBinding;
				bBindingValid = TryGetMaterialRenderV2Binding(
					ResolvedMaterial.Representation, LegacyBinding, BindingDiagnostic);
				if (bBindingValid)
				{
					static_cast<FMaterialRenderV2Binding&>(MaterialBinding) =
						std::move(LegacyBinding);
				}
			}
			if (!bBindingValid)
			{
				RecordMaterialFallbackReason(
					EMaterialFallbackReason::UnsupportedLayout);
				FRenderResourceCreateDiagnostic Diagnostic;
				Diagnostic.Error = MakeRendererResourceCreateError(
					ERenderResourceCreateErrorCategory::ShaderBinding,
					"StaticMeshMaterialBinding",
					GetIdentityText(
						ResolvedMaterial.PipelineIdentity.ShaderMap),
					std::format(
						"{} ErrorMaterial was selected.",
						BindingDiagnostic.Message),
					ERenderResourceGenerationDependency::Manual);
				ReportRendererResourceCreateDiagnostic(Diagnostic);

				const FMaterialRenderData& ErrorMaterial =
					GetErrorMaterialRenderData();
				MaterialData = &ErrorMaterial;
				FMaterialRenderValidationDiagnostic ErrorDiagnostic;
				if (!TryGetMaterialRenderV3Binding(
					ErrorMaterial.Representation,
					MaterialBinding,
					ErrorDiagnostic))
				{
					checkf(
						false,
						"ErrorMaterial must satisfy the exact v3 binding contract: %s",
						ErrorDiagnostic.Message.c_str());
					continue;
				}
			}
			const FMaterialRenderData& Material = *MaterialData;

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
						if (Candidate.VertexShader.GetRHIShader(false) == nullptr
							|| Candidate.FragmentShader.GetRHIShader(false) == nullptr)
						{
							return FShaderMapResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::RHIResource,
									"StaticMeshShaderMap",
									GetIdentityText(Identity),
									"RHI shader creation returned null.",
									ERenderResourceGenerationDependency::Shader
										| ERenderResourceGenerationDependency::Device
										| ERenderResourceGenerationDependency::Manual));
						}
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
					 ShaderMapPayload,
					 &VertexFactory]() -> FPipelineResult {
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
			MaterialUniform.BaseColor = MaterialBinding.BaseColor;
			MaterialUniform.EmissiveMetallic = FVector4f(
				MaterialBinding.Emissive,
				MaterialBinding.Metallic);
			MaterialUniform.NormalRoughness = FVector4f(
				MaterialBinding.Normal,
				MaterialBinding.Roughness);
			MaterialUniform.SurfaceParams = FVector4f(
				MaterialBinding.AmbientOcclusion,
				MaterialBinding.OpacityMask,
				RenderMode == ERenderMode::Lit
						&& Material.PipelineIdentity.ShaderMap.ShadingModel
							== EMaterialShadingModel::Lit
					? 1.0f
					: 0.0f,
				0.0f);
			for (size_t Role = 0; Role < MaterialBinding.Textures.size(); ++Role)
			{
				MaterialUniform.UVTransforms[Role] = FVector4f(
					MaterialBinding.UVScales[Role].x,
					MaterialBinding.UVScales[Role].y,
					MaterialBinding.UVOffsets[Role].x,
					MaterialBinding.UVOffsets[Role].y);
			}
			MaterialUniform.UVChannels0 = FVector4f(
				MaterialBinding.UVChannels[0], MaterialBinding.UVChannels[1],
				MaterialBinding.UVChannels[2], MaterialBinding.UVChannels[3]);
			MaterialUniform.UVChannels1 = FVector4f(
				MaterialBinding.UVChannels[4], MaterialBinding.UVChannels[5],
				MaterialBinding.UVChannels[6], MaterialBinding.UVChannels[7]);
			MaterialUniform.UVRotations0 = FVector4f(
				MaterialBinding.UVRotations[0], MaterialBinding.UVRotations[1],
				MaterialBinding.UVRotations[2], MaterialBinding.UVRotations[3]);
			MaterialUniform.UVRotations1 = FVector4f(
				MaterialBinding.UVRotations[4], MaterialBinding.UVRotations[5],
				MaterialBinding.UVRotations[6], MaterialBinding.UVRotations[7]);
			const FRHIUniformBufferRange MaterialUniformBuffer =
				CommandList.AllocateDynamicUniformBuffer(
					&MaterialUniform,
					sizeof(MaterialUniform));
			FStaticMeshFragmentShader::FParameters FragmentShaderParameters;
			FragmentShaderParameters.Lighting = LightingUniformBuffer;
			FragmentShaderParameters.Material = MaterialUniformBuffer;
			auto ResolveTexture = [&](size_t Role, EDefaultTexture Fallback) {
				FRHITexture* Texture = MaterialBinding.Textures[Role] != nullptr
					? MaterialBinding.Textures[Role]->GetReferencedTexture_RenderThread()
					: nullptr;
				return Texture != nullptr
					? Texture
					: DefaultTextures.Get_RenderThread(Fallback);
			};
			FragmentShaderParameters.BaseColorTexture = ResolveTexture(0, EDefaultTexture::White);
			FragmentShaderParameters.NormalTexture = ResolveTexture(1, EDefaultTexture::FlatNormal);
			FragmentShaderParameters.MetallicTexture = ResolveTexture(2, EDefaultTexture::White);
			FragmentShaderParameters.RoughnessTexture = ResolveTexture(3, EDefaultTexture::White);
			FragmentShaderParameters.AmbientOcclusionTexture = ResolveTexture(4, EDefaultTexture::White);
			FragmentShaderParameters.EmissiveTexture = ResolveTexture(5, EDefaultTexture::Black);
			FragmentShaderParameters.OpacityTexture = ResolveTexture(6, EDefaultTexture::White);
			FragmentShaderParameters.OpacityMaskTexture = ResolveTexture(7, EDefaultTexture::White);
			auto ResolveSampler = [&](size_t Role) -> FRHISampler* {
				const FMaterialSamplerState SamplerState =
					MaterialBinding.Samplers[Role];
				auto Entry = BaseResources->MaterialSamplerCache.try_emplace(
					GetMaterialSamplerKey(SamplerState),
					ERenderResourceGenerationDependency::Device).first;
				using FSamplerResult =
					TRenderResourceCreateResult<FSamplerRHIRef>;
				FSamplerRHIRef* Sampler = Entry->second.Resolve(
					Coordinator.GetGeneration_RenderThread(),
					[SamplerState]() -> FSamplerResult {
						FSamplerRHIRef Candidate =
							RHICreateSampler(MakeMaterialSamplerDesc(SamplerState));
						if (Candidate == nullptr)
						{
							return FSamplerResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::RHIResource,
									"StaticMeshMaterialSampler",
									std::format(
										"min={},mag={},u={},v={}",
										static_cast<uint8>(SamplerState.MinFilter),
										static_cast<uint8>(SamplerState.MagFilter),
										static_cast<uint8>(SamplerState.AddressU),
										static_cast<uint8>(SamplerState.AddressV)),
									"RHI sampler creation returned null.",
									ERenderResourceGenerationDependency::Device
										| ERenderResourceGenerationDependency::Manual));
						}
						return FSamplerResult::Success(std::move(Candidate));
					},
					ReportRendererResourceCreateDiagnostic);
				return Sampler != nullptr ? Sampler->GetReference() : nullptr;
			};
			std::array<FRHISampler*, 8> MaterialSamplers{};
			bool bMaterialSamplersValid = true;
			for (size_t Role = 0; Role < MaterialSamplers.size(); ++Role)
			{
				MaterialSamplers[Role] = ResolveSampler(Role);
				if (MaterialSamplers[Role] == nullptr)
				{
					bMaterialSamplersValid = false;
					break;
				}
			}
			if (!bMaterialSamplersValid)
			{
				continue;
			}
			FragmentShaderParameters.BaseColorSampler = MaterialSamplers[0];
			FragmentShaderParameters.NormalSampler = MaterialSamplers[1];
			FragmentShaderParameters.MetallicSampler = MaterialSamplers[2];
			FragmentShaderParameters.RoughnessSampler = MaterialSamplers[3];
			FragmentShaderParameters.AmbientOcclusionSampler = MaterialSamplers[4];
			FragmentShaderParameters.EmissiveSampler = MaterialSamplers[5];
			FragmentShaderParameters.OpacitySampler = MaterialSamplers[6];
			FragmentShaderParameters.OpacityMaskSampler = MaterialSamplers[7];
			FRHITexture* EnvironmentIrradiance =
				EnvironmentLighting.GetIrradiance_RenderThread();
			FRHITexture* EnvironmentPrefiltered =
				EnvironmentLighting.GetPrefiltered_RenderThread();
			FRHITexture* EnvironmentBrdfLut =
				EnvironmentLighting.GetBrdfLut_RenderThread();
			FRHISampler* EnvironmentSampler =
				EnvironmentLighting.GetSampler_RenderThread();
			const bool bHasCompleteEnvironment = EnvironmentIrradiance != nullptr
				&& EnvironmentPrefiltered != nullptr && EnvironmentBrdfLut != nullptr
				&& EnvironmentSampler != nullptr;
			FragmentShaderParameters.EnvironmentIrradiance = bHasCompleteEnvironment
				? EnvironmentIrradiance : DefaultTextures.GetCube_RenderThread();
			FragmentShaderParameters.EnvironmentPrefiltered = bHasCompleteEnvironment
				? EnvironmentPrefiltered : DefaultTextures.GetCube_RenderThread();
			FragmentShaderParameters.EnvironmentBrdfLut = bHasCompleteEnvironment
				? EnvironmentBrdfLut : DefaultTextures.Get_RenderThread(EDefaultTexture::Black);
			FragmentShaderParameters.EnvironmentSampler = bHasCompleteEnvironment
				? EnvironmentSampler : MaterialSamplers[0];
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
