#include "ShaderFormat.h"
#include "MetalShaderFormat.h"
#include "VulkanShaderFormat.h"
#include "Hash/CanonicalHash.h"

namespace Durin::ShaderFormat
{
	namespace
	{
		auto IsValidFrequency(EShaderFrequency Frequency) -> bool
		{ return uint32(Frequency) <= uint32(EShaderFrequency::RayMiss); }

		auto ValidateLogicalInterface(EShaderFrequency Frequency,
			std::span<const FLogicalBinding> Bindings, std::span<const FPushConstantRange> PushRanges) -> bool
		{
			if (!IsValidFrequency(Frequency) || Bindings.size() > 65536 || PushRanges.size() > 65536) return false;
			const auto Stage = Frequency == EShaderFrequency::Vertex ? EShaderStageFlags::Vertex
				: Frequency == EShaderFrequency::Fragment ? EShaderStageFlags::Fragment
				: Frequency == EShaderFrequency::Compute ? EShaderStageFlags::Compute : EShaderStageFlags::None;
			// Current logical visibility has no ray-stage vocabulary.
			if (Stage == EShaderStageFlags::None) return Bindings.empty() && PushRanges.empty();
			std::vector<uint64> Addresses;
			Addresses.reserve(Bindings.size());
			for (const auto& Binding : Bindings)
			{
				const auto& Item = Binding.Declaration;
				if (Binding.SetIndex > 65535 || Item.Slot > 65535 || Item.ArraySize == 0 || Item.ArraySize > 65536
					|| uint32(Item.Type) > uint32(ERHIBindingType::StorageImage) || Item.StageFlags != Stage) return false;
				Addresses.push_back((uint64(Binding.SetIndex) << 32) | Item.Slot);
			}
			std::ranges::sort(Addresses);
			if (std::ranges::adjacent_find(Addresses) != Addresses.end()) return false;
			std::vector<FPushConstantRange> Ranges(PushRanges.begin(), PushRanges.end());
			std::ranges::sort(Ranges, {}, &FPushConstantRange::Offset);
			uint32 PreviousEnd = 0;
			for (const auto& Range : Ranges)
			{
				if (Range.StageFlags != Stage || Range.Size == 0 || Range.Size > 65536
					|| Range.Offset % 4 || Range.Size % 4 || Range.Offset > 65536 - Range.Size
					|| Range.Offset < PreviousEnd) return false;
				PreviousEnd = Range.Offset + Range.Size;
			}
			return true;
		}
	}

	auto ValidateExecutionMetadata(const FShaderTargetIdentity& Target,
		EShaderFrequency Frequency, const std::array<uint32, 3>& GroupSize) -> bool
	{
		if (!IsValidFrequency(Frequency) || (Target != VulkanShaderTarget && !ResolveMetalCompileSettings(Target))) return false;
		if (Frequency != EShaderFrequency::Compute) return GroupSize == std::array<uint32, 3>{};
		return Target == VulkanShaderTarget || IsValidComputeThreadGroupSize(GroupSize);
	}

	auto ValidateCode(const FShaderTargetIdentity& Target, EShaderCodeFormat Format, FByteView Code) -> bool
	{
		if (Target == VulkanShaderTarget)
			return Format == EShaderCodeFormat::Spirv15 && ValidateVulkanCode(Code);
		if (!ResolveMetalCompileSettings(Target) || Format != Target.OutputFormat) return false;
		return Format == EShaderCodeFormat::MetalLibrary ? ValidateMetalLibrary(Code) : ValidateMetalCode(Code);
	}

