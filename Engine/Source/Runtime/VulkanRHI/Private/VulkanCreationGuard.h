#pragma once

#include "CoreMinimal.h"

namespace Durin::VulkanRHI
{
	// Releases an unpublished native candidate if construction fails.
	template<typename Cleanup>
	class TVulkanCreationGuard final
	{
	public:
		explicit TVulkanCreationGuard(Cleanup InCleanup) : Release(std::move(InCleanup)) {}
		~TVulkanCreationGuard() { if (bActive) Release(); }
		TVulkanCreationGuard(const TVulkanCreationGuard&) = delete;
		auto operator=(const TVulkanCreationGuard&) -> TVulkanCreationGuard& = delete;
		auto Commit() -> void { bActive = false; }
	private:
		Cleanup Release;
		bool bActive = true;
	};
}
