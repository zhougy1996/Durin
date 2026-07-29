#include "RendererModule.h"
#include "Profiling/Profiling.h"

#include "CoreGlobals.h"
#include "DefaultTextures.h"
#include "RendererEditorAssistance.h"
#include "RendererFullscreenGeometry.h"
#include "RendererResourceSlotCache.h"
#include "RendererRenderTargetLayouts.h"
#include "RenderResourceCreation.h"
#include "SkyBoxRendering.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureCubeRenderResource.h"

#include <glm/mat4x4.hpp>

namespace Durin
{
	namespace
	{
		struct FDefaultTextureState
		{
			FTextureRHIRef White;
			FTextureRHIRef Black;
			FTextureRHIRef FlatNormal;
			FTextureRHIRef BlackCube;
		};

		FDefaultTextureState GDefaultTextures;

		auto CreateSolidTexture(FRHICommandListImmediate& CommandList, const char* DebugName, const std::array<uint8, 4>& Color) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(DebugName, 1, 1, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, Texture, 0, 0, Region, 4, Color.data());
			}
			return Texture;
		}

		auto CreateSolidCubeTexture(FRHICommandListImmediate& CommandList, const char* DebugName, const std::array<uint8, 4>& Color) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube(DebugName)
				.SetExtent(1)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
				for (uint32 ArraySlice = 0; ArraySlice < TextureCubeFaceCount; ++ArraySlice)
				{
					GDynamicRHI->RHIUpdateTexture2D(CommandList, Texture, 0, ArraySlice, Region, 4, Color.data());
				}
			}
			return Texture;
		}

		auto InitializeDefaultTextures_RenderThread(FRHICommandListImmediate& CommandList) -> void
		{
			check(IsInRenderingThread());
			if (GDefaultTextures.White != nullptr) return;
			GDefaultTextures.White = CreateSolidTexture(CommandList, "DefaultWhite", {255, 255, 255, 255});
			GDefaultTextures.Black = CreateSolidTexture(CommandList, "DefaultBlack", {0, 0, 0, 255});
			GDefaultTextures.FlatNormal = CreateSolidTexture(CommandList, "DefaultFlatNormal", {128, 128, 255, 255});
			GDefaultTextures.BlackCube = CreateSolidCubeTexture(CommandList, "DefaultBlackCube", {0, 0, 0, 255});
		}

		auto GetViewportOutput(bool bPresent) -> RendererRenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RendererRenderTargetLayouts::EViewportOutput::Present : RendererRenderTargetLayouts::EViewportOutput::Offscreen;
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

		class FTextureCubeThumbnailVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTextureCubeThumbnailVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTextureCubeThumbnailVertexShader, FShader, "/Engine/TextureCubeThumbnail", EShaderFrequency::Vertex, "VertexMain");
		};

		class FTextureCubeThumbnailFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTextureCubeThumbnailFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_TEXTURE(CubeTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(CubeSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTextureCubeThumbnailFragmentShader, FShader, "/Engine/TextureCubeThumbnail", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FSkyBoxVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FSkyBoxVertexShader, FShader, "/Engine/SkyBox", EShaderFrequency::Vertex, "VertexMain");
		};

		class FSkyBoxFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSkyBoxFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SkyTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(SkySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Sky);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FSkyBoxFragmentShader, FShader, "/Engine/SkyBox", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FPostProcessVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FPostProcessVertexShader, FShader, "/Engine/PostProcess", EShaderFrequency::Vertex, "VertexMain");
		};

		class FCopySceneColorFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCopySceneColorFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FCopySceneColorFragmentShader, FShader, "/Engine/PostProcess", EShaderFrequency::Fragment, "CopyFragmentMain");
		};

		class FFXAAFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FFXAAFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FFXAAFragmentShader, FShader, "/Engine/PostProcess", EShaderFrequency::Fragment, "FXAAFragmentMain");
		};

		struct FStaticMeshTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
			glm::mat4 LocalToWorld{1.0f};
			glm::mat4 NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FTextureCubeThumbnailTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
			glm::mat4 LocalToWorld{1.0f};
			glm::mat4 NormalToWorld{1.0f};
			FVector4f ViewPosition{0.0f};
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

		struct FPostProcessViewUniform
		{
			FVector2f InvRenderTargetSize{1.0f, 1.0f};
			FVector2f Padding{0.0f, 0.0f};
		};

		struct FStaticMeshRendererState
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

		struct FTextureCubeThumbnailRendererState
		{
			struct FPayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FTextureCubeThumbnailVertexShader> VertexShader;
				TShaderRef<FTextureCubeThumbnailFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
				FGraphicsPipelineStateRHIRef PipelineState;
				FSamplerRHIRef Sampler;
			};

			TRenderResourceCreationSlot<FPayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FTextureCubeThumbnailVertexShader> VertexShader;
			TShaderRef<FTextureCubeThumbnailFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FSamplerRHIRef Sampler;
		};

		struct FSkyBoxRendererState
		{
			struct FPayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FSkyBoxVertexShader> VertexShader;
				TShaderRef<FSkyBoxFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
				FGraphicsPipelineStateRHIRef PipelineState;
				FBufferRHIRef IndexBuffer;
				FSamplerRHIRef Sampler;
			};

			TRenderResourceCreationSlot<FPayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkyBoxVertexShader> VertexShader;
			TShaderRef<FSkyBoxFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef Sampler;
		};

		struct FPostProcessRendererState
		{
			struct FPayload
			{
				std::shared_ptr<FShaderMapBase> CopyShaderMap;
				std::shared_ptr<FShaderMapBase> FXAAShaderMap;
				TShaderRef<FPostProcessVertexShader> CopyVertexShader;
				TShaderRef<FPostProcessVertexShader> FXAAVertexShader;
				TShaderRef<FCopySceneColorFragmentShader> CopyFragmentShader;
				TShaderRef<FFXAAFragmentShader> FXAAFragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
				FGraphicsPipelineStateRHIRef CopyIntermediatePipelineState;
				FGraphicsPipelineStateRHIRef FXAAIntermediatePipelineState;
				FGraphicsPipelineStateRHIRef CopyOffscreenPipelineState;
				FGraphicsPipelineStateRHIRef CopyPresentPipelineState;
				FGraphicsPipelineStateRHIRef FXAAOffscreenPipelineState;
				FGraphicsPipelineStateRHIRef FXAAPresentPipelineState;
				FSamplerRHIRef SceneColorSampler;
			};

			TRenderResourceCreationSlot<FPayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			struct FSceneTargets
			{
				FTextureRHIRef Color;
				FTextureRHIRef Depth;
			};

			std::shared_ptr<FShaderMapBase> CopyShaderMap;
			std::shared_ptr<FShaderMapBase> FXAAShaderMap;
			TShaderRef<FPostProcessVertexShader> CopyVertexShader;
			TShaderRef<FPostProcessVertexShader> FXAAVertexShader;
			TShaderRef<FCopySceneColorFragmentShader> CopyFragmentShader;
			TShaderRef<FFXAAFragmentShader> FXAAFragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef CopyIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef FXAAIntermediatePipelineState;
			FGraphicsPipelineStateRHIRef CopyOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef CopyPresentPipelineState;
			FGraphicsPipelineStateRHIRef FXAAOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef FXAAPresentPipelineState;
			FSamplerRHIRef SceneColorSampler;
			std::unordered_map<uint64, FSceneTargets> SceneTargetsBySize;
		};

		FStaticMeshRendererState GStaticMeshState;
		FRenderResourceGeneration GRendererResourceGeneration;
		FTextureCubeThumbnailRendererState GTextureCubeThumbnailState;
		FSkyBoxRendererState GSkyBoxState;
		FPostProcessRendererState GPostProcessState;
		auto ReportStaticMeshCreateDiagnostic(
			const FRenderResourceCreateDiagnostic& Diagnostic) -> void;
		auto MakeStaticMeshCreateError(
			ERenderResourceCreateErrorCategory Category,
			std::string Context,
			std::string Identity,
			std::string Message,
			ERenderResourceGenerationDependency RetryDependencies)
			-> FRenderResourceCreateError;

		auto EnsureSkyBoxResources() -> void
		{
			using FResult =
				TRenderResourceCreateResult<FSkyBoxRendererState::FPayload>;
			auto* Payload = GSkyBoxState.Slot.Resolve(
				GRendererResourceGeneration,
				[]() -> FResult {
					FShaderCompileOptions CompileOptions;
					FShaderType& VertexShaderType =
						FSkyBoxVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FSkyBoxFragmentShader::StaticType();
					std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"SkyBox",
							"default",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					auto* VertexShader = static_cast<FSkyBoxVertexShader*>(
						ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader = static_cast<FSkyBoxFragmentShader*>(
						ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"SkyBox",
							"default",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					FSkyBoxRendererState::FPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader = TShaderRef<FSkyBoxVertexShader>(
						VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FSkyBoxFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					FVertexDeclarationElementList EmptyVertexElements{};
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(
							EmptyVertexElements);
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RendererRenderTargetLayouts::MakeSceneTargets();
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
						Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							"SkyBoxPipeline", Initializer);
					const std::array<uint32, 3> FullscreenIndices = {
						0, 1, 2};
					FRHIBufferCreateDesc IndexBufferDesc =
						FRHIBufferCreateDesc::CreateIndex(
							"SkyBoxFullscreenIndexBuffer",
							sizeof(FullscreenIndices),
							sizeof(uint32));
					IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
					IndexBufferDesc.InitialData = {
						FullscreenIndices.data(),
						sizeof(FullscreenIndices)};
					Candidate.IndexBuffer = RHICreateBuffer(IndexBufferDesc);
					Candidate.Sampler =
						RHICreateSampler(FRHISamplerDesc::LinearClamp());
					if (Candidate.VertexDeclaration == nullptr
						|| Candidate.PipelineState == nullptr
						|| Candidate.IndexBuffer == nullptr
						|| Candidate.Sampler == nullptr)
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"SkyBox",
							"default",
							"RHI resource creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					return FResult::Success(std::move(Candidate));
				},
				ReportStaticMeshCreateDiagnostic);
			if (Payload == nullptr)
				return;
			GSkyBoxState.ShaderMap = Payload->ShaderMap;
			GSkyBoxState.VertexShader = Payload->VertexShader;
			GSkyBoxState.FragmentShader = Payload->FragmentShader;
			GSkyBoxState.VertexDeclaration = Payload->VertexDeclaration;
			GSkyBoxState.PipelineState = Payload->PipelineState;
			GSkyBoxState.IndexBuffer = Payload->IndexBuffer;
			GSkyBoxState.Sampler = Payload->Sampler;
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
			if (!Diagnostic.Error)
			{
				return;
			}
			const FRenderResourceCreateError& Error = *Diagnostic.Error;
			if (Diagnostic.Kind
				== ERenderResourceCreateDiagnosticKind::Recovery)
			{
				DURIN_INFO(
					"Recovered renderer resource: context={}, identity={}",
					Error.Context,
					Error.Identity);
				return;
			}
			DURIN_ERROR(
				"Renderer resource creation failed: category={}, context={}, identity={}, generation={}/{}/{}, retained={}, message={}",
				static_cast<uint8>(Error.Category),
				Error.Context,
				Error.Identity,
				Error.AttemptedGeneration.Shader,
				Error.AttemptedGeneration.Device,
				Error.AttemptedGeneration.Manual,
				Error.bRetainedFallback,
				Error.Message);
		}

		auto MakeStaticMeshCreateError(
			ERenderResourceCreateErrorCategory Category,
			std::string Context,
			std::string Identity,
			std::string Message,
			ERenderResourceGenerationDependency RetryDependencies)
			-> FRenderResourceCreateError
		{
			return {
				.Category = Category,
				.Context = std::move(Context),
				.Identity = std::move(Identity),
				.Message = std::move(Message),
				.RetryDependencies = RetryDependencies,
			};
		}

		auto EnsureStaticMeshBaseResources()
			-> FStaticMeshRendererState::FBaseResources*
		{
			using FResult =
				TRenderResourceCreateResult<
					FStaticMeshRendererState::FBaseResources>;
			return GStaticMeshState.BaseResources.Resolve(
				GRendererResourceGeneration,
				[]() -> FResult {
					FStaticMeshRendererState::FBaseResources Candidate;
					const FVertexDeclarationElementList VertexDeclElements =
						GetStaticMeshVertexDeclarationElements();
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(
							VertexDeclElements);
					if (Candidate.VertexDeclaration == nullptr)
					{
						return FResult::Failure(MakeStaticMeshCreateError(
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
				GRendererResourceGeneration,
				[&Identity]() -> FResult {
					FShaderCompileOptions CompileOptions;
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
			const FMaterialPipelineIdentity& Identity)
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
				GRendererResourceGeneration;
			const auto* ShaderMapEntry =
				GStaticMeshState.ShaderMaps.Find(Identity.ShaderMap);
			check(ShaderMapEntry);
			PipelineGeneration.Shader =
				ShaderMapEntry->Slot.GetPayloadGeneration().Shader;
			return Entry.Slot.Resolve(
				PipelineGeneration,
				[&Identity,
				 &Entry,
				 BaseResources,
				 ShaderMapPayload]() -> FResult {
					FStaticMeshRendererState::FPipelinePayload Candidate;
					Candidate.ShaderMap = ShaderMapPayload->ShaderMap;
					Candidate.VertexShader = ShaderMapPayload->VertexShader;
					Candidate.FragmentShader = ShaderMapPayload->FragmentShader;

					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RendererRenderTargetLayouts::MakeSceneTargets();
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

		auto EnsureTextureCubeThumbnailPipeline() -> void
		{
			using FResult = TRenderResourceCreateResult<
				FTextureCubeThumbnailRendererState::FPayload>;
			auto* Payload = GTextureCubeThumbnailState.Slot.Resolve(
				GRendererResourceGeneration,
				[]() -> FResult {
					FShaderCompileOptions CompileOptions;
					FShaderType& VertexShaderType =
						FTextureCubeThumbnailVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FTextureCubeThumbnailFragmentShader::StaticType();
					std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"TextureCubeThumbnail",
							"default",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					auto* VertexShader =
						static_cast<FTextureCubeThumbnailVertexShader*>(
							ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader =
						static_cast<FTextureCubeThumbnailFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"TextureCubeThumbnail",
							"default",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					FTextureCubeThumbnailRendererState::FPayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader =
						TShaderRef<FTextureCubeThumbnailVertexShader>(
							VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FTextureCubeThumbnailFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					const FVertexDeclarationElementList VertexDeclElements =
						GetStaticMeshVertexDeclarationElements();
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(
							VertexDeclElements);
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RendererRenderTargetLayouts::MakeSceneTargets();
					Initializer.BoundShaders.VertexShader =
						Candidate.VertexShader.GetRHIShader();
					Initializer.BoundShaders.FragmentShader =
						Candidate.FragmentShader.GetRHIShader();
					Initializer.VertexDeclaration =
						Candidate.VertexDeclaration;
					Initializer.bEnableAlphaBlend = false;
					Initializer.bEnableDepthTest = true;
					Initializer.bEnableDepthWrite = true;
					Initializer.bEnableBackFaceCulling = false;
					Initializer.PipelineLayout =
						Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							"TextureCubeThumbnailPipeline", Initializer);
					Candidate.Sampler =
						RHICreateSampler(FRHISamplerDesc::LinearClamp());
					if (Candidate.VertexDeclaration == nullptr
						|| Candidate.PipelineState == nullptr
						|| Candidate.Sampler == nullptr)
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"TextureCubeThumbnail",
							"default",
							"RHI resource creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					return FResult::Success(std::move(Candidate));
				},
				ReportStaticMeshCreateDiagnostic);
			if (Payload == nullptr)
				return;
			GTextureCubeThumbnailState.ShaderMap = Payload->ShaderMap;
			GTextureCubeThumbnailState.VertexShader = Payload->VertexShader;
			GTextureCubeThumbnailState.FragmentShader =
				Payload->FragmentShader;
			GTextureCubeThumbnailState.VertexDeclaration =
				Payload->VertexDeclaration;
			GTextureCubeThumbnailState.PipelineState =
				Payload->PipelineState;
			GTextureCubeThumbnailState.Sampler = Payload->Sampler;
		}

		auto CreatePostProcessPipeline(
			FName PipelineName,
			FRHIShader* VertexShader,
			FRHIShader* FragmentShader,
			const FVertexDeclarationRHIRef& VertexDeclaration,
			const FPipelineLayoutDesc& PipelineLayout,
			const FRHIRenderTargetLayout& RenderTargetLayout
		) -> FGraphicsPipelineStateRHIRef
		{
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RenderTargetLayout;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = VertexDeclaration;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = PipelineLayout;
			return GDynamicRHI->RHICreateGraphicsPipelineState(PipelineName, Initializer);
		}

		auto EnsurePostProcessResources(FRHICommandListImmediate& CommandList) -> void
		{
			using FPayload = FPostProcessRendererState::FPayload;
			using FResult = TRenderResourceCreateResult<FPayload>;
			auto* Payload = GPostProcessState.Slot.Resolve(
				GRendererResourceGeneration,
				[&CommandList]() -> FResult {
					FShaderCompileOptions CompileOptions;
					FShaderType& VertexShaderType =
						FPostProcessVertexShader::StaticType();
					FShaderType& CopyFragmentShaderType =
						FCopySceneColorFragmentShader::StaticType();
					FShaderType& FXAAFragmentShaderType =
						FFXAAFragmentShader::StaticType();
					std::array<const FShaderType*, 2> CopyShaderTypes = {
						&VertexShaderType, &CopyFragmentShaderType};
					std::array<const FShaderType*, 2> FXAAShaderTypes = {
						&VertexShaderType, &FXAAFragmentShaderType};
					FPayload Candidate;
					Candidate.CopyShaderMap =
						std::make_shared<FShaderMapBase>();
					Candidate.FXAAShaderMap =
						std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!Candidate.CopyShaderMap->InitializeFromShaderTypes(
							CopyShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"PostProcess",
							"copy",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					if (!Candidate.FXAAShaderMap->InitializeFromShaderTypes(
							FXAAShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"PostProcess",
							"fxaa",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					auto* CopyVertexShader =
						static_cast<FPostProcessVertexShader*>(
							Candidate.CopyShaderMap->GetShader(
								&VertexShaderType));
					auto* FXAAVertexShader =
						static_cast<FPostProcessVertexShader*>(
							Candidate.FXAAShaderMap->GetShader(
								&VertexShaderType));
					auto* CopyFragmentShader =
						static_cast<FCopySceneColorFragmentShader*>(
							Candidate.CopyShaderMap->GetShader(
								&CopyFragmentShaderType));
					auto* FXAAFragmentShader =
						static_cast<FFXAAFragmentShader*>(
							Candidate.FXAAShaderMap->GetShader(
								&FXAAFragmentShaderType));
					if (CopyVertexShader == nullptr
						|| FXAAVertexShader == nullptr
						|| CopyFragmentShader == nullptr
						|| FXAAFragmentShader == nullptr)
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"PostProcess",
							"copy+fxaa",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					Candidate.CopyVertexShader =
						TShaderRef<FPostProcessVertexShader>(
							CopyVertexShader,
							Candidate.CopyShaderMap.get());
					Candidate.FXAAVertexShader =
						TShaderRef<FPostProcessVertexShader>(
							FXAAVertexShader,
							Candidate.FXAAShaderMap.get());
					Candidate.CopyFragmentShader =
						TShaderRef<FCopySceneColorFragmentShader>(
							CopyFragmentShader,
							Candidate.CopyShaderMap.get());
					Candidate.FXAAFragmentShader =
						TShaderRef<FFXAAFragmentShader>(
							FXAAFragmentShader,
							Candidate.FXAAShaderMap.get());
					FVertexDeclarationElementList VertexDeclElements;
					constexpr uint32 VertexStride =
						sizeof(RendererFullscreenGeometry::FVertex);
					VertexDeclElements[0] = FVertexElement(
						0,
						offsetof(
							RendererFullscreenGeometry::FVertex, Position),
						EVertexElementType::Float2, 0, VertexStride);
					VertexDeclElements[1] = FVertexElement(
						0,
						offsetof(RendererFullscreenGeometry::FVertex, UV),
						EVertexElementType::Float2, 1, VertexStride);
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(
							VertexDeclElements);
					if (!RendererFullscreenGeometry::EnsureResources(
							CommandList))
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"PostProcess",
							"fullscreen-geometry",
							"Shared fullscreen geometry is unavailable.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					Candidate.SceneColorSampler =
						RHICreateSampler(FRHISamplerDesc::LinearClamp());
					auto MakePipeline = [&](FName Name, FRHIShader* VS,
											FRHIShader* FS,
											const FPipelineLayoutDesc& Layout,
											const FRHIRenderTargetLayout& RT) {
						return CreatePostProcessPipeline(
							Name, VS, FS, Candidate.VertexDeclaration,
							Layout, RT);
					};
					Candidate.CopyIntermediatePipelineState = MakePipeline(
						"PostProcessCopyIntermediatePipeline",
						Candidate.CopyVertexShader.GetRHIShader(),
						Candidate.CopyFragmentShader.GetRHIShader(),
						Candidate.CopyShaderMap->GetMergedPipelineLayout(),
						RendererRenderTargetLayouts::
							MakeScenePostProcessOutput());
					Candidate.FXAAIntermediatePipelineState = MakePipeline(
						"PostProcessFXAAIntermediatePipeline",
						Candidate.FXAAVertexShader.GetRHIShader(),
						Candidate.FXAAFragmentShader.GetRHIShader(),
						Candidate.FXAAShaderMap->GetMergedPipelineLayout(),
						RendererRenderTargetLayouts::
							MakeScenePostProcessOutput());
					Candidate.CopyOffscreenPipelineState = MakePipeline(
						"PostProcessCopyOffscreenPipeline",
						Candidate.CopyVertexShader.GetRHIShader(),
						Candidate.CopyFragmentShader.GetRHIShader(),
						Candidate.CopyShaderMap->GetMergedPipelineLayout(),
						RendererRenderTargetLayouts::
							MakeFinalScenePostProcessOutput(
								RendererRenderTargetLayouts::
									EViewportOutput::Offscreen));
					Candidate.CopyPresentPipelineState = MakePipeline(
						"PostProcessCopyPresentPipeline",
						Candidate.CopyVertexShader.GetRHIShader(),
						Candidate.CopyFragmentShader.GetRHIShader(),
						Candidate.CopyShaderMap->GetMergedPipelineLayout(),
						RendererRenderTargetLayouts::
							MakeFinalScenePostProcessOutput(
								RendererRenderTargetLayouts::
									EViewportOutput::Present));
					Candidate.FXAAOffscreenPipelineState = MakePipeline(
						"PostProcessFXAAOffscreenPipeline",
						Candidate.FXAAVertexShader.GetRHIShader(),
						Candidate.FXAAFragmentShader.GetRHIShader(),
						Candidate.FXAAShaderMap->GetMergedPipelineLayout(),
						RendererRenderTargetLayouts::
							MakeFinalScenePostProcessOutput(
								RendererRenderTargetLayouts::
									EViewportOutput::Offscreen));
					Candidate.FXAAPresentPipelineState = MakePipeline(
						"PostProcessFXAAPresentPipeline",
						Candidate.FXAAVertexShader.GetRHIShader(),
						Candidate.FXAAFragmentShader.GetRHIShader(),
						Candidate.FXAAShaderMap->GetMergedPipelineLayout(),
						RendererRenderTargetLayouts::
							MakeFinalScenePostProcessOutput(
								RendererRenderTargetLayouts::
									EViewportOutput::Present));
					if (Candidate.VertexDeclaration == nullptr
						|| Candidate.SceneColorSampler == nullptr
						|| Candidate.CopyIntermediatePipelineState == nullptr
						|| Candidate.FXAAIntermediatePipelineState == nullptr
						|| Candidate.CopyOffscreenPipelineState == nullptr
						|| Candidate.CopyPresentPipelineState == nullptr
						|| Candidate.FXAAOffscreenPipelineState == nullptr
						|| Candidate.FXAAPresentPipelineState == nullptr)
						return FResult::Failure(MakeStaticMeshCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"PostProcess",
							"copy+fxaa",
							"RHI resource or pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					return FResult::Success(std::move(Candidate));
				},
				ReportStaticMeshCreateDiagnostic);
			if (Payload == nullptr)
				return;
			GPostProcessState.CopyShaderMap = Payload->CopyShaderMap;
			GPostProcessState.FXAAShaderMap = Payload->FXAAShaderMap;
			GPostProcessState.CopyVertexShader = Payload->CopyVertexShader;
			GPostProcessState.FXAAVertexShader = Payload->FXAAVertexShader;
			GPostProcessState.CopyFragmentShader =
				Payload->CopyFragmentShader;
			GPostProcessState.FXAAFragmentShader =
				Payload->FXAAFragmentShader;
			GPostProcessState.VertexDeclaration =
				Payload->VertexDeclaration;
			GPostProcessState.CopyIntermediatePipelineState =
				Payload->CopyIntermediatePipelineState;
			GPostProcessState.FXAAIntermediatePipelineState =
				Payload->FXAAIntermediatePipelineState;
			GPostProcessState.CopyOffscreenPipelineState =
				Payload->CopyOffscreenPipelineState;
			GPostProcessState.CopyPresentPipelineState =
				Payload->CopyPresentPipelineState;
			GPostProcessState.FXAAOffscreenPipelineState =
				Payload->FXAAOffscreenPipelineState;
			GPostProcessState.FXAAPresentPipelineState =
				Payload->FXAAPresentPipelineState;
			GPostProcessState.SceneColorSampler = Payload->SceneColorSampler;
		}

		auto EnsureSceneTargets(uint32 Width, uint32 Height) -> FPostProcessRendererState::FSceneTargets*
		{
			const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
			if (auto It = GPostProcessState.SceneTargetsBySize.find(Key); It != GPostProcessState.SceneTargetsBySize.end())
			{
				return &It->second;
			}

			FRHITextureCreateDesc SceneColorDesc = FRHITextureCreateDesc::Create2D("SceneColor", Width, Height, EPixelFormat::SRGBA8_UNORM);
			SceneColorDesc.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
			SceneColorDesc.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
			FRHITextureCreateDesc SceneDepthDesc = FRHITextureCreateDesc::Create2D("SceneDepth", Width, Height, EPixelFormat::D32);
			SceneDepthDesc.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
			SceneDepthDesc.SetClearValue(FClearValueBinding(1.0f, 0u));
			auto [It, bInserted] = GPostProcessState.SceneTargetsBySize.emplace(Key, FPostProcessRendererState::FSceneTargets{
				.Color = RHICreateTexture(SceneColorDesc),
				.Depth = RHICreateTexture(SceneDepthDesc),
			});
			// Interactive viewport resizing can produce many transient dimensions. Keep a
			// small pool so the main view and camera previews reuse their stable sizes
			// without retaining every intermediate drag size for the entire session.
			if (GPostProcessState.SceneTargetsBySize.size() > 8)
			{
				const auto EvictionIt = std::ranges::find_if(GPostProcessState.SceneTargetsBySize, [Key](const auto& Entry) { return Entry.first != Key; });
				if (EvictionIt != GPostProcessState.SceneTargetsBySize.end()) GPostProcessState.SceneTargetsBySize.erase(EvictionIt);
			}
			return bInserted ? &It->second : nullptr;
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}

		auto DrawSkyBox(FRHICommandListImmediate& CommandList, IScene& Scene, const FSceneView& View) -> void
		{
			FSkyBoxSceneData SkyBox;
			if (!Scene.GetActiveSkyBox_RenderThread(SkyBox)) return;

			if (GSkyBoxState.PipelineState == nullptr || GSkyBoxState.Sampler == nullptr
				|| !GSkyBoxState.VertexShader || !GSkyBoxState.FragmentShader
				|| GSkyBoxState.IndexBuffer == nullptr) return;

			SkyBoxRendering::FSkyBoxUniform Uniform;
			if (!SkyBoxRendering::BuildUniform(View, SkyBox, Uniform)) return;

			FRHITexture* Texture = SkyBox.TextureReference != nullptr
				? SkyBox.TextureReference->GetReferencedTexture_RenderThread()
				: nullptr;
			if (Texture == nullptr)
				Texture = GetDefaultCubeTexture_RenderThread();
			if (Texture == nullptr) return;

			CommandList.SetGraphicsPipelineState(*GSkyBoxState.PipelineState);
			CommandList.BindIndexBuffer(GSkyBoxState.IndexBuffer, 0);
			FSkyBoxFragmentShader::FParameters Parameters;
			Parameters.SkyTexture = Texture;
			Parameters.SkySampler = GSkyBoxState.Sampler;
			Parameters.Sky = CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
			SetShaderParameters(CommandList, GSkyBoxState.FragmentShader, Parameters);
			CommandList.DrawIndexed(3, 0, 0);
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

			CommandList.BindVertexBuffer(
				0,
				LOD.VertexBuffers.PositionVertexBuffer.GetRHI(),
				0);
			CommandList.BindVertexBuffer(
				1,
				LOD.VertexBuffers.StaticMeshVertexBuffer.GetRHI(),
				0);
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
					GetOrCreateStaticMeshPipeline(Material.PipelineIdentity);
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

		auto DrawTextureCubeThumbnailProxy(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FTextureCubePreviewSceneProxy& Proxy) -> void
		{
			const FRHITextureReferenceRef& TextureReference =
				Proxy.GetTextureReference();
			if (TextureReference == nullptr) return;
			FRHITexture* Texture =
				TextureReference->GetReferencedTexture_RenderThread();
			if (Texture == nullptr) return;

			if (GSkyBoxState.PipelineState == nullptr
				|| GSkyBoxState.Sampler == nullptr
				|| !GSkyBoxState.VertexShader
				|| !GSkyBoxState.FragmentShader
				|| GSkyBoxState.IndexBuffer == nullptr)
				return;

			// Content Browser thumbnails favor recognition over inspection: show a
			// wide environment view here and reserve the reflective sphere for the
			// interactive TextureCube editor.
			constexpr float EnvironmentVerticalFieldOfViewDegrees = 100.0f;
			FSceneView EnvironmentView = View;
			const float AspectRatio = static_cast<float>(View.ViewportWidth)
				/ static_cast<float>(std::max(1u, View.ViewportHeight));
			const float YScale = 1.0f
				/ std::tan(
					glm::radians(EnvironmentVerticalFieldOfViewDegrees) * 0.5f);
			EnvironmentView.ProjectionMatrix[1][0] =
				YScale / std::max(AspectRatio, 0.001f);
			EnvironmentView.ProjectionMatrix[2][1] = -YScale;
			EnvironmentView.ViewProjectionMatrix =
				EnvironmentView.ProjectionMatrix * EnvironmentView.ViewMatrix;

			SkyBoxRendering::FSkyBoxUniform Uniform;
			if (!SkyBoxRendering::BuildUniform(
					EnvironmentView, FSkyBoxSceneData{}, Uniform))
				return;

			CommandList.SetGraphicsPipelineState(*GSkyBoxState.PipelineState);
			CommandList.BindIndexBuffer(GSkyBoxState.IndexBuffer, 0);
			FSkyBoxFragmentShader::FParameters FragmentParameters;
			FragmentParameters.SkyTexture = Texture;
			FragmentParameters.SkySampler = GSkyBoxState.Sampler;
			FragmentParameters.Sky =
				CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
			SetShaderParameters(
				CommandList,
				GSkyBoxState.FragmentShader,
				FragmentParameters);
			CommandList.DrawIndexed(3, 0, 0);
		}

		auto DrawPostProcess(
			FRHICommandListImmediate& CommandList,
			FRHITexture* SceneColor,
			uint32 Width,
			uint32 Height,
			bool bPresentOutput,
			bool bEnableFXAA,
			bool bHasEditorAssistance
		) -> void
		{
			FGraphicsPipelineStateRHIRef PipelineState;
			if (bHasEditorAssistance)
			{
				PipelineState = bEnableFXAA
					? GPostProcessState.FXAAIntermediatePipelineState
					: GPostProcessState.CopyIntermediatePipelineState;
			}
			else
			{
				PipelineState = bEnableFXAA
					? (bPresentOutput
						? GPostProcessState.FXAAPresentPipelineState
						: GPostProcessState.FXAAOffscreenPipelineState)
					: (bPresentOutput
						? GPostProcessState.CopyPresentPipelineState
						: GPostProcessState.CopyOffscreenPipelineState);
			}
			if (PipelineState == nullptr
				|| RendererFullscreenGeometry::GetVertexBuffer() == nullptr
				|| RendererFullscreenGeometry::GetIndexBuffer() == nullptr)
			{
				return;
			}

			CommandList.SetGraphicsPipelineState(*PipelineState);
			CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 1.0f);
			CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
			CommandList.BindVertexBuffer(
				0, RendererFullscreenGeometry::GetVertexBuffer(), 0);
			CommandList.BindIndexBuffer(
				RendererFullscreenGeometry::GetIndexBuffer(), 0);

			if (bEnableFXAA)
			{
				FPostProcessViewUniform ViewUniform;
				ViewUniform.InvRenderTargetSize = FVector2f(1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
				const FRHIUniformBufferRange ViewUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&ViewUniform, sizeof(ViewUniform));

				FFXAAFragmentShader::FParameters FragmentParameters;
				FragmentParameters.SceneColor = SceneColor;
				FragmentParameters.SceneColorSampler = GPostProcessState.SceneColorSampler;
				FragmentParameters.View = ViewUniformBuffer;
				SetShaderParameters(CommandList, GPostProcessState.FXAAFragmentShader, FragmentParameters);
			}
			else
			{
				FCopySceneColorFragmentShader::FParameters FragmentParameters;
				FragmentParameters.SceneColor = SceneColor;
				FragmentParameters.SceneColorSampler = GPostProcessState.SceneColorSampler;
				SetShaderParameters(CommandList, GPostProcessState.CopyFragmentShader, FragmentParameters);
			}

			CommandList.DrawIndexed(3, 0, 0);
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

	auto GetDefaultTexture_RenderThread(EDefaultTexture Texture) -> FRHITexture*
	{
		check(IsInRenderingThread());
		switch (Texture)
		{
		case EDefaultTexture::White: return GDefaultTextures.White;
		case EDefaultTexture::Black: return GDefaultTextures.Black;
		case EDefaultTexture::FlatNormal: return GDefaultTextures.FlatNormal;
		}
		return GDefaultTextures.White;
	}

	auto ResolveTexture_RenderThread(
		const FRHITextureReferenceRef& TextureReference,
		EDefaultTexture Fallback) -> FRHITexture*
	{
		check(IsInRenderingThread());
		if (TextureReference != nullptr)
		{
			if (FRHITexture* Texture =
				TextureReference->GetReferencedTexture_RenderThread())
			{
				return Texture;
			}
		}
		return GetDefaultTexture_RenderThread(Fallback);
	}

	auto GetDefaultCubeTexture_RenderThread() -> FRHITexture*
	{
		check(IsInRenderingThread());
		return GDefaultTextures.BlackCube;
	}

	auto FRendererModule::StartupModule() -> void
	{
		if (GDynamicRHI != nullptr)
		{
			ENQUEUE_RENDER_COMMAND(InitializeDefaultTextures)(
				[](FRHICommandListImmediate& CommandList) {
					InitializeDefaultTextures_RenderThread(CommandList);
				});
		}
	}

	static auto ReleaseRendererResources() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseRendererResources)([](FRHICommandListImmediate&) {
			GDefaultTextures = {};
			GStaticMeshState = {};
			GRendererResourceGeneration = {};
			GTextureCubeThumbnailState.Slot.Reset();
			GTextureCubeThumbnailState = {};
			GSkyBoxState.Slot.Reset();
			GSkyBoxState = {};
			RendererEditorAssistance::ReleaseResources();
			GPostProcessState.Slot.Reset();
			GPostProcessState = {};
			RendererFullscreenGeometry::ReleaseResources();
		});
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		ReleaseRendererResources();
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
		const RendererRenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			RendererEditorAssistance::AnalyzeRequest(View, ViewportOutput);

		EnsurePostProcessResources(CommandList);
		// Sky resources include a static index upload, so initialize them before
		// entering the Scene Color render pass.
		EnsureSkyBoxResources();
		FPostProcessRendererState::FSceneTargets* SceneTargets = EnsureSceneTargets(Width, Height);
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
		ScenePassInfo.RenderTargetLayout = RendererRenderTargetLayouts::MakeSceneTargets();
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
			? RendererRenderTargetLayouts::MakeScenePostProcessOutput()
			: RendererRenderTargetLayouts::MakeFinalScenePostProcessOutput(
				ViewportOutput);
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
		CommandList.BeginRenderPass(PostProcessPassInfo, bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass");
		DrawPostProcess(
			CommandList, SceneColor, Width, Height, bPresentOutput,
			View.Settings.bEnableFXAA, bHasEditorAssistance);
		CommandList.EndRenderPass();
		if (!bHasEditorAssistance) return;

		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RendererRenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
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

		DrawSkyBox(CommandList, *Scene, View);

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
			[&CommandList, &View](FTextureCubePreviewSceneProxy& Proxy) {
				DrawTextureCubeThumbnailProxy(CommandList, View, Proxy);
			});
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
}