	auto ComputeShaderIdentity(const FShaderCode& Shader) -> FXxHash128
	{
		if (Shader.Reflection.ResourceBindings.size() > 65536 || Shader.Reflection.PushConstantRanges.size() > 65536)
			return {};
		// Retain the v1 wire identity while deriving it directly from reflection.
		std::vector<const FShaderResourceBinding*> Bindings;
		Bindings.reserve(Shader.Reflection.ResourceBindings.size());
		for (const auto& Binding : Shader.Reflection.ResourceBindings)
		{
			if (Binding.SetIndex > 65535) return {};
			Bindings.push_back(&Binding);
		}
		std::ranges::sort(Bindings, [](const auto* A, const auto* B) {
			return std::tie(A->SetIndex, A->BindingIndex, A->Name) < std::tie(B->SetIndex, B->BindingIndex, B->Name);
		});
		FXxHash128Builder Names;
		UpdateCanonicalHashString(Names, "Durin.Shader.InterfaceNames.v1");
		UpdateCanonicalHash(Names, uint64(Bindings.size()));
		for (const auto* Binding : Bindings)
		{
			UpdateCanonicalHash(Names, Binding->SetIndex); UpdateCanonicalHash(Names, Binding->BindingIndex);
			UpdateCanonicalHashString(Names, Binding->Name);
		}
		FXxHash128Builder Hash;
		UpdateCanonicalHashString(Hash, "Durin.Shader.Artifact.v1");
		UpdateCanonicalHash(Hash, Shader.Target.Platform);
		UpdateCanonicalHash(Hash, Shader.Target.Backend);
		UpdateCanonicalHash(Hash, Shader.Target.IntermediateFormat);
		UpdateCanonicalHash(Hash, Shader.Target.OutputFormat);
		UpdateCanonicalHash(Hash, Shader.Target.SettingsDigest);
		UpdateCanonicalHash(Hash, Shader.CodeFormat);
		UpdateCanonicalHash(Hash, Shader.PlatformDataVersion);
		UpdateCanonicalHash(Hash, Shader.Frequency);
		for (const auto Size : Shader.ComputeThreadGroupSize) UpdateCanonicalHash(Hash, Size);
		UpdateCanonicalHashString(Hash, Shader.BinaryEntryPoint);
		UpdateCanonicalHash(Hash, Names.Finalize());
		UpdateCanonicalHash(Hash, Shader.Code.GetSize()); Hash.Update(Shader.Code.GetBytes());
		UpdateCanonicalHash(Hash, Shader.PlatformData.GetSize()); Hash.Update(Shader.PlatformData.GetBytes());
		UpdateCanonicalHash(Hash, true);
		const uint64 SetCount = Bindings.empty() ? 0 : uint64(Bindings.back()->SetIndex) + 1;
		UpdateCanonicalHash(Hash, SetCount);
		size_t Index = 0;
		for (uint64 Set = 0; Set < SetCount; ++Set)
		{
			const size_t Begin = Index;
			while (Index < Bindings.size() && Bindings[Index]->SetIndex == Set) ++Index;
			UpdateCanonicalHash(Hash, uint64(Index - Begin));
			for (size_t Slot = Begin; Slot < Index; ++Slot)
			{
				const auto& Binding = *Bindings[Slot];
				UpdateCanonicalHash(Hash, Binding.StageFlags); UpdateCanonicalHash(Hash, Binding.BindingIndex);
				UpdateCanonicalHash(Hash, Binding.Type); UpdateCanonicalHash(Hash, Binding.ArraySize);
			}
		}
		auto Ranges = Shader.Reflection.PushConstantRanges;
		std::ranges::sort(Ranges, [](const auto& A, const auto& B) { return std::tie(A.Offset, A.Size) < std::tie(B.Offset, B.Size); });
		UpdateCanonicalHash(Hash, uint64(Ranges.size()));
		for (const auto& Range : Ranges)
		{
			UpdateCanonicalHash(Hash, Range.StageFlags); UpdateCanonicalHash(Hash, Range.Offset); UpdateCanonicalHash(Hash, Range.Size);
		}
		return Hash.Finalize();
	}

	auto BuildLogicalBindings(const FShaderReflectionData& Reflection) -> std::vector<FLogicalBinding>
	{
		std::vector<FLogicalBinding> Bindings;
		Bindings.reserve(Reflection.ResourceBindings.size());
		for (const auto& Binding : Reflection.ResourceBindings)
			Bindings.push_back({Binding.SetIndex, {Binding.StageFlags, Binding.BindingIndex, Binding.Type, Binding.ArraySize}});
		return Bindings;
	}

