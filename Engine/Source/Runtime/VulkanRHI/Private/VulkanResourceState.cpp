#include "VulkanResourceState.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		constexpr ERHIAccess ReadMask = ERHIAccess::VertexBufferRead | ERHIAccess::IndexBufferRead
			| ERHIAccess::GraphicsUniformRead | ERHIAccess::ComputeUniformRead
			| ERHIAccess::GraphicsShaderRead | ERHIAccess::ComputeShaderRead
			| ERHIAccess::TransferRead | ERHIAccess::HostRead;

		auto MergeSource(FVulkanResourceStateMapping& Result, ERHIAccess Access) -> void
		{
			const auto Source = MapVulkanResourceState(Access);
			Result.StageMask2 |= Source.StageMask2;
			Result.AccessMask2 |= Source.AccessMask2;
			Result.LegacyStageMask |= Source.LegacyStageMask;
			Result.LegacyAccessMask |= Source.LegacyAccessMask;
		}

		auto ForEachAspect(ERHITextureAspect Aspects, const auto& Function) -> void
		{
			for (ERHITextureAspect Aspect : {ERHITextureAspect::Color, ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
			{
				if (EnumHasAnyFlags(Aspects, Aspect)) Function(Aspect);
			}
		}
	}

	auto MapVulkanResourceState(ERHIAccess Access) -> FVulkanResourceStateMapping
	{
		FVulkanResourceStateMapping Result;
		if (Access == ERHIAccess::None || Access == ERHIAccess::Discard)
		{
			Result.LegacyStageMask = vk::PipelineStageFlagBits::eTopOfPipe;
			Result.bTextureCompatible = true;
			return Result;
		}
		if (Access == ERHIAccess::Present)
		{
			Result.LegacyStageMask = vk::PipelineStageFlagBits::eBottomOfPipe;
			Result.Layout = vk::ImageLayout::ePresentSrcKHR;
			Result.bTextureCompatible = true;
			return Result;
		}

		auto Add = [&Result](vk::PipelineStageFlags2 Stage2, vk::AccessFlags2 Access2,
			vk::PipelineStageFlags LegacyStage, vk::AccessFlags LegacyAccess) {
			Result.StageMask2 |= Stage2;
			Result.AccessMask2 |= Access2;
			Result.LegacyStageMask |= LegacyStage;
			Result.LegacyAccessMask |= LegacyAccess;
		};
		if (EnumHasAnyFlags(Access, ERHIAccess::VertexBufferRead))
			Add(vk::PipelineStageFlagBits2::eVertexInput, vk::AccessFlagBits2::eVertexAttributeRead,
				vk::PipelineStageFlagBits::eVertexInput, vk::AccessFlagBits::eVertexAttributeRead);
		if (EnumHasAnyFlags(Access, ERHIAccess::IndexBufferRead))
			Add(vk::PipelineStageFlagBits2::eVertexInput, vk::AccessFlagBits2::eIndexRead,
				vk::PipelineStageFlagBits::eVertexInput, vk::AccessFlagBits::eIndexRead);
		constexpr vk::PipelineStageFlags2 GraphicsStages2 = vk::PipelineStageFlagBits2::eAllGraphics;
		constexpr vk::PipelineStageFlags GraphicsStages = vk::PipelineStageFlagBits::eAllGraphics;
		if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsUniformRead))
		{
			Result.StageMask2 |= GraphicsStages2;
			Result.AccessMask2 |= vk::AccessFlagBits2::eUniformRead;
			Result.LegacyStageMask |= GraphicsStages;
			Result.LegacyAccessMask |= vk::AccessFlagBits::eUniformRead;
		}
		if (EnumHasAnyFlags(Access, ERHIAccess::ComputeUniformRead))
			Add(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eUniformRead,
				vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eUniformRead);
		if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsShaderRead))
		{
			Result.StageMask2 |= GraphicsStages2;
			Result.AccessMask2 |= vk::AccessFlagBits2::eShaderRead;
			Result.LegacyStageMask |= GraphicsStages;
			Result.LegacyAccessMask |= vk::AccessFlagBits::eShaderRead;
		}
		if (EnumHasAnyFlags(Access, ERHIAccess::ComputeShaderRead))
			Add(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
				vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead);
		if (EnumHasAnyFlags(Access, ERHIAccess::TransferRead))
			Add(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
				vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferRead);
		if (EnumHasAnyFlags(Access, ERHIAccess::HostRead))
			Add(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead,
				vk::PipelineStageFlagBits::eHost, vk::AccessFlagBits::eHostRead);

		if (Access == ERHIAccess::ColorAttachmentReadWrite)
			Add(vk::PipelineStageFlagBits2::eColorAttachmentOutput,
				vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
				vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
		else if (Access == ERHIAccess::DepthStencilReadWrite)
		{
			Result.StageMask2 = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
			Result.AccessMask2 = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
			Result.LegacyStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
			Result.LegacyAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite;
		}
		else if (Access == ERHIAccess::GraphicsShaderReadWrite)
		{
			Result.StageMask2 = GraphicsStages2;
			Result.AccessMask2 = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
			Result.LegacyStageMask = GraphicsStages;
			Result.LegacyAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
		}
		else if (Access == ERHIAccess::ComputeShaderReadWrite)
			Add(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
				vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
		else if (Access == ERHIAccess::TransferWrite)
			Add(vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
				vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eTransferWrite);
		else if (Access == ERHIAccess::HostWrite)
			Add(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
				vk::PipelineStageFlagBits::eHost, vk::AccessFlagBits::eHostWrite);

		ERHITextureLayout PortableLayout;
		Result.bTextureCompatible = GetTextureLayoutForAccess(Access, PortableLayout);
		if (Result.bTextureCompatible)
		{
			switch (PortableLayout)
			{
			case ERHITextureLayout::Undefined: Result.Layout = vk::ImageLayout::eUndefined; break;
			case ERHITextureLayout::ColorAttachment: Result.Layout = vk::ImageLayout::eColorAttachmentOptimal; break;
			case ERHITextureLayout::DepthStencilAttachment: Result.Layout = vk::ImageLayout::eDepthStencilAttachmentOptimal; break;
			case ERHITextureLayout::ShaderReadOnly: Result.Layout = vk::ImageLayout::eShaderReadOnlyOptimal; break;
			case ERHITextureLayout::TransferSource: Result.Layout = vk::ImageLayout::eTransferSrcOptimal; break;
			case ERHITextureLayout::TransferDestination: Result.Layout = vk::ImageLayout::eTransferDstOptimal; break;
			case ERHITextureLayout::General: Result.Layout = vk::ImageLayout::eGeneral; break;
			case ERHITextureLayout::Present: Result.Layout = vk::ImageLayout::ePresentSrcKHR; break;
			}
		}
		check(Access == ERHIAccess::Discard || EnumHasAnyFlags(Access, ReadMask)
			|| Result.StageMask2 != vk::PipelineStageFlags2{});
		return Result;
	}

	auto ToVulkanAspectFlags(ERHITextureAspect Aspects) -> vk::ImageAspectFlags
	{
		vk::ImageAspectFlags Result;
		if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Color)) Result |= vk::ImageAspectFlagBits::eColor;
		if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Depth)) Result |= vk::ImageAspectFlagBits::eDepth;
		if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Stencil)) Result |= vk::ImageAspectFlagBits::eStencil;
		return Result;
	}

	auto GetVulkanDescriptorImageLayout(ERHIBindingType BindingType)
		-> vk::ImageLayout
	{
		check(BindingType == ERHIBindingType::Texture
			|| BindingType == ERHIBindingType::StorageImage);
		return BindingType == ERHIBindingType::Texture
			? vk::ImageLayout::eShaderReadOnlyOptimal
			: vk::ImageLayout::eGeneral;
	}

	FVulkanBufferStateTracker::FVulkanBufferStateTracker(uint64 Size)
		: Intervals{{0, Size, ERHIAccess::None}}
	{
		check(Size > 0);
	}

	auto FVulkanBufferStateTracker::Validate(uint64 Offset, uint64 Size,
		ERHIAccess Expected, ERHIAccess& OutTracked) const -> bool
	{
		bool bFound = false;
		for (const FInterval& Interval : Intervals)
		{
			if (Interval.Offset >= Offset + Size || Offset >= Interval.Offset + Interval.Size) continue;
			if (!bFound) { OutTracked = Interval.Access; bFound = true; }
			if (Expected != ERHIAccess::Discard && Interval.Access != Expected) return false;
		}
		return bFound;
	}

	auto FVulkanBufferStateTracker::GetBarrierSource(uint64 Offset, uint64 Size) const
		-> FVulkanResourceStateMapping
	{
		FVulkanResourceStateMapping Result;
		for (const auto& Interval : Intervals)
			if (Interval.Offset < Offset + Size && Offset < Interval.Offset + Interval.Size)
				MergeSource(Result, Interval.Access);
		return Result;
	}

	auto FVulkanBufferStateTracker::Apply(uint64 Offset, uint64 Size, ERHIAccess Access) -> void
	{
		std::vector<FInterval> Result;
		for (const FInterval& Interval : Intervals)
		{
			const uint64 End = Interval.Offset + Interval.Size;
			const uint64 UpdateEnd = Offset + Size;
			if (End <= Offset || Interval.Offset >= UpdateEnd) { Result.push_back(Interval); continue; }
			if (Interval.Offset < Offset) Result.push_back({Interval.Offset, Offset - Interval.Offset, Interval.Access});
			Result.push_back({std::max(Interval.Offset, Offset), std::min(End, UpdateEnd) - std::max(Interval.Offset, Offset), Access});
			if (End > UpdateEnd) Result.push_back({UpdateEnd, End - UpdateEnd, Interval.Access});
		}
		Intervals.clear();
		for (const FInterval& Interval : Result)
		{
			if (!Intervals.empty() && Intervals.back().Access == Interval.Access
				&& Intervals.back().Offset + Intervals.back().Size == Interval.Offset)
				Intervals.back().Size += Interval.Size;
			else Intervals.push_back(Interval);
		}
	}

	FVulkanTextureStateTracker::FVulkanTextureStateTracker(uint32 NumMips, uint32 NumLayers)
		: MipCount(NumMips), LayerCount(NumLayers), States(static_cast<size_t>(NumMips) * NumLayers * 3, ERHIAccess::None)
	{
		check(NumMips > 0 && NumLayers > 0);
	}

	auto FVulkanTextureStateTracker::GetIndex(ERHITextureAspect Aspect, uint32 Mip, uint32 Layer) const -> size_t
	{
		uint32 Plane = Aspect == ERHITextureAspect::Color ? 0 : Aspect == ERHITextureAspect::Depth ? 1 : 2;
		return (static_cast<size_t>(Plane) * LayerCount + Layer) * MipCount + Mip;
	}

	auto FVulkanTextureStateTracker::Get(ERHITextureAspect Aspect, uint32 Mip, uint32 Layer) const -> ERHIAccess
	{
		return States[GetIndex(Aspect, Mip, Layer)];
	}

	auto FVulkanTextureStateTracker::Validate(const FRHITextureSubresourceRange& Range,
		ERHIAccess Expected, ERHIAccess& OutTracked) const -> bool
	{
		bool bFound = false;
		bool bValid = true;
		ForEachAspect(Range.Aspects, [&](ERHITextureAspect Aspect) {
			for (uint32 Layer = Range.FirstArrayLayer; Layer < Range.FirstArrayLayer + Range.NumArrayLayers; ++Layer)
				for (uint32 Mip = Range.FirstMip; Mip < Range.FirstMip + Range.NumMips; ++Mip)
				{
					const ERHIAccess Tracked = Get(Aspect, Mip, Layer);
					if (!bFound) { OutTracked = Tracked; bFound = true; }
					if (Expected != ERHIAccess::Discard && Tracked != Expected) bValid = false;
				}
		});
		return bFound && bValid;
	}

	auto FVulkanTextureStateTracker::GetBarrierSource(
		const FRHITextureSubresourceRange& Range, bool bDiscardContents) const
		-> FVulkanResourceStateMapping
	{
		FVulkanResourceStateMapping Result;
		ForEachAspect(Range.Aspects, [&](ERHITextureAspect Aspect) {
			for (uint32 Layer = Range.FirstArrayLayer; Layer < Range.FirstArrayLayer + Range.NumArrayLayers; ++Layer)
				for (uint32 Mip = Range.FirstMip; Mip < Range.FirstMip + Range.NumMips; ++Mip)
				{
					const ERHIAccess Access = Get(Aspect, Mip, Layer);
					MergeSource(Result, Access);
					if (!bDiscardContents) Result.Layout = MapVulkanResourceState(Access).Layout;
				}
		});
		return Result;
	}

	auto FVulkanTextureStateTracker::Apply(const FRHITextureSubresourceRange& Range, ERHIAccess Access) -> void
	{
		ForEachAspect(Range.Aspects, [&](ERHITextureAspect Aspect) {
			for (uint32 Layer = Range.FirstArrayLayer; Layer < Range.FirstArrayLayer + Range.NumArrayLayers; ++Layer)
				for (uint32 Mip = Range.FirstMip; Mip < Range.FirstMip + Range.NumMips; ++Mip)
					States[GetIndex(Aspect, Mip, Layer)] = Access;
		});
	}

	auto ValidateVulkanTextureDescriptorState(
		const FVulkanTextureStateTracker& Tracker,
		const FRHITextureSubresourceRange& Range,
		ERHIBindingType BindingType, ERHIAccess& OutTracked) -> bool
	{
		if (BindingType == ERHIBindingType::Texture)
			return Tracker.Validate(Range, ERHIAccess::GraphicsShaderRead, OutTracked);
		if (BindingType == ERHIBindingType::StorageImage)
			return Tracker.Validate(Range, ERHIAccess::GraphicsShaderReadWrite, OutTracked);
		return false;
	}
} // namespace Durin::VulkanRHI
