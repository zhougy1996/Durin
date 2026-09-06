#pragma once

#include "RHIResources.h"
#include "VulkanRHIAPI.h"
#include <vulkan/vulkan.hpp>

namespace Durin::VulkanRHI
{
	struct FVulkanResourceStateMapping
	{
		vk::PipelineStageFlags2 StageMask2{};
		vk::AccessFlags2 AccessMask2{};
		vk::PipelineStageFlags LegacyStageMask{};
		vk::AccessFlags LegacyAccessMask{};
		vk::ImageLayout Layout = vk::ImageLayout::eUndefined;
		bool bTextureCompatible = false;
	};

	VULKANRHI_API auto MapVulkanResourceState(ERHIAccess Access) -> FVulkanResourceStateMapping;
	VULKANRHI_API auto ToVulkanAspectFlags(ERHITextureAspect Aspects) -> vk::ImageAspectFlags;
	VULKANRHI_API auto GetVulkanDescriptorImageLayout(ERHIBindingType BindingType)
		-> vk::ImageLayout;

	class VULKANRHI_API FVulkanBufferStateTracker
	{
	public:
		struct FInterval
		{
			uint64 Offset = 0;
			uint64 Size = 0;
			ERHIAccess Access = ERHIAccess::None;

			auto operator==(const FInterval&) const -> bool = default;
		};

		explicit FVulkanBufferStateTracker(uint64 Size);

		auto Validate(uint64 Offset, uint64 Size, ERHIAccess Expected, ERHIAccess& OutTracked) const -> bool;
		// Combines synchronization scopes across every overlapping tracked interval.
		auto GetBarrierSource(uint64 Offset, uint64 Size) const -> FVulkanResourceStateMapping;
		auto Apply(uint64 Offset, uint64 Size, ERHIAccess Access) -> void;
		auto GetIntervals() const -> const std::vector<FInterval>& { return Intervals; }

	private:
		std::vector<FInterval> Intervals;
	};

	class VULKANRHI_API FVulkanTextureStateTracker
	{
	public:
		FVulkanTextureStateTracker(uint32 NumMips, uint32 NumLayers);

		auto Validate(const FRHITextureSubresourceRange& Range, ERHIAccess Expected,
			ERHIAccess& OutTracked) const -> bool;
		// Discard affects only the old layout; all selected access scopes survive.
		auto GetBarrierSource(const FRHITextureSubresourceRange& Range,
			bool bDiscardContents) const -> FVulkanResourceStateMapping;
		auto Apply(const FRHITextureSubresourceRange& Range, ERHIAccess Access) -> void;
		auto Get(ERHITextureAspect Aspect, uint32 Mip, uint32 Layer) const -> ERHIAccess;

	private:
		auto GetIndex(ERHITextureAspect Aspect, uint32 Mip, uint32 Layer) const -> size_t;

		uint32 MipCount = 0;
		uint32 LayerCount = 0;
		std::vector<ERHIAccess> States;
	};

	VULKANRHI_API auto ValidateVulkanTextureDescriptorState(
		const FVulkanTextureStateTracker& Tracker,
		const FRHITextureSubresourceRange& Range,
		ERHIBindingType BindingType, ERHIAccess& OutTracked) -> bool;
} // namespace Durin::VulkanRHI