	auto ValidateCodeHash(FByteView Code, FXxHash128 Hash) -> bool
	{
		return !Code.empty() && FXxHash128::HashBuffer(Code) == Hash;
	}

	auto ValidateShaderCode(const FShaderCode& Shader) -> bool
	{
		if (!ValidateExecutionMetadata(Shader.Target, Shader.Frequency, Shader.ComputeThreadGroupSize)
			|| !ValidateCodeHash(Shader.Code, Shader.CodeHash)
			|| Shader.Reflection.ResourceBindings.size() > 65536 || Shader.Reflection.PushConstantRanges.size() > 65536) return false;
		const auto Bindings = BuildLogicalBindings(Shader.Reflection);
		return ValidateShaderCodeParts(Shader.Target, Shader.CodeFormat, Shader.PlatformDataVersion, Shader.Frequency,
			Shader.Code.GetBytes(), Shader.BinaryEntryPoint, Shader.PlatformData.GetBytes(), Bindings, Shader.Reflection.PushConstantRanges);
	}

	auto ValidateShaderCreateDesc(const FRHIShaderCreateDesc& Desc) -> bool
	{
		if (!Desc.EntryPoint || !*Desc.EntryPoint) return false;
		if (!Desc.CodeOwner)
			return Desc.ShaderIdentity.IsZero()
				&& ValidateExecutionMetadata(Desc.Target, Desc.Frequency, Desc.ComputeThreadGroupSize)
				&& ValidateCode(Desc.Target, Desc.CodeFormat, Desc.Code) && ValidateCodeHash(Desc.Code, Desc.Hash);
		const auto& Shader = *Desc.CodeOwner;
		return Desc.Target == Shader.Target && Desc.CodeFormat == Shader.CodeFormat
			&& Desc.Frequency == Shader.Frequency && Desc.ComputeThreadGroupSize == Shader.ComputeThreadGroupSize
			&& std::string_view(Desc.EntryPoint) == Shader.BinaryEntryPoint
			&& Desc.Hash == Shader.CodeHash && std::ranges::equal(Desc.Code, Shader.Code.GetBytes())
			&& (Desc.ShaderIdentity.IsZero() || Desc.ShaderIdentity == Shader.ShaderIdentity)
			&& ValidateShaderCode(Shader) && Shader.ShaderIdentity == ComputeShaderIdentity(Shader);
	}

	auto ValidateShaderCodeParts(const FShaderTargetIdentity& Target, EShaderCodeFormat Format,
		uint32 Version, EShaderFrequency Frequency, FByteView Code, std::string_view EntryPoint,
		FByteView Metadata, std::span<const FLogicalBinding> Bindings, std::span<const FPushConstantRange> PushRanges) -> bool
	{
		if (Version != ShaderMetadataVersion || !ValidateCode(Target, Format, Code)
			|| EntryPoint.empty() || EntryPoint.find('\0') != std::string_view::npos || Metadata.size() > MaximumMetadataBytes
			|| !ValidateLogicalInterface(Frequency, Bindings, PushRanges)) return false;
		if (Target == VulkanShaderTarget)
			return Metadata.empty();
		if (Format == EShaderCodeFormat::Msl20Source)
		{
			const std::string_view Source(reinterpret_cast<const char*>(Code.data()), Code.size());
			if (!Source.contains(EntryPoint)) return false;
		}
		const auto Map = DecodeMetalMetadata(Frequency, Metadata);
		const auto Expected = BuildMetalBindingMap(Frequency, Bindings, !PushRanges.empty());
		return Map && Expected && Map->Bindings == Expected->Bindings
			&& Map->PushConstantBufferSlot == Expected->PushConstantBufferSlot && Map->Identity == Expected->Identity;
	}
}
