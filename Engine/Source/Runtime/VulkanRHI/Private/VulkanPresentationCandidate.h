#pragma once

#include "PCH.VulkanRHI.h"
#include "RHIInitialization.h"

namespace Durin::VulkanRHI
{
	// Owns the startup surface from admission until checked viewport transfer.
	class FVulkanPresentationCandidate
	{
	public:
		enum class EState : uint8
		{
			Available,
			Consumed,
		};

		FVulkanPresentationCandidate(
			vk::Instance InInstance,
			FRHIPresentationTarget InTarget,
			vk::SurfaceKHR InSurface);
		~FVulkanPresentationCandidate();

		FVulkanPresentationCandidate(
			const FVulkanPresentationCandidate&) = delete;
		auto operator=(const FVulkanPresentationCandidate&)
			-> FVulkanPresentationCandidate& = delete;

		FVulkanPresentationCandidate(
			FVulkanPresentationCandidate&& Other) noexcept;
		auto operator=(FVulkanPresentationCandidate&& Other) noexcept
			-> FVulkanPresentationCandidate&;

		auto GetSurfaceForAdmission() const -> vk::SurfaceKHR;
		auto GetState() const -> EState { return State; }

		// Transfers the surface once after validating the requested native window.
		auto TakeForNativeWindow(void* NativeWindowHandle) -> vk::SurfaceKHR;

	private:
		auto Reset() -> void;

		vk::Instance Instance;
		FRHIPresentationTarget Target;
		vk::SurfaceKHR Surface;
		EState State = EState::Available;
	};
}
