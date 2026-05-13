#include "ImGuiRHIImpl.h"

#include "RHI.h"
#include "RenderingThread.h"
#include "Shader/ShaderPaths.h"
#include "Shader/ShaderCompiler.h"

namespace Doge::Mona::MonaImGuiBackend
{
	ImGuiContext* GMonaImGuiContext = nullptr;

	// Reusable buffers used for rendering 1 current in-flight frame
	struct FImGuiRHIImpl_FrameRenderBuffers
	{
		FBufferRHIRef VertexBuffer;
		FBufferRHIRef IndexBuffer;
	};

	struct FImGuiRHIImpl_WindowRenderBuffers
	{
		std::array<FImGuiRHIImpl_FrameRenderBuffers, kFrameInFlight> FrameRenderBuffers;

		auto Clear() -> void
		{
			for (FImGuiRHIImpl_FrameRenderBuffers& Buffers : FrameRenderBuffers)
			{
				Buffers.VertexBuffer = nullptr;
				Buffers.IndexBuffer = nullptr;
			}
		}
	};

	static auto CalcOrthoProj(float L, float R, float B, float T) -> FMatrix
	{
		FMatrix Result(1.0f);
		Result[0][0] = 2.0f / (R - L);
		Result[1][1] = 2.0f / (T - B);
		Result[2][2] = -1.0f;
		Result[3][0] = -(R + L) / (R - L);
		Result[3][1] = -(T + B) / (T - B);
		return Result;
	}

	struct FImGuiRHIImpl_BackendState
	{
		FShaderRHIRef VertexShader;
		FShaderRHIRef PixelShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;
		FBufferRHIRef ProjectionUBO;

		// Font atlas
		FTextureRHIRef FontAtlasTexture;

		// Render buffers for main window
		FImGuiRHIImpl_WindowRenderBuffers MainWindowRenderBuffers;

		bool bInitialized = false;
	};

	static FImGuiRHIImpl_BackendState GBackendState;

	static auto ImGuiRHIImpl_CreateMainPipeline()
	{
		// Compile shaders
		auto VertexShaderCode = std::make_shared<std::vector<uint32>>();
		auto PixelShaderCode = std::make_shared<std::vector<uint32>>();

		const std::string ImGuiShaderName = "/Engine/ImGui";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {"vertexMain", "fragmentMain"};
		if (FShaderCompilerOutput CompileResult = GShaderCompiler->Compile(FShaderPaths::SourcePath(ImGuiShaderName), CompileOptions))
		{
			CompileResult.Codes[0].swap(*VertexShaderCode);
			CompileResult.Codes[1].swap(*PixelShaderCode);
		}
		else
		{
			DOGE_ERROR("Failed to compile ImGui shader: {}", CompileResult.ErrorMessage);
		}

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([VertexShaderCode, PixelShaderCode](FRHICommandListImmediate& CommandList) {
			const FRHIShaderCreateDesc VertexShaderCreateDesc = FRHIShaderCreateDesc::CreateVertex("ImGuiVertexShader", *VertexShaderCode, {});
			GBackendState.VertexShader = GDynamicRHI->RHICreateShader(VertexShaderCreateDesc);

			const FRHIShaderCreateDesc PixelShaderCreateDesc = FRHIShaderCreateDesc::CreatePixel("ImGuiPixelShader", *PixelShaderCode, {});
			GBackendState.PixelShader = GDynamicRHI->RHICreateShader(PixelShaderCreateDesc);

			FVertexDeclarationElementList VertexDeclElements;
			constexpr uint32 VertexStride = sizeof(ImDrawVert);
			VertexDeclElements[0] = FVertexElement(0, 0, EVertexElementType::Float2, 0, VertexStride);
			VertexDeclElements[1] = FVertexElement(0, 0, EVertexElementType::Float2, 1, VertexStride);
			VertexDeclElements[2] = FVertexElement(0, 0, EVertexElementType::Float4, 2, VertexStride);
			GBackendState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderPassName = "ImGuiRenderPass";
			Initializer.BoundShaders.VertexShader = GBackendState.VertexShader;
			Initializer.BoundShaders.PixelShader = GBackendState.PixelShader;
			Initializer.VertexDeclaration = GBackendState.VertexDeclaration;

			Initializer.PixelFormat = EPixelFormat::SRGBA8_UNORM;
			GDynamicRHI->RHICreateGraphicsPipelineState("ImGuiMainPipeline", Initializer);
		});
	}

	static auto ImGuiRHIImpl_CreateRHIResources()
	{
		ImGuiRHIImpl_CreateMainPipeline();
	}

	static auto ImGuiRHIImpl_ClearRHIResources() -> void
	{
		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([](FRHICommandListImmediate& CommandList) {
			GBackendState.VertexShader = nullptr;
			GBackendState.PixelShader = nullptr;
			GBackendState.VertexDeclaration = nullptr;
			GBackendState.PipelineState = nullptr;
			GBackendState.ProjectionUBO = nullptr;
			GBackendState.FontAtlasTexture = nullptr;
			GBackendState.MainWindowRenderBuffers.Clear();
		});
	}

