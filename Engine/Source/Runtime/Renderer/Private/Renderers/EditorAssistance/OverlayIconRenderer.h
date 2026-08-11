#pragma once

#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"

namespace Durin
{
	class FRendererResourceCoordinator;

	// Defines the tightly packed horizontal atlas shared by every editor overlay icon.
	struct FOverlayIconAtlasLayout
	{
		static constexpr uint32 IconExtent = 64;
		static constexpr uint32 IconCount =
			static_cast<uint32>(EViewOverlayIcon::PlayerStart) + 1;
		static constexpr uint32 Width = IconExtent * IconCount;
		static constexpr uint32 Height = IconExtent;
		static constexpr uint32 BytesPerPixel = 4;
		static constexpr uint32 RowPitchBytes = Width * BytesPerPixel;
		static constexpr size_t PixelByteCount =
			static_cast<size_t>(RowPitchBytes) * Height;

		static constexpr auto GetTileX(EViewOverlayIcon Icon) -> uint32
		{
			return static_cast<uint32>(Icon) * IconExtent;
		}

		static constexpr auto GetMinU(EViewOverlayIcon Icon) -> float
		{
			return static_cast<float>(GetTileX(Icon)) / Width;
		}
	};

	class FOverlayIconRenderer final
	{
	public:
		explicit FOverlayIconRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FOverlayIconRenderer();

		FOverlayIconRenderer(const FOverlayIconRenderer&) = delete;
		auto operator=(const FOverlayIconRenderer&)
			-> FOverlayIconRenderer& = delete;

		auto Prepare_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output,
			RendererEditorAssistance::FPrepared& Prepared) -> void;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const RendererEditorAssistance::FPrepared& Prepared,
			RendererEditorAssistance::EDepthMode DepthMode) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;

		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
