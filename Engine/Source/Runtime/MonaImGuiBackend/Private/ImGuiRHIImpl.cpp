#include "ImGuiRHIImpl.h"

#include "RHI.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::Mona
{
	namespace
	{
		class FImGuiFragmentShader : public FShader
		{
		public:
			using FShader::FShader;

			struct FParameters
			{
				FRHITexture* fontTexture = nullptr;
				FRHISampler* fontSampler = nullptr;
			};

			static auto GetParametersMetadata() -> std::span<const FShaderParameterMetadata>
			{
				static const std::array Parameters = {
					DURIN_SHADER_PARAMETER(fontTexture, ERHIBindingType::Texture),
					DURIN_SHADER_PARAMETER(fontSampler, ERHIBindingType::Sampler)
				};
				return Parameters;
			}
		};

		auto CreateImGuiFragmentShader(
			const FShaderType* ShaderType,
			FShaderMapBase* ShaderMap,
			uint32 ShaderIndex,
			const FShaderReflectionData& Reflection
		) -> std::unique_ptr<FShader>
		{
			return std::make_unique<FImGuiFragmentShader>(ShaderType, ShaderMap, ShaderIndex, Reflection);
		}

		auto GetImGuiVertexShaderType() -> FShaderType&
		{
			static FShaderType ShaderType("ImGuiVertexShader", "/Engine/ImGui", EShaderFrequency::Vertex, "vertexMain");
			return ShaderType;
		}

		auto GetImGuiFragmentShaderType() -> FShaderType&
		{
			static FShaderType ShaderType(
				"ImGuiFragmentShader",
				"/Engine/ImGui",
				EShaderFrequency::Fragment,
				"fragmentMain",
				{},
				&CreateImGuiFragmentShader,
				nullptr,
				nullptr,
				FImGuiFragmentShader::GetParametersMetadata()
			);
			return ShaderType;
		}
	}

	// State of the ImGui RHI backend, stored in a struct to ensure proper initialization order of static variables.
	struct FImGuiRHIImplRT_BackendState
	{
		std::shared_ptr<FShaderMapBase> ShaderMap;
		TShaderRef<FShader> VertexShader;
		TShaderRef<FImGuiFragmentShader> FragmentShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateRHIRef PipelineState;

		FSamplerRHIRef LinearSampler;
	};

	static FImGuiRHIImplRT_BackendState GBackendState;

	struct FImGuiRHIImpl_ConstantBufferData
	{
		FVector2f Scale;
		FVector2f Translation;
	};

	auto FImGuiRHIImpl_WindowRenderBuffers::Clear() -> void
	{
		for (FImGuiRHIImpl_FrameRenderBuffers& Buffers : FrameRenderBuffers)
		{
			Buffers.VertexBuffer = nullptr;
			Buffers.IndexBuffer = nullptr;
			Buffers.ProjectionUniform = nullptr;
		}
	}

	static auto ImGuiRHIImpl_CreateFontAtlasTexture() -> void
	{
		ENQUEUE_RENDER_COMMAND(CreateImGuiFontAtlas)([](FRHICommandListImmediate& CommandList) {
			FRHISamplerDesc SamplerCreateDesc = FRHISamplerDesc::LinearClamp();
			GBackendState.LinearSampler = RHICreateSampler(SamplerCreateDesc);
		});
	}

	static auto ImGuiRHIImpl_CreateMainPipeline()
	{
		FShaderCompileOptions CompileOptions;
		FShaderType& VertexShaderType = GetImGuiVertexShaderType();
		FShaderType& FragmentShaderType = GetImGuiFragmentShaderType();
		std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
		std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
		std::string ErrorMessage;
		if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
		{
			DURIN_ERROR("Failed to initialize ImGui shader map: {}", ErrorMessage);
			return;
		}

		FShader* VertexShader = ShaderMap->GetShader(&VertexShaderType);
		auto* FragmentShader = static_cast<FImGuiFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
		check(VertexShader);
		check(FragmentShader);

		GBackendState.ShaderMap = ShaderMap;
		GBackendState.VertexShader = TShaderRef<FShader>(VertexShader, ShaderMap.get());
		GBackendState.FragmentShader = TShaderRef<FImGuiFragmentShader>(FragmentShader, ShaderMap.get());

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([
			ShaderMap,
			VertexShaderRef = GBackendState.VertexShader,
			FragmentShaderRef = GBackendState.FragmentShader
		](FRHICommandListImmediate& CommandList) {
			FVertexDeclarationElementList VertexDeclElements;
			constexpr uint32 VertexStride = sizeof(ImDrawVert);
			VertexDeclElements[0] = FVertexElement(0, offsetof(ImDrawVert, pos), EVertexElementType::Float2, 0, VertexStride);
			VertexDeclElements[1] = FVertexElement(0, offsetof(ImDrawVert, uv), EVertexElementType::Float2, 1, VertexStride);
			VertexDeclElements[2] = FVertexElement(0, offsetof(ImDrawVert, col), EVertexElementType::UByte4N, 2, VertexStride);
			GBackendState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderPassName = "ImGuiRenderPass";
			Initializer.BoundShaders.VertexShader = VertexShaderRef.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = FragmentShaderRef.GetRHIShader();
			Initializer.VertexDeclaration = GBackendState.VertexDeclaration;

			Initializer.PixelFormat = EPixelFormat::SRGBA8_UNORM;
			Initializer.bEnableAlphaBlend = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GBackendState.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("ImGuiMainPipeline", Initializer);
		});
	}

	static auto ImGuiRHIImpl_CreateRHIResources()
	{
		ENQUEUE_RENDER_COMMAND(SwitchPipeline)([](FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		});
		ImGuiRHIImpl_CreateMainPipeline();
		ImGuiRHIImpl_CreateFontAtlasTexture();
	}

	static auto ImGuiRHIImpl_DestroyTexture(ImTextureData* InTex) -> void
	{
		if (InTex->BackendUserData)
		{
			const auto* TextureRefPtr = static_cast<FTextureRHIRef*>(InTex->BackendUserData);
			delete TextureRefPtr;
			InTex->BackendUserData = nullptr;
			InTex->TexID = ImTextureID_Invalid;
			InTex->Status = ImTextureStatus_Destroyed;
		}
	}

	static auto ImGuiRHIImpl_DestroyRHIResources() -> void
	{
		ImGui::GetIO().Fonts->SetTexID(ImTextureID_Invalid);

		for (ImTextureData* Tex : ImGui::GetPlatformIO().Textures)
		{
			ImGuiRHIImpl_DestroyTexture(Tex);
		}

		ENQUEUE_RENDER_COMMAND(CreateImGuiMainPipeline)([](FRHICommandListImmediate& CommandList) {
			GBackendState.ShaderMap.reset();
			GBackendState.VertexShader = {};
			GBackendState.FragmentShader = {};
			GBackendState.VertexDeclaration = nullptr;
			GBackendState.PipelineState = nullptr;
			GBackendState.LinearSampler = nullptr;
		});
	}

	auto ImGuiRHIImpl_Init() -> void
	{
		check(GDynamicRHI);
		ImGuiIO& IO = ImGui::GetIO();
		IMGUI_CHECKVERSION();

		IO.BackendRendererUserData = GDynamicRHI;
		IO.BackendRendererName = "DurinRHI";
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
		IO.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.ClearRendererHandlers();
	}

	auto ImGuiRHIImpl_NewFrame() -> void
	{
	}

	static auto ImGuiRHIImplRT_UpdateBuffers(FRHICommandListImmediate& CommandList, const ImDrawData* DrawData, FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers) -> void
	{
		check(IsInRenderingThread());

		if (DrawData->TotalVtxCount <= 0) return;

		uint32 RequiredVertexBufferSize = DrawData->TotalVtxCount * sizeof(ImDrawVert);
		uint32 RequiredIndexBufferSize = DrawData->TotalIdxCount * sizeof(ImDrawIdx);

		if (RenderBuffers.VertexBuffer == nullptr || RenderBuffers.VertexBuffer->GetSize() < RequiredVertexBufferSize)
		{
			FRHIBufferCreateDesc VertexBufferCreateDesc = FRHIBufferCreateDesc::CreateVertex("ImGuiVertexBuffer", RequiredVertexBufferSize);
			VertexBufferCreateDesc.Usage |= EBufferUsageFlags::Dynamic;
			RenderBuffers.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexBufferCreateDesc);
		}
		if (RenderBuffers.IndexBuffer == nullptr || RenderBuffers.IndexBuffer->GetSize() < RequiredIndexBufferSize)
		{
			FRHIBufferCreateDesc IndexBufferCreateDesc = FRHIBufferCreateDesc::CreateIndex("ImGuiIndexBuffer", RequiredIndexBufferSize, sizeof(ImDrawIdx));
			IndexBufferCreateDesc.Usage |= EBufferUsageFlags::Dynamic;
			RenderBuffers.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexBufferCreateDesc);
		}

		void* VertexBufferData = GDynamicRHI->RHILockBuffer(CommandList, RenderBuffers.VertexBuffer, 0, RequiredVertexBufferSize, EResourceLockMode::WriteOnly);
		void* IndexBufferData = GDynamicRHI->RHILockBuffer(CommandList, RenderBuffers.IndexBuffer, 0, RequiredIndexBufferSize, EResourceLockMode::WriteOnly);

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

	static auto ImGuiRHIImpl_UpdateTexture(ImTextureData* InTex) -> void
	{
		if (InTex->Status == ImTextureStatus_WantCreate)
		{
			check(InTex->TexID == ImTextureID_Invalid && InTex->BackendUserData == nullptr);
			check(InTex->Format == ImTextureFormat_RGBA32 || InTex->Format == ImTextureFormat_Alpha8);

			auto* TextureRefPtr = new FTextureRHIRef();
			const EPixelFormat TextureFormat = (InTex->Format == ImTextureFormat_Alpha8) ? EPixelFormat::R8_UNORM : EPixelFormat::RGBA8_UNORM;
			ENQUEUE_RENDER_COMMAND(ImGuiImpl_CreateTexture)([TextureRefPtr, Width = InTex->Width, Height = InTex->Height, TextureFormat](FRHICommandListImmediate& CommandList) {
				FRHITextureCreateDesc TextureCreateDesc = FRHITextureCreateDesc::Create2D("ImGuiCreatedTexture", Width, Height, TextureFormat);
				*TextureRefPtr = RHICreateTexture(TextureCreateDesc);
			});
			// The created texture will be uploaded in the next step, so the rendering commands will be flushed to ensure the texture is ready before it's used for rendering.
			InTex->SetTexID(reinterpret_cast<ImTextureID>(TextureRefPtr));
			InTex->BackendUserData = TextureRefPtr;
		}

		if (InTex->Status == ImTextureStatus_WantCreate)
		{
			auto* TextureRefPtr = static_cast<FTextureRHIRef*>(InTex->BackendUserData);
			FUpdateTextureRegion2D UpdateRegion(0, 0, 0, 0, InTex->Width, InTex->Height);
			const uint32 SourcePitch = static_cast<uint32>(InTex->GetPitch());
			const uint8* TexPixels = InTex->Pixels;

			ENQUEUE_RENDER_COMMAND(ImGuiImpl_UpdateTexture)([=](FRHICommandListImmediate& CommandList) {
				GDynamicRHI->RHIUpdateTexture2D(CommandList, *TextureRefPtr, 0, UpdateRegion, SourcePitch, TexPixels);
			});

			FlushRenderingCommands(); // Make sure the texture update is processed before the texture is used for rendering in the next frame.
			InTex->SetStatus(ImTextureStatus_OK);
		}

		if (InTex->Status == ImTextureStatus_WantUpdates)
		{
			auto* TextureRefPtr = static_cast<FTextureRHIRef*>(InTex->BackendUserData);
			const uint32 SourcePitch = static_cast<uint32>(InTex->GetPitch());
			const uint32 BytesPerPixel = static_cast<uint32>(InTex->BytesPerPixel);
			for (const ImTextureRect& UpdateRect : InTex->Updates)
			{
				if (UpdateRect.w <= 0 || UpdateRect.h <= 0)
				{
					continue;
				}

				FUpdateTextureRegion2D UpdateRegion(UpdateRect.x, UpdateRect.y, UpdateRect.x, UpdateRect.y, UpdateRect.w, UpdateRect.h);
				const uint8* UpdatePixels = InTex->Pixels + static_cast<size_t>(UpdateRect.y) * SourcePitch + static_cast<size_t>(UpdateRect.x) * BytesPerPixel;
				ENQUEUE_RENDER_COMMAND(ImGuiImpl_UpdateTextureRegion)([=](FRHICommandListImmediate& CommandList) {
					GDynamicRHI->RHIUpdateTexture2D(CommandList, *TextureRefPtr, 0, UpdateRegion, SourcePitch, UpdatePixels);
				});
			}

			FlushRenderingCommands(); // Ensure atlas updates are visible before rendering new glyphs.
			InTex->SetStatus(ImTextureStatus_OK);
		}

		if (InTex->Status == ImTextureStatus_WantDestroy)
		{
			ImGuiRHIImpl_DestroyTexture(InTex);
		}
	}

	static auto ImGuiRHIImplRT_SetupRenderState(
		FRHICommandListImmediate& CommandList,
		const ImDrawData* DrawData,
		FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers
	) -> void
	{
		const int32 FrameBufferWidth = static_cast<int32>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		const int32 FrameBufferHeight = static_cast<int32>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);

		CommandList.SetGraphicsPipelineState(*GBackendState.PipelineState);
		CommandList.SetViewport(0.0f, 0.0f, 0.0f, FrameBufferWidth, FrameBufferHeight, 1.0f);
		CommandList.BindVertexBuffer(0, RenderBuffers.VertexBuffer, 0);
		CommandList.BindIndexBuffer(RenderBuffers.IndexBuffer, 0);

		FImGuiRHIImpl_ConstantBufferData DataToPush;
		DataToPush.Scale.x = 2.0f / DrawData->DisplaySize.x;
		DataToPush.Scale.y = 2.0f / DrawData->DisplaySize.y;
		DataToPush.Translation.x = -1.0f - DrawData->DisplayPos.x * DataToPush.Scale.x;
		DataToPush.Translation.y = -1.0f - DrawData->DisplayPos.y * DataToPush.Scale.y;
		CommandList.PushConstants(EShaderStageFlags::Vertex, 0, sizeof(FImGuiRHIImpl_ConstantBufferData), &DataToPush);

		// Reset to a full-frame scissor so the next draw command starts from known state.
		CommandList.SetScissor(0.0f, 0.0f, FrameBufferWidth, FrameBufferHeight);
	}

	static auto ImGuiRHIImplRT_RenderDrawData(
		FRHICommandListImmediate& CommandList,
		FRHITexture* InTargetFrameBuffer,
		const ImDrawData* DrawData,
		FImGuiRHIImpl_FrameRenderBuffers& RenderBuffers,
		const FClearValueBinding& ClearValue
	) -> void
	{
		check(IsInRenderingThread());
		const ImVec2 ClipOff = DrawData->DisplayPos;
		const ImVec2 ClipScale = DrawData->FramebufferScale;

		int FrameBufferWidth = static_cast<int>(DrawData->DisplaySize.x * DrawData->FramebufferScale.x);
		int FrameBufferHeight = static_cast<int>(DrawData->DisplaySize.y * DrawData->FramebufferScale.y);
		if (FrameBufferWidth <= 0 || FrameBufferHeight <= 0)
			return;

		// Update vertex/index buffers
		ImGuiRHIImplRT_UpdateBuffers(CommandList, DrawData, RenderBuffers);

		// Render pass
		FRHIRenderPassInfo PassInfo{};
		PassInfo.ColorRenderTargets[0] = InTargetFrameBuffer;
		PassInfo.ColorClearValue = ClearValue;

		CommandList.BeginRenderPass(PassInfo, "ImGuiRenderPass");
		ImGuiRHIImplRT_SetupRenderState(CommandList, DrawData, RenderBuffers);

		int GlobalVertexOffset = 0;
		int GlobalIndexOffset = 0;

		for (const ImDrawList* DrawList : DrawData->CmdLists)
		{
			for (int CmdIndex = 0; CmdIndex < DrawList->CmdBuffer.Size; CmdIndex++)
			{
				const ImDrawCmd* Cmd = &DrawList->CmdBuffer[CmdIndex];
				if (Cmd->UserCallback != nullptr)
				{
					// User callback, registered via ImDrawList::AddCallback()
					// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
					if (Cmd->UserCallback == ImDrawCallback_ResetRenderState)
					{
						ImGuiRHIImplRT_SetupRenderState(CommandList, DrawData, RenderBuffers);
					}
					else
					{
						Cmd->UserCallback(DrawList, Cmd);
					}
				}
				else
				{
					ImGuiRHIImplRT_SetupRenderState(CommandList, DrawData, RenderBuffers);

					const ImVec2 ClipMin((Cmd->ClipRect.x - ClipOff.x) * ClipScale.x, (Cmd->ClipRect.y - ClipOff.y) * ClipScale.y);
					const ImVec2 ClipMax((Cmd->ClipRect.z - ClipOff.x) * ClipScale.x, (Cmd->ClipRect.w - ClipOff.y) * ClipScale.y);

					const float ScissorMinX = std::max(0.0f, ClipMin.x);
					const float ScissorMinY = std::max(0.0f, ClipMin.y);
					const float ScissorMaxX = std::min(static_cast<float>(FrameBufferWidth), ClipMax.x);
					const float ScissorMaxY = std::min(static_cast<float>(FrameBufferHeight), ClipMax.y);
					if (ScissorMaxX <= ScissorMinX || ScissorMaxY <= ScissorMinY)
					{
						continue;
					}
					CommandList.SetScissor(ScissorMinX, ScissorMinY, ScissorMaxX - ScissorMinX, ScissorMaxY - ScissorMinY);

					auto* TextureRefPtr = reinterpret_cast<FTextureRHIRef*>(Cmd->GetTexID());

					FImGuiFragmentShader::FParameters ShaderParameters;
					ShaderParameters.fontTexture = *TextureRefPtr;
					ShaderParameters.fontSampler = GBackendState.LinearSampler;
					SetShaderParameters(CommandList, GBackendState.FragmentShader, ShaderParameters);

					CommandList.DrawIndexed(Cmd->ElemCount, Cmd->IdxOffset + GlobalIndexOffset, Cmd->VtxOffset + GlobalVertexOffset);
				}
			}
			GlobalIndexOffset += DrawList->IdxBuffer.Size;
			GlobalVertexOffset += DrawList->VtxBuffer.Size;
		}

		CommandList.EndRenderPass();
	}

	static auto ImGuiRHIImplRT_ClearViewport(
		FRHICommandListImmediate& CommandList,
		FRHITexture* InTargetFrameBuffer,
		const FClearValueBinding& ClearValue
	) -> void
	{
		FRHIRenderPassInfo PassInfo{};
		PassInfo.ColorRenderTargets[0] = InTargetFrameBuffer;
		PassInfo.ColorClearValue = ClearValue;
		CommandList.BeginRenderPass(PassInfo, "ImGuiRenderPass");
		CommandList.EndRenderPass();
	}

	auto ImGuiRHIImpl_RenderDrawData(const FViewportRHIRef& InViewport, ImDrawData* DrawData, FImGuiRHIImpl_WindowRenderBuffers* WindowRenderBuffers) -> void
	{
		const bool bHasDrawData = DrawData != nullptr;

		// Update textures
		if (bHasDrawData && DrawData->Textures != nullptr)
		{
			for (ImTextureData* Tex : *DrawData->Textures)
			{
				if (Tex->Status != ImTextureStatus_OK)
				{
					ImGuiRHIImpl_UpdateTexture(Tex);
				}
			}
		}

		const ImVec4 WindowBgColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		const FClearValueBinding ClearValue(WindowBgColor.x, WindowBgColor.y, WindowBgColor.z, 1.0f);

		ENQUEUE_RENDER_COMMAND(RenderWindow)([ViewportRHI = InViewport, DrawData, WindowRenderBuffers, ClearValue](FRHICommandListImmediate& CommandList) {
			check(WindowRenderBuffers != nullptr);
			auto& RenderBuffersCurrentFrame = WindowRenderBuffers->FrameRenderBuffers[GFrameCounterRenderThread % kFrameInFlight];
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);

			CommandList.BeginDrawingViewport(ViewportRHI, nullptr);

			FTextureRHIRef BackBuffer = GDynamicRHI->RHIGetViewportBackBuffer(ViewportRHI);
			if (BackBuffer == nullptr)
			{
				CommandList.EndDrawingViewport(ViewportRHI, false, false);
				return;
			}

			if (DrawData != nullptr
				&& DrawData->TotalVtxCount > 0
				&& DrawData->TotalIdxCount > 0
				&& GBackendState.PipelineState
				&& GBackendState.VertexShader
				&& GBackendState.FragmentShader)
			{
				ImGuiRHIImplRT_RenderDrawData(CommandList, BackBuffer, DrawData, RenderBuffersCurrentFrame, ClearValue);
			}
			else
			{
				ImGuiRHIImplRT_ClearViewport(CommandList, BackBuffer, ClearValue);
			}

			CommandList.EndDrawingViewport(ViewportRHI, true, false);
		});
	}

} // namespace Durin::Mona::MonaImGuiBackend
