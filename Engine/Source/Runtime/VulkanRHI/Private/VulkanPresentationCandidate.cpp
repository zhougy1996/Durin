#include "VulkanPresentationCandidate.h"

namespace Durin::VulkanRHI
{
	FVulkanPresentationCandidate::FVulkanPresentationCandidate(
		vk::Instance InInstance,
		FRHIPresentationTarget InTarget,
		vk::SurfaceKHR InSurface)
		: Instance(InInstance)
		, Target(InTarget)
		, Surface(InSurface)
	{
		require(Instance);
		require(Target.IsValid());
		require(Surface);
	}

	FVulkanPresentationCandidate::~FVulkanPresentationCandidate()
	{
		Reset();
	}

	FVulkanPresentationCandidate::FVulkanPresentationCandidate(
		FVulkanPresentationCandidate&& Other) noexcept
		: Instance(std::exchange(Other.Instance, nullptr))
		, Target(std::exchange(Other.Target, {}))
		, Surface(std::exchange(Other.Surface, nullptr))
		, State(std::exchange(Other.State, EState::Consumed))
	{
	}

	auto FVulkanPresentationCandidate::operator=(
		FVulkanPresentationCandidate&& Other) noexcept
		-> FVulkanPresentationCandidate&
	{
		if (this == &Other) return *this;
		Reset();
		Instance = std::exchange(Other.Instance, nullptr);
		Target = std::exchange(Other.Target, {});
		Surface = std::exchange(Other.Surface, nullptr);
		State = std::exchange(Other.State, EState::Consumed);
		return *this;
	}

	auto FVulkanPresentationCandidate::GetSurfaceForAdmission() const
		-> vk::SurfaceKHR
	{
		require(State == EState::Available);
		require(Surface);
		return Surface;
	}

	auto FVulkanPresentationCandidate::TakeForNativeWindow(
		void* NativeWindowHandle) -> vk::SurfaceKHR
	{
		if (State != EState::Available
			|| Target.NativeWindowHandle != NativeWindowHandle)
		{
			return VK_NULL_HANDLE;
		}
		State = EState::Consumed;
		Instance = nullptr;
		Target = {};
		return std::exchange(Surface, nullptr);
	}

	auto FVulkanPresentationCandidate::Reset() -> void
	{
		if (State == EState::Available && Surface)
		{
			require(Instance);
			Instance.destroySurfaceKHR(Surface);
		}
		Instance = nullptr;
		Target = {};
		Surface = nullptr;
		State = EState::Consumed;
	}
}
