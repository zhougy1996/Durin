#pragma once

namespace Durin::VulkanRHI
{
	// Lazily owns the hidden platform window required for hardware RHI admission
	// and keeps it registered with the active test application for the process.
	auto GetVulkanTestPresentationWindowHandle() -> void*;
}
