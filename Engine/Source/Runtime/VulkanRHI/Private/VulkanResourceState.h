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

	// One backend writer tracks exclusive ownership in byte/subresource units.
	// A release removes access until the exact matching acquire is recorded.
	class FVulkanQueueOwnershipTracker final
	{
	public:
		struct FRange
		{
			uint64 Offset, Size;
			auto operator==(const FRange&) const -> bool = default;
		};
		VULKANRHI_API explicit FVulkanQueueOwnershipTracker(uint64 Size);
		VULKANRHI_API auto CanUse(uint64 Offset, uint64 Size, uint32 Family) const -> bool;
		VULKANRHI_API auto IsReleased(uint64 Offset, uint64 Size) const -> bool;
		VULKANRHI_API auto Claim(uint64 Offset, uint64 Size, uint32 Family) -> bool;
		VULKANRHI_API auto Release(uint64 Offset, uint64 Size, uint32 Source,
			uint32 Destination, uint64 TransferId) -> bool;
		VULKANRHI_API auto Acquire(uint64 Offset, uint64 Size, uint32 Source,
			uint32 Destination, uint64 TransferId) -> bool;
		VULKANRHI_API auto ReleaseRanges(std::span<const FRange> Ranges, uint32 Source,
			uint32 Destination, uint64 TransferId) -> bool;
		VULKANRHI_API auto AcquireRanges(std::span<const FRange> Ranges, uint32 Source,
			uint32 Destination, uint64 TransferId) -> bool;

	private:
		struct FInterval
		{
			uint64 Offset = 0;
			uint64 Size = 0;
			uint32 Family = VK_QUEUE_FAMILY_IGNORED;
			uint64 TransferId = 0;
		};
		struct FTransfer
		{
			std::vector<FRange> Ranges;
			uint32 Source, Destination;
		};
		auto InBounds(uint64 Offset, uint64 Size) const -> bool;
		auto Assign(uint64 Offset, uint64 Size, uint32 Family, uint64 TransferId) -> void;
		uint64 TotalSize;
		std::vector<FInterval> Intervals;
		std::unordered_map<uint64, FTransfer> Transfers;
	};

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
		auto GetOwnership() -> FVulkanQueueOwnershipTracker& { return Ownership; }

	private:
		std::vector<FInterval> Intervals;
		FVulkanQueueOwnershipTracker Ownership;
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
		auto ClaimOwnership(const FRHITextureSubresourceRange& Range, uint32 Family) -> bool;
		auto CanUseOwnership(const FRHITextureSubresourceRange& Range, uint32 Family) const -> bool;
		auto ReleaseOwnership(const FRHITextureSubresourceRange& Range, uint32 Source,
			uint32 Destination, uint64 TransferId) -> bool;
		auto AcquireOwnership(const FRHITextureSubresourceRange& Range, uint32 Source,
			uint32 Destination, uint64 TransferId) -> bool;

	private:
		auto GetIndex(ERHITextureAspect Aspect, uint32 Mip, uint32 Layer) const -> size_t;
		auto GetOwnershipRanges(const FRHITextureSubresourceRange& Range) const
			-> std::vector<FVulkanQueueOwnershipTracker::FRange>;

		uint32 MipCount = 0;
		uint32 LayerCount = 0;
		std::vector<ERHIAccess> States;
		FVulkanQueueOwnershipTracker Ownership;
	};

	VULKANRHI_API auto ValidateVulkanTextureDescriptorState(
		const FVulkanTextureStateTracker& Tracker,
		const FRHITextureSubresourceRange& Range,
		ERHIBindingType BindingType, ERHIAccess& OutTracked) -> bool;
} // namespace Durin::VulkanRHI
