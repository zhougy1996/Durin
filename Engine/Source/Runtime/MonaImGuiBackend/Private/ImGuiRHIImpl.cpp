#include "ImGuiRHIImpl.h"

#include "RHI.h"
#include "RenderingThread.h"
#include "Misc/FileHelper.h"
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
		FBufferRHIRef ProjectionUniform;
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
				Buffers.ProjectionUniform = nullptr;
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

	// State of the ImGui RHI backend, stored in a struct to ensure proper initialization order of static variables.
	// Only accessed from the render thread, so no synchronization is needed.
	struct FImGuiRHIImplRT_BackendState
	{
		FShaderRHIRef VertexShader;
		FShaderRHIRef PixelShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;

		// Font atlas
		FTextureRHIRef FontAtlasTexture;

		// Render buffers for main window
		FImGuiRHIImpl_WindowRenderBuffers MainWindowRenderBuffers;

		bool bInitialized = false;
	};

	struct FImGuiRHIImpl_ConstantBufferData
	{
		FVector2f Scale;
		FVector2f Translation;
	};

	static FImGuiRHIImplRT_BackendState GBackendState;

	static auto ImGuiRHIImpl_CreateFontAtlasTexture() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		unsigned char* FontPixels = nullptr;
		int FontWidth = 0, FontHeight = 0;
		IO.Fonts->GetTexDataAsRGBA32(&FontPixels, &FontWidth, &FontHeight);

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([FontWidth, FontHeight, FontPixels](FRHICommandListImmediate& CommandList) {
			FRHITextureCreateDesc TextureCreateDesc = FRHITextureCreateDesc::Create2D("ImGuiFontAtlas", FontWidth, FontHeight, EPixelFormat::RGBA8_UNORM);
			GBackendState.FontAtlasTexture = RHICreateTexture(TextureCreateDesc);
			FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, FontWidth, FontHeight);
			GDynamicRHI->RHIUpdateTexture2D(CommandList, GBackendState.FontAtlasTexture, 0, UpdateRegion, FontWidth * 4, FontPixels);
		});
	}

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
			FFileHelper::SaveArrayToFile(*VertexShaderCode, FShaderPaths::BinaryPath(ImGuiShaderName, CompileOptions.EntryPoints[0], 0));
			FFileHelper::SaveArrayToFile(*PixelShaderCode, FShaderPaths::BinaryPath(ImGuiShaderName, CompileOptions.EntryPoints[1], 0));
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

			FBindingLayoutDesc LayoutDesc;
			LayoutDesc.BindingLayouts = {
				FBindingLayoutItem{EShaderStageFlags::Fragment, 0, ERHIBindingType::Texture},
				FBindingLayoutItem{EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler}
			};
			Initializer.BindingLayouts = {LayoutDesc};

			Initializer.PushConstantRanges = {
				FRHIPushConstantRange{EShaderStageFlags::Vertex, 0, sizeof(FImGuiRHIImpl_ConstantBufferData)}
			};
			GDynamicRHI->RHICreateGraphicsPipelineState("ImGuiMainPipeline", Initializer);
		});
	}

	static auto ImGuiRHIImpl_CreateRHIResources()
	{
		ENQUEUE_RENDER_COMMAND(SwitchPipeline)([](FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		});
		ImGuiRHIImpl_CreateFontAtlasTexture();
		ImGuiRHIImpl_CreateMainPipeline();
	}

	static auto ImGuiRHIImpl_DestroyTexture(ImTextureData* Tex) -> void
	{
		if (Tex->BackendUserData)
		{
			FRHITexture* Texture = static_cast<FRHITexture*>(Tex->BackendUserData);
			// ReSharper disable once CppExpressionWithoutSideEffects
			Texture->Release();
			Tex->BackendUserData = nullptr;
			Tex->TexID = ImTextureID_Invalid;
			Tex->Status = ImTextureStatus_Destroyed;
		}
	}

	static auto ImGuiRHIImpl_DestroyRHIResources() -> void
	{
		for (ImTextureData* Tex : ImGui::GetPlatformIO().Textures)
		{
			ImGuiRHIImpl_DestroyTexture(Tex);
		}

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([](FRHICommandListImmediate& CommandList) {
			GBackendState.VertexShader = nullptr;
			GBackendState.PixelShader = nullptr;
			GBackendState.VertexDeclaration = nullptr;
			GBackendState.PipelineState = nullptr;
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

		ImGuiRHIImpl_CreateRHIResources();
	}

	auto ImGuiRHIImpl_Shutdown() -> void
	{
		ImGuiRHIImpl_DestroyRHIResources();

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

	static auto ImGuiRHIImplRT_UpdateBuffers(FRHICommandListImmediate& CommandList, const ImDrawData* DrawData, FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers) -> void
	{
		check(IsInRenderingThread());

		if (DrawData->TotalVtxCount <= 0) return;

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

	static auto ImGuiRHIImplRT_UpdateTexture(FRHICommandListImmediate& CommandList, ImTextureData* InTex) -> void
	{
		check(IsInRenderingThread());

		// Create if needed
		if (InTex->Status == ImTextureStatus_WantCreate)
		{
			check(InTex->TexID == ImTextureID_Invalid && InTex->BackendUserData == nullptr);
			check(InTex->Format == ImTextureFormat_RGBA32);

			FRHITextureCreateDesc TextureCreateDesc = FRHITextureCreateDesc::Create2D("ImGuiCreatedTexture", InTex->Width, InTex->Height, EPixelFormat::RGBA8_UNORM);
			FTextureRHIRef Texture = RHICreateTexture(TextureCreateDesc);
			// ReSharper disable once CppExpressionWithoutSideEffects
			Texture->AddRef();
			InTex->BackendUserData = Texture.GetReference();
		}

		if (InTex->Status == ImTextureStatus_WantCreate || InTex->Status == ImTextureStatus_WantUpdates)
		{
			FRHITexture* Texture = static_cast<FRHITexture*>(InTex->BackendUserData);
			const int UploadX = (InTex->Status == ImTextureStatus_WantCreate) ? 0 : InTex->UpdateRect.x;
			const int UploadY = (InTex->Status == ImTextureStatus_WantCreate) ? 0 : InTex->UpdateRect.y;
			const int UploadW = (InTex->Status == ImTextureStatus_WantCreate) ? InTex->Width : InTex->UpdateRect.w;
			const int UploadH = (InTex->Status == ImTextureStatus_WantCreate) ? InTex->Height : InTex->UpdateRect.h;

			FUpdateTextureRegion2D UpdateRegion(UploadX, UploadY, UploadX, UploadY, UploadW, UploadH);
			GDynamicRHI->RHIUpdateTexture2D(CommandList, Texture, 0, UpdateRegion, UploadW * 4, InTex->Pixels);

			InTex->SetStatus(ImTextureStatus_OK);
		}

		if (InTex->Status == ImTextureStatus_WantDestroy)
		{
			ImGuiRHIImpl_DestroyTexture(InTex);
		}
	}

	static auto ImGuiRHIImplRT_RenderDrawData(
		FRHICommandListImmediate& CommandList,
		FRHITexture* InTargetFrameBuffer,
		const ImDrawData* DrawData,
		FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers
	) -> void
	{
		check(IsInRenderingThread());
		// Update textures
		if (DrawData->Textures != nullptr)
		{
			for (ImTextureData* Tex : *DrawData->Textures)
			{
				if (Tex->Status != ImTextureStatus_OK)
				{
					ImGuiRHIImplRT_UpdateTexture(CommandList, Tex);
				}
			}
		}

		// Update vertex/index buffers
		ImGuiRHIImplRT_UpdateBuffers(CommandList, DrawData, RenderBuffers);

		// Render pass
		FRHIRenderPassInfo PassInfo{};
		PassInfo.ColorRenderTargets[0] = InTargetFrameBuffer;

		CommandList.BeginRenderPass(PassInfo, "ImGuiRenderPass");
		CommandList.SetGraphicsPipelineState(*GDynamicRHI->RHIGetGraphicsPipelineState("ImGuiMainPipeline"));

		if (RenderBuffers.IndexBuffer)
		{
			CommandList.BindVertexBuffer(0, RenderBuffers.VertexBuffer, 0);
			CommandList.BindIndexBuffer(RenderBuffers.IndexBuffer, 0);

			FImGuiRHIImpl_ConstantBufferData DataToPush;
			DataToPush.Scale.x = 2.0f / DrawData->DisplaySize.x;
			DataToPush.Scale.y = 2.0f / DrawData->DisplaySize.y;
			DataToPush.Translation.x = -1.0f - DrawData->DisplayPos.x * DataToPush.Scale.x;
			DataToPush.Translation.y = -1.0f - DrawData->DisplayPos.y * DataToPush.Scale.y;
			CommandList.PushConstants(EShaderStageFlags::Vertex, 0, sizeof(FImGuiRHIImpl_ConstantBufferData), &DataToPush);
			// CommandList.DrawIndexed(DrawData->TotalIdxCount, 0, 0);
		}

		CommandList.EndRenderPass();
	}

	auto ImGuiRHIImpl_RenderDrawData(const FViewportRHIRef& InViewport, ImDrawData* DrawData) -> void
	{
		const int32 FrameBufferWidth = static_cast<int32>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		const int32 FrameBufferHeight = static_cast<int32>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);
		if (FrameBufferWidth <= 0 || FrameBufferHeight <= 0)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(RenderWindow)([ViewportRHI = InViewport, DrawData](FRHICommandListImmediate& CommandList) {
			auto& RenderBuffersCurrentFrame = GBackendState.MainWindowRenderBuffers.FrameRenderBuffers[GFrameCounterRenderThread % kFrameInFlight];
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);

			CommandList.BeginDrawingViewport(ViewportRHI, nullptr);
			FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
			ImGuiRHIImplRT_RenderDrawData(CommandList, BackBuffer, DrawData, RenderBuffersCurrentFrame);
			CommandList.EndDrawingViewport(ViewportRHI, true, false);
		});
	}

} // namespace Doge::Mona::MonaImGuiBackend