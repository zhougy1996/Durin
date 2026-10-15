#pragma once

#include "CoreMinimal.h"
#include "MetalShaderFormat.h"
#include "RHIShaderParameters.h"

namespace Durin
{
	// SDK-free preparation shared by native pipelines and host contract fixtures.
	struct FMetalParameterSlot
	{
		uint32 SetIndex;
		uint32 BindingIndex;
		uint32 ArrayElement;
		ERHIBindingType Type;
		EShaderStageFlags Stages;
		auto Address() const { return std::tuple(SetIndex, BindingIndex, ArrayElement); }
	};
	struct FMetalBindingOperation
	{
		uint32 ParameterIndex;
		uint32 NativeSlot;
		EShaderStageFlags Stage;
	};
	struct FMetalStageBindings
	{
		EShaderFrequency Frequency;
		std::span<const FMetalShaderBinding> Bindings;
		uint32 PushSlot;
	};
	struct FMetalPushOperation
	{
		uint32 NativeSlot = UINT32_MAX;
		uint32 Extent = 0;
		std::vector<FPushConstantRange> RequiredRanges;
	};
	struct FMetalBindingPlan
	{
		std::vector<FMetalParameterSlot> Parameters;
		std::vector<FMetalBindingOperation> Operations;
		std::array<FMetalPushOperation, 3> Push;

		auto Find(const FRHIShaderParameterResource& Parameter, EShaderStageFlags Stage) const
			-> std::optional<uint32>
		{
			if (Stage != EShaderStageFlags::Vertex && Stage != EShaderStageFlags::Fragment
				&& Stage != EShaderStageFlags::Compute) return {};
			const auto Key = std::tuple(Parameter.SetIndex, Parameter.BindingIndex, Parameter.ArrayElement);
			const auto It = std::ranges::lower_bound(Parameters, Key, {}, &FMetalParameterSlot::Address);
			if (It == Parameters.end() || It->Address() != Key || It->Type != Parameter.Type
				|| !EnumHasAnyFlags(It->Stages, Stage)) return {};
			return uint32(It - Parameters.begin());
		}
		auto RetainedBytes() const -> size_t
		{
			size_t Bytes = sizeof(*this) + Parameters.capacity() * sizeof(FMetalParameterSlot)
				+ Operations.capacity() * sizeof(FMetalBindingOperation);
			for (const auto& Item : Push) Bytes += Item.RequiredRanges.capacity() * sizeof(FPushConstantRange);
			return Bytes;
		}
	};

	inline auto PrepareMetalBindings(const FPipelineLayoutDesc& Layout,
		std::span<const FMetalStageBindings> Stages) -> std::shared_ptr<const FMetalBindingPlan>
	{
		auto Plan = std::make_shared<FMetalBindingPlan>();
		// At most three stages of the format's native resource limits, independent of sparse addresses.
		constexpr uint32 MaximumElements = 3 * ShaderFormat::MaximumMetalBindings;
		if (Layout.BindingLayouts.size() > 65536 || Layout.PushConstantRanges.size() > 65536) return {};
		for (uint32 SetIndex = 0; SetIndex < Layout.BindingLayouts.size(); ++SetIndex)
		for (const auto& Binding : Layout.BindingLayouts[SetIndex].BindingLayouts)
		{
			if (Binding.StageFlags == EShaderStageFlags::None || SetIndex > 65535 || Binding.Slot > 65535 || Binding.ArraySize == 0
				|| Binding.ArraySize > MaximumElements - Plan->Parameters.size()) return {};
			for (uint32 Element = 0; Element < Binding.ArraySize; ++Element)
				Plan->Parameters.push_back({SetIndex, Binding.Slot, Element, Binding.Type, Binding.StageFlags});
		}
		std::ranges::sort(Plan->Parameters, {}, &FMetalParameterSlot::Address);
		for (size_t Index = 1; Index < Plan->Parameters.size(); ++Index)
			if (Plan->Parameters[Index - 1].Address() == Plan->Parameters[Index].Address()) return {};
		EShaderStageFlags Seen = EShaderStageFlags::None;
		for (const auto& Source : Stages)
		{
			const uint32 StageIndex = Source.Frequency == EShaderFrequency::Vertex ? 0
				: Source.Frequency == EShaderFrequency::Fragment ? 1 : 2;
			const auto Stage = StageIndex == 0 ? EShaderStageFlags::Vertex
				: StageIndex == 1 ? EShaderStageFlags::Fragment : EShaderStageFlags::Compute;
			if (EnumHasAnyFlags(Seen, Stage) || !ValidateMetalBindingRemap(Source.Frequency, Source.Bindings,
				Source.PushSlot, ComputeMetalBindingRemapIdentity(Source.Frequency, Source.Bindings, Source.PushSlot))) return {};
			Seen |= Stage;
			size_t Required = 0;
			for (const auto& Parameter : Plan->Parameters) if (EnumHasAnyFlags(Parameter.Stages, Stage)) ++Required;
			size_t Matched = 0;
			for (const auto& Binding : Source.Bindings)
			for (uint32 Element = 0; Element < Binding.Count; ++Element)
			{
				const auto Key = std::tuple(Binding.SetIndex, Binding.BindingIndex, Element);
				const auto It = std::ranges::lower_bound(Plan->Parameters, Key, {}, &FMetalParameterSlot::Address);
				if (It == Plan->Parameters.end() || It->Address() != Key
					|| !EnumHasAnyFlags(It->Stages, Stage)
					|| Binding.Type != (It->Type == ERHIBindingType::UniformBufferDynamic ? ERHIBindingType::UniformBuffer : It->Type)) return {};
				Plan->Operations.push_back({uint32(It - Plan->Parameters.begin()), Binding.Slot + Element, Stage});
				++Matched;
			}
			if (Matched != Required) return {};
			auto& Push = Plan->Push[StageIndex];
			Push.NativeSlot = Source.PushSlot;
			for (const auto& Range : Layout.PushConstantRanges)
			{
				if (!EnumHasAnyFlags(Range.StageFlags, Stage)) continue;
				if (Range.Size == 0 || uint64(Range.Offset) + Range.Size > 65536) return {};
				Push.RequiredRanges.push_back(Range);
				Push.Extent = std::max(Push.Extent, Range.Offset + Range.Size);
			}
			if ((Push.Extent != 0) != (Push.NativeSlot != UINT32_MAX)) return {};
		}
		for (const auto& Parameter : Plan->Parameters)
			if ((Parameter.Stages & Seen) != Parameter.Stages) return {};
		for (const auto& Range : Layout.PushConstantRanges)
			if (Range.StageFlags == EShaderStageFlags::None || (Range.StageFlags & Seen) != Range.StageFlags) return {};
		if (Plan->RetainedBytes() > 1024 * 1024) return {};
		return Plan;
	}

