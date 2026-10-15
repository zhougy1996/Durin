#include "MetalShaderFormat.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderFormat
{
	auto EncodeMetalMetadata(EShaderFrequency Frequency, const FMetalBindingMap& Map)
		-> std::optional<FSharedByteBuffer>
	{
		if (!ValidateMetalBindingRemap(Frequency, Map.Bindings, Map.PushConstantBufferSlot, Map.Identity)) return {};
		FBinaryWriter Writer({.MaximumTotalBytes = MaximumMetadataBytes});
		Writer.WriteU32(1); Writer.WriteU32(uint32(Frequency));
		Writer.WriteU32(Map.PushConstantBufferSlot); Writer.WriteU32(uint32(Map.Bindings.size()));
		for (const auto& Binding : Map.Bindings)
		{
			Writer.WriteU32(Binding.SetIndex); Writer.WriteU32(Binding.BindingIndex);
			Writer.WriteU32(uint32(Binding.Type)); Writer.WriteU32(Binding.Slot); Writer.WriteU32(Binding.Count);
		}
		Writer.WriteU64(Map.Identity.HashLow); Writer.WriteU64(Map.Identity.HashHigh);
		if (Writer.HasError()) return {};
		return FSharedByteBuffer::Take(Writer.TakeBytes());
	}

	auto DecodeMetalMetadata(EShaderFrequency Frequency, FByteView Bytes) -> std::optional<FMetalBindingMap>
	{
		++GetMetalFormatWorkCounters().MetadataDecodes;
		FBinaryReader Reader(Bytes, {.MaximumTotalBytes = MaximumMetadataBytes});
		uint32 Version = 0, Stage = 0, Count = 0;
		FMetalBindingMap Map;
		if (Bytes.size() > MaximumMetadataBytes || !Reader.ReadU32(Version) || Version != 1
			|| !Reader.ReadU32(Stage) || Stage != uint32(Frequency)
			|| !Reader.ReadU32(Map.PushConstantBufferSlot) || !Reader.ReadU32(Count) || Count > MaximumMetalBindings) return {};
		Map.Bindings.reserve(Count);
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FMetalShaderBinding Binding; uint32 Type = 0;
			if (!Reader.ReadU32(Binding.SetIndex) || !Reader.ReadU32(Binding.BindingIndex)
				|| !Reader.ReadU32(Type) || Type > uint32(ERHIBindingType::StorageImage)
				|| !Reader.ReadU32(Binding.Slot) || !Reader.ReadU32(Binding.Count)) return {};
			Binding.Type = ERHIBindingType(Type); Map.Bindings.push_back(Binding);
		}
		if (!Reader.ReadU64(Map.Identity.HashLow) || !Reader.ReadU64(Map.Identity.HashHigh)
			|| !Reader.IsAtEnd() || !ValidateMetalBindingRemap(Frequency, Map.Bindings, Map.PushConstantBufferSlot, Map.Identity)) return {};
		return Map;
	}
}
