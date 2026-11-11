#pragma once

#include "CoreMinimal.h"

#include "MonaAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class MWindow;
	class FViewportOutputReceipt;
}

namespace Durin::Mona
{
	// Defines the renderer services required by Mona application windows.
	class FMonaRenderer
	{
	public:
		virtual ~FMonaRenderer() = default;

		MONA_API virtual auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void = 0;

		MONA_API virtual auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void = 0;

		MONA_API virtual auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void = 0;

		// Read-only application-thread receipt check against the window's current viewport.
		// Never creates, resizes or changes presentation policy.
		virtual auto IsOutputPresented(const MWindow& Window, const FViewportOutputReceipt& Receipt, uint64 Generation) const -> bool = 0;
		// Read-only first-presentation check for automation without a scene (e.g. project browser).
		virtual auto IsWindowPresented(const MWindow& Window) const -> bool { return false; }

		// Consumes the latest resize request before returning the viewport used by
		// this draw. Callers must invoke this immediately before enqueueing work.
		MONA_API virtual auto PrepareViewportForDraw(const MWindow& Window) -> TRefCountPtr<FRHIViewport> = 0;

		// Application-thread override for all existing and future window viewports.
		auto SetPresentationPolicyOverride(EViewportPresentationPolicy Policy) -> void { PresentationPolicyOverride = Policy; }
		auto GetPresentationPolicyOverride() const -> std::optional<EViewportPresentationPolicy> { return PresentationPolicyOverride; }
		virtual auto GetViewportPresentMode(const MWindow& Window) const -> EViewportPresentMode { return EViewportPresentMode::Unavailable; }

	private:
		std::optional<EViewportPresentationPolicy> PresentationPolicyOverride;
	};
}