	// Dense state owns each resource independently, so replacing one entry is O(1).
	struct FMetalBindingStateCounters
	{
		uint64 ParameterLookups = 0;
		uint64 MappingAllocations = 0;
		uint64 SubmissionBatches = 0;
		uint64 SubmittedOperations = 0;
	};
	class FMetalParameterState
	{
	public:
		auto Reset(std::shared_ptr<const FMetalBindingPlan> InPlan = {}) -> void
		{
			Plan = std::move(InPlan); Values.clear(); Owners.clear(); Written.clear();
			const size_t Count = Plan ? Plan->Parameters.size() : 0;
			Counters.MappingAllocations += (Count > Values.capacity()) + (Count > Owners.capacity()) + (Count > Written.capacity());
			Values.resize(Count); Owners.resize(Count); Written.resize(Count); Missing = Count;
		}
		auto Find(const FRHIShaderParameterResource& Parameter, EShaderStageFlags Stage) const -> std::optional<uint32>
		{
			++Counters.ParameterLookups;
			return Plan && Parameter.Resource ? Plan->Find(Parameter, Stage) : std::nullopt;
		}
		auto Write(uint32 Index, const FRHIShaderParameterResource& Parameter) -> void
		{
			Values[Index] = Parameter; Owners[Index] = Parameter.Resource;
			if (!Written[Index]) { Written[Index] = 1; --Missing; }
		}
		auto IsComplete() const -> bool { return Plan && Missing == 0; }
		auto Get(uint32 Index) const -> const FRHIShaderParameterResource& { return Values[Index]; }
		auto OperationsForSubmission() const -> std::span<const FMetalBindingOperation>
		{
			++Counters.SubmissionBatches; Counters.SubmittedOperations += Plan->Operations.size();
			return Plan->Operations;
		}
		auto GetCounters() const -> const FMetalBindingStateCounters& { return Counters; }
	private:
		std::shared_ptr<const FMetalBindingPlan> Plan;
		std::vector<FRHIShaderParameterResource> Values;
		std::vector<TRefCountPtr<FRHIResource>> Owners;
		std::vector<uint8> Written;
		size_t Missing = 0;
		mutable FMetalBindingStateCounters Counters;
	};

	class FMetalPushState
	{
	public:
		auto Reset(const FMetalPushOperation* Operation = nullptr) -> void
		{
			Bytes.clear(); Required.clear(); Written.clear(); Missing = 0;
			if (!Operation) return;
			Bytes.resize(Operation->Extent); Required.resize(Operation->Extent); Written.resize(Operation->Extent);
			for (const auto& Range : Operation->RequiredRanges)
			for (uint32 Byte = Range.Offset; Byte < Range.Offset + Range.Size; ++Byte)
				if (!Required[Byte]) { Required[Byte] = 1; ++Missing; }
		}
		auto Write(uint32 Offset, uint32 Size, const void* Data) -> bool
		{
			if (!Data || Size == 0 || Offset % 4 || Size % 4 || Offset > Bytes.size() || Size > Bytes.size() - Offset) return false;
			for (uint32 Byte = Offset; Byte < Offset + Size; ++Byte) if (!Required[Byte]) return false;
			std::memcpy(Bytes.data() + Offset, Data, Size);
			for (uint32 Byte = Offset; Byte < Offset + Size; ++Byte)
				if (!Written[Byte]) { Written[Byte] = 1; --Missing; }
			return true;
		}
		auto IsComplete() const -> bool { return Missing == 0; }
		auto GetBytes() const -> FByteView { return Bytes; }
	private:
		FByteBuffer Bytes;
		std::vector<uint8> Required;
		std::vector<uint8> Written;
		size_t Missing = 0;
	};
}
