#include "RendererModule.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "Scene.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

#include <glm/mat4x4.hpp>

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

			DURIN_DECLARE_SHADER(FStaticMeshVertexShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Vertex, "VertexMain");
		};

		class FStaticMeshFragmentShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FStaticMeshFragmentShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Fragment, "FragmentMain");
		};

		struct FStaticMeshTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
		};

		struct FStaticMeshRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			bool bCreateAttempted = false;
		};

		FStaticMeshRendererState GStaticMeshState;

		auto EnsureStaticMeshPipeline() -> void
		{
			if (GStaticMeshState.bCreateAttempted)
			{
				return;
			}

			GStaticMeshState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FStaticMeshVertexShader::StaticType();
			FShaderType& FragmentShaderType = FStaticMeshFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize StaticMesh shader map: {}", ErrorMessage);
				return;
			}

			auto* VertexShader = static_cast<FStaticMeshVertexShader*>(ShaderMap->GetShader(&VertexShaderType));
			auto* FragmentShader = static_cast<FStaticMeshFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
			check(VertexShader);
			check(FragmentShader);

			GStaticMeshState.ShaderMap = ShaderMap;
			GStaticMeshState.VertexShader = TShaderRef<FStaticMeshVertexShader>(VertexShader, ShaderMap.get());
			GStaticMeshState.FragmentShader = TShaderRef<FStaticMeshFragmentShader>(FragmentShader, ShaderMap.get());

			FVertexDeclarationElementList VertexDeclElements;
			constexpr uint32 VertexStride = sizeof(FVector3f);
			VertexDeclElements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, VertexStride);
			GStaticMeshState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderPassName = "StaticMeshRenderPass";
			Initializer.BoundShaders.VertexShader = GStaticMeshState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GStaticMeshState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GStaticMeshState.VertexDeclaration;
			Initializer.PixelFormat = EPixelFormat::SRGBA8_UNORM;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GStaticMeshState.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("StaticMeshMainPipeline", Initializer);
		}

		auto DrawStaticMeshProxy(FRHICommandListImmediate& CommandList, const FStaticMeshSceneProxy& Proxy) -> void
		{
			FStaticMeshRenderData* RenderData = Proxy.GetRenderData();
			if (RenderData == nullptr || RenderData->IndexCount == 0)
			{
				return;
			}

			if (!RenderData->IsReadyForRendering())
			{
				return;
			}

			FStaticMeshTransformUniform TransformUniform;
			const FRHIUniformBufferRange TransformUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&TransformUniform, sizeof(TransformUniform));

			CommandList.SetGraphicsPipelineState(*GStaticMeshState.PipelineState);

			FStaticMeshVertexShader::FParameters VertexShaderParameters;
			VertexShaderParameters.Transform = TransformUniformBuffer;
			SetShaderParameters(CommandList, GStaticMeshState.VertexShader, VertexShaderParameters);

			CommandList.BindVertexBuffer(0, RenderData->PositionVertexBufferRHI, 0);
			CommandList.BindIndexBuffer(RenderData->IndexBufferRHI, 0);
			CommandList.DrawIndexed(RenderData->IndexCount, 0, 0);
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
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		GStaticMeshState = {};
	}

	auto FRendererModule::CreateScene() -> std::unique_ptr<IScene>
	{
		return std::make_unique<FScene>();
	}

	auto FRendererModule::PrepareSceneResources(FRHICommandListImmediate& CommandList, IScene* Scene) -> void
	{
		ForEachStaticMeshProxy(Scene, [&CommandList](FStaticMeshSceneProxy& Proxy) {
			if (FStaticMeshRenderData* RenderData = Proxy.GetRenderData())
			{
				RenderData->InitResources(CommandList);
			}
		});
	}

	auto FRendererModule::RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, FRHITexture* RenderTarget, uint32 Width, uint32 Height) -> void
	{
		if (Scene == nullptr || RenderTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}

		EnsureStaticMeshPipeline();
		if (GStaticMeshState.PipelineState == nullptr || !GStaticMeshState.VertexShader || !GStaticMeshState.FragmentShader)
		{
			return;
		}

		CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));

		ForEachStaticMeshProxy(Scene, [&CommandList](FStaticMeshSceneProxy& Proxy) {
			DrawStaticMeshProxy(CommandList, Proxy);
		});
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
}
