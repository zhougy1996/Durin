#pragma once

#include "CoreMinimal.h"
#include "MonaImGuiAPI.h"

namespace Durin { class MWindow; }

namespace Durin::MonaImGui
{
	// Removes only its own viewport policy generation; may outlive backend shutdown.
	class FViewportClosePolicyRegistration
	{
	public:
		FViewportClosePolicyRegistration() = default;
		MONAIMGUI_API ~FViewportClosePolicyRegistration();
		FViewportClosePolicyRegistration(const FViewportClosePolicyRegistration&) = delete;
		auto operator=(const FViewportClosePolicyRegistration&) -> FViewportClosePolicyRegistration& = delete;
		MONAIMGUI_API FViewportClosePolicyRegistration(FViewportClosePolicyRegistration&& Other) noexcept;
		MONAIMGUI_API auto operator=(FViewportClosePolicyRegistration&& Other) noexcept -> FViewportClosePolicyRegistration&;
		MONAIMGUI_API auto Reset() -> void;
		explicit operator bool() const { return static_cast<bool>(Release); }
	private:
		friend MONAIMGUI_API auto RegisterViewportClosePolicy(uint32, std::function<bool()>) -> FViewportClosePolicyRegistration;
		std::function<void()> Release;
	};

	// Game thread only. Registration may precede native viewport creation. Duplicate ownership is rejected.
	MONAIMGUI_API auto RegisterViewportClosePolicy(uint32 ViewportId,
		std::function<bool()> RequestClose) -> FViewportClosePolicyRegistration;
	// Executes the owner's close request. Only true authorizes PlatformRequestClose.
	MONAIMGUI_API auto TryCloseViewport(uint32 ViewportId) -> bool;
	MONAIMGUI_API auto GetWindowForViewport(uint32 ViewportId) -> std::shared_ptr<MWindow>;
}
