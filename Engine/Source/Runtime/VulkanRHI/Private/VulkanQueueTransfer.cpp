#include "VulkanQueueTransfer.h"
#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanCompletion.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	struct FVulkanQueueTransfer::FState
	{
		FVulkanDevice& Device;
		FRHIQueueId Source, Destination;
		bool bSync2;
		enum class EPhase { Prepared, Released, Acquired } Phase = EPhase::Prepared;
		FRHIGPUSubmissionTicket Signal;
		std::vector<FRHIBufferTransition> Buffers;
		std::vector<FRHITextureTransition> Textures;
		std::vector<FBufferRHIRef> BufferOwners;
		std::vector<FTextureRHIRef> TextureOwners;
		std::vector<vk::BufferMemoryBarrier2> BufferBarriers;
		std::vector<vk::ImageMemoryBarrier2> TextureBarriers;
		std::vector<FVulkanResourceStateMapping> BufferSources, BufferDestinations;
		std::vector<FVulkanResourceStateMapping> TextureSources, TextureDestinations;
		std::vector<uint64> BufferIds, TextureIds;

		auto Emit(vk::CommandBuffer Commands, bool bRelease) const -> void
		{
			auto Buffers = BufferBarriers;
			auto Textures = TextureBarriers;
			for (auto& Barrier : Buffers)
			{
				if (bRelease) Barrier.setDstStageMask({}).setDstAccessMask({});
				else Barrier.setSrcStageMask({}).setSrcAccessMask({});
			}
			for (auto& Barrier : Textures)
			{
				if (bRelease) Barrier.setDstStageMask({}).setDstAccessMask({});
				else
				{
					Barrier.setSrcStageMask({}).setSrcAccessMask({});
					// Shared-family layout transition happens once, on the producer.
					if (Barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) Barrier.oldLayout = Barrier.newLayout;
				}
			}
			if (bSync2)
			{
				const auto Info = vk::DependencyInfo().setBufferMemoryBarriers(Buffers).setImageMemoryBarriers(Textures);
				if (Device.GetGpuProperties().apiVersion >= VK_API_VERSION_1_3) Commands.pipelineBarrier2(Info);
				else Commands.pipelineBarrier2KHR(Info);
				return;
			}
			std::vector<vk::BufferMemoryBarrier> LegacyBuffers;
			std::vector<vk::ImageMemoryBarrier> LegacyTextures;
			vk::PipelineStageFlags SourceStages = bRelease ? vk::PipelineStageFlags{} : vk::PipelineStageFlagBits::eTopOfPipe;
			vk::PipelineStageFlags DestinationStages = bRelease ? vk::PipelineStageFlagBits::eBottomOfPipe : vk::PipelineStageFlags{};
			for (size_t Index = 0; Index < Buffers.size(); ++Index)
			{
				const auto& Barrier = Buffers[Index];
				if (bRelease) SourceStages |= BufferSources[Index].LegacyStageMask;
				else DestinationStages |= BufferDestinations[Index].LegacyStageMask;
				LegacyBuffers.push_back(vk::BufferMemoryBarrier()
					.setSrcAccessMask(bRelease ? BufferSources[Index].LegacyAccessMask : vk::AccessFlags{})
					.setDstAccessMask(bRelease ? vk::AccessFlags{} : BufferDestinations[Index].LegacyAccessMask)
					.setSrcQueueFamilyIndex(Barrier.srcQueueFamilyIndex).setDstQueueFamilyIndex(Barrier.dstQueueFamilyIndex)
					.setBuffer(Barrier.buffer).setOffset(Barrier.offset).setSize(Barrier.size));
			}
			for (size_t Index = 0; Index < Textures.size(); ++Index)
			{
				const auto& Barrier = Textures[Index];
				if (bRelease) SourceStages |= TextureSources[Index].LegacyStageMask;
				else DestinationStages |= TextureDestinations[Index].LegacyStageMask;
				LegacyTextures.push_back(vk::ImageMemoryBarrier()
					.setSrcAccessMask(bRelease ? TextureSources[Index].LegacyAccessMask : vk::AccessFlags{})
					.setDstAccessMask(bRelease ? vk::AccessFlags{} : TextureDestinations[Index].LegacyAccessMask)
					.setOldLayout(Barrier.oldLayout).setNewLayout(Barrier.newLayout)
					.setSrcQueueFamilyIndex(Barrier.srcQueueFamilyIndex).setDstQueueFamilyIndex(Barrier.dstQueueFamilyIndex)
					.setImage(Barrier.image).setSubresourceRange(Barrier.subresourceRange));
			}
			if (!SourceStages) SourceStages = vk::PipelineStageFlagBits::eTopOfPipe;
			if (!DestinationStages) DestinationStages = vk::PipelineStageFlagBits::eBottomOfPipe;
			Commands.pipelineBarrier(SourceStages, DestinationStages, {}, {}, LegacyBuffers, LegacyTextures);
		}
	};

	FVulkanQueueTransfer::FVulkanQueueTransfer(FVulkanDevice& Device, FRHIQueueId Source, FRHIQueueId Destination,
		std::span<const FRHIBufferTransition> Buffers, std::span<const FRHITextureTransition> Textures, bool bUseSynchronization2)
		: FRHIQueueTransfer(Source, Destination)
		, State(std::make_unique<FState>(Device, Source, Destination, bUseSynchronization2 && Device.SupportsSynchronization2()))
	{
		require(Source != Destination && Device.FindQueue(Source) && Device.FindQueue(Destination));
		require(!Buffers.empty() || !Textures.empty());
		std::string Error;
		requiref(ValidateBufferTransitions(Buffers, Error) && ValidateTextureTransitions(Textures, Error), "{}", Error);
		State->Buffers.assign(Buffers.begin(), Buffers.end());
		State->Textures.assign(Textures.begin(), Textures.end());
		static std::atomic<uint64> NextId = 1;
		for (const auto& Transition : Buffers)
		{
			State->BufferOwners.emplace_back(Transition.Buffer);
			State->BufferIds.push_back(NextId.fetch_add(1));
			require(State->BufferIds.back() != 0);
		}
		for (const auto& Transition : Textures)
		{
			State->TextureOwners.emplace_back(Transition.Texture);
			State->TextureIds.push_back(NextId.fetch_add(1));
			require(State->TextureIds.back() != 0);
		}
	}

	FVulkanQueueTransfer::~FVulkanQueueTransfer() = default;
	auto FVulkanQueueTransfer::GetReleaseTicket() const -> FRHIGPUSubmissionTicket { return State->Signal; }

	auto FVulkanQueueTransfer::RecordRelease(FVulkanQueue& Queue, vk::CommandBuffer Commands,
		const FRHIGPUSubmissionTicket& Ticket) -> void
	{
		CheckVulkanRHIThread();
		require(State->Phase == FState::EPhase::Prepared && Queue.GetId() == State->Source);
		require(State->Device.FindQueue(Queue.GetId()) == &Queue);
		require(Queue.GetCompletionTracker().Owns(Ticket) && Ticket.GetState() == ERHIGPUSubmissionState::Pending);
		const auto SourceFamily = Queue.GetFamilyIndex();
		const auto DestinationFamily = State->Device.FindQueue(State->Destination)->GetFamilyIndex();
		const bool bSameFamily = SourceFamily == DestinationFamily;
		std::unordered_map<FVulkanBuffer*, FVulkanBufferStateTracker> BufferStates;
		std::unordered_map<FVulkanTexture*, FVulkanTextureStateTracker> TextureStates;
		State->BufferBarriers.clear(); State->TextureBarriers.clear();
		State->BufferSources.clear(); State->BufferDestinations.clear();
		State->TextureSources.clear(); State->TextureDestinations.clear();
		for (size_t Index = 0; Index < State->Buffers.size(); ++Index)
		{
			const auto& Transition = State->Buffers[Index];
			auto* Buffer = static_cast<FVulkanBuffer*>(Transition.Buffer);
			auto& Candidate = BufferStates.try_emplace(Buffer, Buffer->GetStateTracker()).first->second;
			ERHIAccess Tracked;
			require(Candidate.Validate(Transition.Offset, Transition.Size, Transition.ExpectedBefore, Tracked));
			const auto Source = Candidate.GetBarrierSource(Transition.Offset, Transition.Size);
			const auto Destination = MapVulkanResourceState(Transition.RequiredAfter);
			require(Candidate.GetOwnership().Release(Transition.Offset, Transition.Size, SourceFamily, DestinationFamily, State->BufferIds[Index]));
			State->BufferSources.push_back(Source);
			State->BufferDestinations.push_back(Destination);
			State->BufferBarriers.push_back(vk::BufferMemoryBarrier2()
				.setSrcStageMask(Source.StageMask2).setSrcAccessMask(Source.AccessMask2)
				.setDstStageMask(Destination.StageMask2).setDstAccessMask(Destination.AccessMask2)
				.setSrcQueueFamilyIndex(bSameFamily ? VK_QUEUE_FAMILY_IGNORED : SourceFamily)
				.setDstQueueFamilyIndex(bSameFamily ? VK_QUEUE_FAMILY_IGNORED : DestinationFamily)
				.setBuffer(Buffer->GetHandle()).setOffset(Transition.Offset).setSize(Transition.Size));
		}
		for (size_t Index = 0; Index < State->Textures.size(); ++Index)
		{
			const auto& Transition = State->Textures[Index];
			auto* Texture = static_cast<FVulkanTexture*>(Transition.Texture);
			auto& Candidate = TextureStates.try_emplace(Texture, Texture->GetStateTracker()).first->second;
			ERHIAccess Tracked;
			require(Candidate.Validate(Transition.Range, Transition.ExpectedBefore, Tracked));
			const auto Source = Candidate.GetBarrierSource(Transition.Range,
				Transition.bDiscardContents || Transition.ExpectedBefore == ERHIAccess::Discard);
			const auto Destination = MapVulkanResourceState(Transition.RequiredAfter);
			require(Candidate.ReleaseOwnership(Transition.Range, SourceFamily, DestinationFamily, State->TextureIds[Index]));
			State->TextureSources.push_back(Source);
			State->TextureDestinations.push_back(Destination);
			State->TextureBarriers.push_back(vk::ImageMemoryBarrier2()
				.setSrcStageMask(Source.StageMask2).setSrcAccessMask(Source.AccessMask2)
				.setDstStageMask(Destination.StageMask2).setDstAccessMask(Destination.AccessMask2)
				.setOldLayout(Source.Layout).setNewLayout(Destination.Layout)
				.setSrcQueueFamilyIndex(bSameFamily ? VK_QUEUE_FAMILY_IGNORED : SourceFamily)
				.setDstQueueFamilyIndex(bSameFamily ? VK_QUEUE_FAMILY_IGNORED : DestinationFamily)
				.setImage(Texture->Image).setSubresourceRange({ToVulkanAspectFlags(Transition.Range.Aspects),
					Transition.Range.FirstMip, Transition.Range.NumMips, Transition.Range.FirstArrayLayer, Transition.Range.NumArrayLayers}));
		}
		State->Emit(Commands, true);
		for (auto& [Buffer, Candidate] : BufferStates) Buffer->GetStateTracker() = std::move(Candidate);
		for (auto& [Texture, Candidate] : TextureStates) Texture->GetStateTracker() = std::move(Candidate);
		State->Signal = Ticket;
		State->Phase = FState::EPhase::Released;
	}

	auto FVulkanQueueTransfer::RecordAcquire(FVulkanQueue& Queue, vk::CommandBuffer Commands) -> void
	{
		CheckVulkanRHIThread();
		require(State->Phase == FState::EPhase::Released && Queue.GetId() == State->Destination);
		require(State->Device.FindQueue(Queue.GetId()) == &Queue);
		const auto Status = State->Signal.GetState();
		require(Status == ERHIGPUSubmissionState::Pending
			|| Status == ERHIGPUSubmissionState::Submitted || Status == ERHIGPUSubmissionState::Complete);
		const auto SourceFamily = State->Device.FindQueue(State->Source)->GetFamilyIndex();
		const auto DestinationFamily = Queue.GetFamilyIndex();
		std::unordered_map<FVulkanBuffer*, FVulkanBufferStateTracker> BufferStates;
		std::unordered_map<FVulkanTexture*, FVulkanTextureStateTracker> TextureStates;
		for (size_t Index = 0; Index < State->Buffers.size(); ++Index)
		{
			const auto& Transition = State->Buffers[Index];
			auto* Buffer = static_cast<FVulkanBuffer*>(Transition.Buffer);
			auto& Candidate = BufferStates.try_emplace(Buffer, Buffer->GetStateTracker()).first->second;
			require(Candidate.GetOwnership().Acquire(Transition.Offset, Transition.Size, SourceFamily, DestinationFamily, State->BufferIds[Index]));
			Candidate.Apply(Transition.Offset, Transition.Size, Transition.RequiredAfter);
		}
		for (size_t Index = 0; Index < State->Textures.size(); ++Index)
		{
			const auto& Transition = State->Textures[Index];
			auto* Texture = static_cast<FVulkanTexture*>(Transition.Texture);
			auto& Candidate = TextureStates.try_emplace(Texture, Texture->GetStateTracker()).first->second;
			require(Candidate.AcquireOwnership(Transition.Range, SourceFamily, DestinationFamily, State->TextureIds[Index]));
			Candidate.Apply(Transition.Range, Transition.RequiredAfter);
		}
		State->Emit(Commands, false);
		for (auto& [Buffer, Candidate] : BufferStates) Buffer->GetStateTracker() = std::move(Candidate);
		for (auto& [Texture, Candidate] : TextureStates) Texture->GetStateTracker() = std::move(Candidate);
		State->Phase = FState::EPhase::Acquired;
	}
}