	auto ImGuiRHIImpl_Init() -> void
	{
		check(GDynamicRHI);
		ImGuiIO& IO = ImGui::GetIO();
		IMGUI_CHECKVERSION();

		IO.BackendRendererUserData = GDynamicRHI;
		IO.BackendRendererName = "DogeRHI";
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // We can honor ImGuiPlatformIO::Textures[] requests during render.

		// Get font atlas pixel data on main thread
		unsigned char* FontPixels = nullptr;
		int FontWidth = 0, FontHeight = 0;
		IO.Fonts->GetTexDataAsRGBA32(&FontPixels, &FontWidth, &FontHeight);
		const uint32 FontDataSize = FontWidth * FontHeight * 4;

		ImGuiRHIImpl_CreateRHIResources();
	}

	auto ImGuiRHIImpl_Shutdown() -> void
	{
		ImGuiRHIImpl_ClearRHIResources();

		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendRendererUserData = nullptr;
		IO.BackendRendererName = nullptr;
		IO.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.ClearPlatformHandlers();
	}

	auto ImGuiRHIImpl_NewFrame() -> void
	{
	}

	static auto RenderDrawData_RenderThread(FRHICommandListImmediate& CommandList, const ImDrawData* DrawData, FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers) -> void
	{
		check(IsInRenderingThread());

		// Update buffers
		if (DrawData->TotalVtxCount > 0)
		{
			if (RenderBuffers.VertexBuffer == nullptr || RenderBuffers.VertexBuffer->GetSize() < DrawData->TotalVtxCount * sizeof(ImDrawVert))
			{
				FRHIBufferCreateDesc VertexBufferCreateDesc = FRHIBufferCreateDesc::CreateVertex("ImGuiVertexBuffer", DrawData->TotalVtxCount * sizeof(ImDrawVert));
				VertexBufferCreateDesc.Usage |= EBufferUsageFlags::Dynamic;
				RenderBuffers.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexBufferCreateDesc);
			}
			if (RenderBuffers.IndexBuffer == nullptr || RenderBuffers.IndexBuffer->GetSize() < DrawData->TotalIdxCount * sizeof(ImDrawIdx))
			{
				FRHIBufferCreateDesc IndexBufferCreateDesc = FRHIBufferCreateDesc::CreateIndex("ImGuiIndexBuffer", DrawData->TotalIdxCount * sizeof(ImDrawIdx), sizeof(ImDrawIdx));
				IndexBufferCreateDesc.Usage |= EBufferUsageFlags::Dynamic;
				RenderBuffers.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexBufferCreateDesc);
			}

			void* VertexBufferData = GDynamicRHI->RHILockBuffer(CommandList, RenderBuffers.VertexBuffer, 0, RenderBuffers.VertexBuffer->GetSize(), EResourceLockMode::WriteOnly);
			void* IndexBufferData = GDynamicRHI->RHILockBuffer(CommandList, RenderBuffers.IndexBuffer, 0, RenderBuffers.IndexBuffer->GetSize(), EResourceLockMode::WriteOnly);

			auto VertexBufferDst = static_cast<ImDrawVert*>(VertexBufferData);
			auto IndexBufferDst = static_cast<ImDrawIdx*>(IndexBufferData);

			for (const ImDrawList* DrawList : DrawData->CmdLists)
			{
				memcpy(VertexBufferDst, DrawList->VtxBuffer.Data, DrawList->VtxBuffer.Size * sizeof(ImDrawVert));
				memcpy(IndexBufferDst, DrawList->IdxBuffer.Data, DrawList->IdxBuffer.Size * sizeof(ImDrawIdx));
				VertexBufferDst += DrawList->VtxBuffer.Size;
				IndexBufferDst += DrawList->IdxBuffer.Size;
			}

			GDynamicRHI->RHIUnlockBuffer(CommandList, RenderBuffers.VertexBuffer);
			GDynamicRHI->RHIUnlockBuffer(CommandList, RenderBuffers.IndexBuffer);
		}
	}

	auto ImGuiRHIImpl_RenderDrawData(ImDrawData* DrawData) -> void
	{
		const int32 FrameBufferWidth = static_cast<int32>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		const int32 FrameBufferHeight = static_cast<int32>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);
		if (FrameBufferWidth <= 0 || FrameBufferHeight <= 0)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(RenderDrawData)([DrawData](FRHICommandListImmediate& CommandList) {
			auto& RenderBuffersCurrentFrame = GBackendState.MainWindowRenderBuffers.FrameRenderBuffers[GFrameCounterRenderThread % kFrameInFlight];
			RenderDrawData_RenderThread(CommandList, DrawData, RenderBuffersCurrentFrame);
		});
	}

	auto ImGuiRHIImpl_UpdateTexture(ImTextureData* TextureData) -> void
	{
	}


} // namespace Doge::Mona::MonaImGuiBackend