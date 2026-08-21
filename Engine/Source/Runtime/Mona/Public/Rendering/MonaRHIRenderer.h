#pragma once

#include "MonaAPI.h"
#include "RHIResources.h"
#include "Rendering/MonaRenderer.h"

namespace Durin
{
	class MWindow;
}

namespace Durin::Mona
{
	// Tracks one window viewport and the latest resize request awaiting a draw.
	struct FMonaViewportInfo
	{
		// Keeps only the newest extent and ignores the size already submitted.
		auto QueueResize(const FIntPoint& RequestedExtent) -> void
		{
			if ((PendingExtent && *PendingExtent == RequestedExtent)
				|| (!PendingExtent && SubmittedExtent == RequestedExtent))
			{
				return;
			}
			PendingExtent = RequestedExtent;
		}

		// Transfers the pending request to the draw preparation boundary.
		auto TakePendingResize() -> std::optional<FIntPoint>
		{
			return std::exchange(PendingExtent, std::nullopt);
		}

		TRefCountPtr<FRHIViewport> ViewportRHI;
		FIntPoint SubmittedExtent = {};
		std::optional<FIntPoint> PendingExtent;
		bool bFullScreen = false;
	};


	// Owns and lazily resizes the RHI viewports backing Mona windows.
	class FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		MONA_API explicit FMonaRHIRenderer(
			bool bAdoptInitializationPresentationCandidate);
		MONA_API ~FMonaRHIRenderer() override;

		MONA_API auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void override;
		MONA_API auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void override;
		MONA_API auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void override;

		MONA_API auto PrepareViewportForDraw(const MWindow& Window) -> TRefCountPtr<FRHIViewport> override;

		// Window keys are non-owning; this renderer owns every mapped viewport record.
		std::unordered_map<const MWindow*, FMonaViewportInfo*> WindowToViewportInfoMap;

	private:
		bool bAdoptInitializationPresentationCandidate = false;
	};
}
