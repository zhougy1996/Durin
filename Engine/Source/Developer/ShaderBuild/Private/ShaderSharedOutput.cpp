#include "ShaderSharedOutput.h"
#include "Shader/ShaderCompiledOutput.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderSharedOutput
{
	using namespace DerivedData;
	namespace
	{
		constexpr uint64 MaximumCodeBytes = 64ull * 1024 * 1024;
		constexpr uint32 MaximumReflectionEntries = 65536;
		constexpr uint32 MaximumDescriptorIndex = 65535;
		constexpr uint32 MaximumPushBytes = 65536;
		constexpr uint64 MaximumStringBytes = 32768;
		constexpr uint64 MaximumMetadataBytes = 4 + 8 + MaximumStringBytes + 6 * 4
			+ 32 * (2 * (8 + MaximumStringBytes) + 8 + 16);
		auto WriteTarget(FBinaryWriter& Writer, const FShaderTargetIdentity& Target) -> void
		{
			Writer.WriteU32(uint32(Target.Platform)); Writer.WriteU32(uint32(Target.Backend));
			Writer.WriteU32(uint32(Target.IntermediateFormat)); Writer.WriteU32(uint32(Target.OutputFormat));
			Writer.WriteU32(Target.MslLanguageVersion); Writer.WriteU32(Target.BindingRemapSchema);
		}
		auto ReadTarget(FBinaryReader& Reader, FShaderTargetIdentity& Target) -> bool
		{
			uint32 Platform = 0, Backend = 0, Intermediate = 0, Output = 0;
			if (!Reader.ReadU32(Platform) || !Reader.ReadU32(Backend)
				|| !Reader.ReadU32(Intermediate) || !Reader.ReadU32(Output)
				|| !Reader.ReadU32(Target.MslLanguageVersion)
				|| !Reader.ReadU32(Target.BindingRemapSchema)) return false;
			Target.Platform = EShaderTargetPlatform(Platform); Target.Backend = EShaderRuntimeBackend(Backend);
			Target.IntermediateFormat = EShaderCodeFormat(Intermediate);
			Target.OutputFormat = EShaderCodeFormat(Output);
			return true;
		}
		auto Failure(EShaderError Code, uint64 Index = 0) -> std::unexpected<FShaderError>
		{ return std::unexpected(FShaderError{.Code = Code, .Index = Index}); }
		auto ValidFlags(uint32 Flags) -> bool
		{
			constexpr uint32 Mask = uint32(EShaderStageFlags::Vertex) | uint32(EShaderStageFlags::Fragment)
				| uint32(EShaderStageFlags::Compute) | uint32(EShaderStageFlags::Geometry);
			return (Flags & ~Mask) == 0;
		}
		auto ValidBinding(const FShaderResourceBinding& Binding) -> bool
		{
			return Binding.Name.size() <= MaximumStringBytes && ValidFlags(uint32(Binding.StageFlags))
				&& Binding.SetIndex <= MaximumDescriptorIndex && Binding.BindingIndex <= MaximumDescriptorIndex
				&& uint32(Binding.Type) <= uint32(ERHIBindingType::StorageImage)
				&& Binding.ArraySize && Binding.ArraySize <= MaximumReflectionEntries;
		}
		auto ValidRange(const FPushConstantRange& Range) -> bool
		{
			return ValidFlags(uint32(Range.StageFlags)) && Range.Size && Range.Size <= MaximumPushBytes
				&& Range.Offset <= MaximumPushBytes - Range.Size;
		}
		auto ValidRequest(const FShaderCompileOptions& Options) -> bool
		{
			if ((Options.Target != VulkanShaderTarget && Options.Target != MetalShaderTarget)
				|| Options.VirtualShaderPath.size() > MaximumStringBytes
				|| Options.EntryPoints.empty() || Options.EntryPoints.size() > ShaderCompiledOutput::MaximumEntryPoints
				|| Options.Frequencies.size() != Options.EntryPoints.size()) return false;
			std::set<std::pair<std::string_view, uint32>> Unique;
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				const std::string_view Name = Options.EntryPoints[Index] ? Options.EntryPoints[Index] : "";
				if (Name.empty() || Name.size() > MaximumStringBytes || uint32(Options.Frequencies[Index]) > uint32(EShaderFrequency::RayMiss)
					|| !Unique.emplace(Name, uint32(Options.Frequencies[Index])).second) return false;
			}
			return true;
		}
		auto ValidCode(FByteView Code, const FShaderTargetIdentity& Target) -> bool
		{
			if (Target == MetalShaderTarget)
			{
				if (Code.size() < 32 || Code.size() > MaximumCodeBytes
					|| std::ranges::find(Code, std::byte{0}) != Code.end()) return false;
				const std::string_view Source(reinterpret_cast<const char*>(Code.data()), Code.size());
				return Source.substr(0, 256).contains("#include <metal_stdlib>");
			}
			uint32 Magic = 0;
			return Code.size() >= 20 && Code.size() <= MaximumCodeBytes && Code.size() % 4 == 0
				&& ReadLittleEndianAt(Code, 0, Magic) && Magic == 0x07230203u;
		}
		auto ValidMetalMap(const FCompiledShader& Shader) -> bool
		{
			if (Shader.Target == VulkanShaderTarget)
				return Shader.MetalBindings.empty()
					&& Shader.MetalPushConstantBufferSlot == UINT32_MAX
					&& Shader.BindingRemapIdentity == FXxHash128{};
			if (Shader.Target != MetalShaderTarget) return false;
			const auto Expected = BuildMetalShaderBindingMap(Shader.Frequency, Shader.Reflection);
			return Expected && Shader.MetalBindings == Expected->Bindings
				&& Shader.MetalPushConstantBufferSlot == Expected->PushConstantBufferSlot
				&& Shader.BindingRemapIdentity == Expected->Identity;
		}
		auto ReadReflection(const FSharedByteBuffer& Block, size_t Entry, FCompiledShader& Shader,
			const std::function<bool()>& Cancel) -> FShaderOperationResult
		{
			FBinaryReader Reader(Block.GetBytes(), {.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
			uint32 Count = 0;
			if (!Reader.ReadU32(Count) || Count > MaximumReflectionEntries) return Failure(EShaderError::PayloadBindingCountInvalid, Entry);
			Shader.Reflection.ResourceBindings.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				if (Index % 256 == 0 && Cancel && Cancel()) return Failure(EShaderError::Cancelled);
				FShaderResourceBinding Binding; uint32 Flags = 0, Type = 0;
				if (!Reader.ReadString(Binding.Name, MaximumStringBytes) || !Reader.ReadU32(Flags)
					|| !Reader.ReadU32(Binding.SetIndex) || !Reader.ReadU32(Binding.BindingIndex)
					|| !Reader.ReadU32(Type) || !Reader.ReadU32(Binding.ArraySize)
					|| Type > uint32(ERHIBindingType::StorageImage) || !ValidFlags(Flags))
					return Failure(EShaderError::PayloadBindingInvalid, Entry);
				Binding.StageFlags = EShaderStageFlags(Flags); Binding.Type = ERHIBindingType(Type);
				if (!ValidBinding(Binding)) return Failure(EShaderError::PayloadBindingInvalid, Entry);
				Shader.Reflection.ResourceBindings.push_back(std::move(Binding));
			}
			if (!Reader.ReadU32(Count) || Count > MaximumReflectionEntries) return Failure(EShaderError::PayloadPushConstantCountInvalid, Entry);
			Shader.Reflection.PushConstantRanges.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				if (Index % 256 == 0 && Cancel && Cancel()) return Failure(EShaderError::Cancelled);
				FPushConstantRange Range{}; uint32 Flags = 0, Reserved = 0;
				if (!Reader.ReadU32(Flags) || !Reader.ReadU32(Range.Offset) || !Reader.ReadU32(Range.Size)
					|| !Reader.ReadU32(Reserved) || Reserved || !ValidFlags(Flags))
					return Failure(EShaderError::PayloadPushConstantInvalid, Entry);
				Range.StageFlags = EShaderStageFlags(Flags);
				if (!ValidRange(Range)) return Failure(EShaderError::PayloadPushConstantInvalid, Entry);
				Shader.Reflection.PushConstantRanges.push_back(Range);
			}
			if (!Reader.ReadU32(Count) || Count > MaximumReflectionEntries)
				return Failure(EShaderError::PayloadBindingCountInvalid, Entry);
			Shader.MetalBindings.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				FMetalShaderBinding Binding;
				uint32 Type = 0;
				if (!Reader.ReadU32(Binding.SetIndex) || !Reader.ReadU32(Binding.BindingIndex)
					|| !Reader.ReadU32(Type) || !Reader.ReadU32(Binding.Slot)
					|| !Reader.ReadU32(Binding.Count)
					|| Type > uint32(ERHIBindingType::StorageImage))
					return Failure(EShaderError::PayloadBindingInvalid, Entry);
				Binding.Type = ERHIBindingType(Type);
				Shader.MetalBindings.push_back(Binding);
			}
			if (!Reader.ReadU32(Shader.MetalPushConstantBufferSlot)
				|| !Reader.ReadU64(Shader.BindingRemapIdentity.HashLow)
				|| !Reader.ReadU64(Shader.BindingRemapIdentity.HashHigh)
				|| !ValidMetalMap(Shader))
				return Failure(EShaderError::PayloadBindingInvalid, Entry);
			if (!Reader.IsAtEnd()) return Failure(EShaderError::PayloadTrailingBytes, Entry);
			return {};
		}
		struct FEntry
		{
			std::string SourceName, BinaryName;
			EShaderFrequency Frequency;
			FXxHash128 Hash;
			FSharedByteBuffer Code, Reflection;
		};
		auto ValueId(uint32 Index, bool Reflection) -> FValueId
		{ return FValueId::FromName("Durin.Shader.EntryValue").MakeIndexed(Index * 2 + uint32(Reflection)); }
		auto ReadLayout(const FShaderCompileOptions& Options, const FBuildOutput& Output,
			const std::function<bool()>& Cancel) -> std::expected<std::vector<FEntry>, FShaderError>
		{
			if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
			if (!ValidRequest(Options)) return Failure(EShaderError::PayloadRequestInvalid);
			const auto Metadata = GetBuildMetadataPayload(Output);
			if (Output.GetSchema() != "Shader.Output" || Output.GetSchemaVersion() != 5
				|| Metadata.size() > MaximumMetadataBytes
				|| !Output.CheckLimits({.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes}))
				return Failure(EShaderError::PayloadHeaderInvalid);
			FBinaryReader Reader(Metadata.GetBytes(), {.MaximumTotalBytes = MaximumMetadataBytes});
			uint32 Count = 0; std::string VirtualShaderPath; FShaderTargetIdentity Target;
			if (!Reader.ReadU32(Count) || !Reader.ReadString(VirtualShaderPath, MaximumStringBytes)
				|| !ReadTarget(Reader, Target) || Target != Options.Target
				|| VirtualShaderPath != Options.VirtualShaderPath
				|| Count != Options.EntryPoints.size() || Output.GetValues().size() != Count * 2)
				return Failure(EShaderError::PayloadHeaderInvalid);
			std::vector<FEntry> Entries; Entries.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
			FEntry Entry; uint32 Frequency = 0, Format = 0;
				if (!Reader.ReadString(Entry.SourceName, MaximumStringBytes) || !Reader.ReadString(Entry.BinaryName, MaximumStringBytes)
					|| !Reader.ReadU32(Frequency) || !Reader.ReadU32(Format)
					|| !Reader.ReadU64(Entry.Hash.HashLow) || !Reader.ReadU64(Entry.Hash.HashHigh)
					|| Entry.SourceName != Options.EntryPoints[Index] || Entry.BinaryName.empty()
					|| Frequency != uint32(Options.Frequencies[Index])
					|| Format != uint32(Target.OutputFormat)) return Failure(EShaderError::PayloadEntryInvalid, Index);
				Entry.Frequency = EShaderFrequency(Frequency);
				const auto* Code = Output.FindValue(ValueId(Index, false));
				const auto* Reflection = Output.FindValue(ValueId(Index, true));
				if (!Code || !Reflection) return Failure(EShaderError::PayloadEntryInvalid, Index);
				if (!ValidCode(Code->GetData().GetBytes(), Target))
					return Failure(Target == MetalShaderTarget ? EShaderError::PayloadMslInvalid : EShaderError::PayloadSpirvInvalid, Index);
				if (Code->GetRawHash() != Entry.Hash)
					return Failure(Target == MetalShaderTarget ? EShaderError::PayloadMslHashMismatch : EShaderError::PayloadSpirvHashMismatch, Index);
				Entry.Code = Code->GetData(); Entry.Reflection = Reflection->GetData();
				Entries.push_back(std::move(Entry));
			}
			if (!Reader.IsAtEnd()) return Failure(EShaderError::PayloadTrailingBytes);
			return Entries;
		}
	}

	auto Make(const FShaderCompileOptions& Options, const FShaderCompilerOutput& Product, const std::function<bool()>& Cancel)
		-> std::expected<FBuildOutput, FShaderError>
	{
		if (!Product || !ValidRequest(Options) || Product.CompiledShaders.size() != Options.EntryPoints.size())
			return Failure(EShaderError::PayloadRequestInvalid);
		FBuildOutputBuilder Output("Shader.Output", 5, {.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
		uint64 DataBytes = 0;
		FBinaryWriter Metadata({.MaximumTotalBytes = MaximumMetadataBytes});
		Metadata.WriteU32(uint32(Product.CompiledShaders.size())); Metadata.WriteString(Options.VirtualShaderPath);
		WriteTarget(Metadata, Options.Target);
		for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
		{
			if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
			const auto& Shader = Product.CompiledShaders[Index];
			if (!Shader.Code || Shader.Target != Options.Target || Shader.CodeFormat != Options.Target.OutputFormat
				|| Shader.SourceEntryPoint != Options.EntryPoints[Index] || Shader.Frequency != Options.Frequencies[Index]
				|| Shader.SourceEntryPoint.size() > MaximumStringBytes || Shader.BinaryEntryPoint.empty() || Shader.BinaryEntryPoint.size() > MaximumStringBytes
				|| !ValidCode(Shader.Code->GetBytes(), Options.Target)
				|| !ValidMetalMap(Shader)
				|| FXxHash128::HashBuffer(Shader.Code->GetBytes()) != Shader.Hash)
				return Failure(EShaderError::PayloadOutputInvalid, Index);
			Metadata.WriteString(Shader.SourceEntryPoint); Metadata.WriteString(Shader.BinaryEntryPoint);
			Metadata.WriteU32(uint32(Shader.Frequency)); Metadata.WriteU32(uint32(Shader.CodeFormat));
			Metadata.WriteU64(Shader.Hash.HashLow); Metadata.WriteU64(Shader.Hash.HashHigh);
			const auto& Reflection = Shader.Reflection;
			if (Reflection.ResourceBindings.size() > MaximumReflectionEntries || Reflection.PushConstantRanges.size() > MaximumReflectionEntries)
				return Failure(EShaderError::PayloadOutputInvalid, Index);
			if (Shader.Code->size() > ShaderCompiledOutput::MaximumValueBytes - DataBytes
				|| Metadata.Tell() > ShaderCompiledOutput::MaximumValueBytes - DataBytes - Shader.Code->size())
				return Failure(EShaderError::PayloadTooLarge, Index);
			FBinaryWriter Descriptor({.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes - DataBytes - Shader.Code->size() - Metadata.Tell()});
			Descriptor.WriteU32(uint32(Reflection.ResourceBindings.size()));
			for (const auto& Binding : Reflection.ResourceBindings)
			{
				if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
				if (!ValidBinding(Binding)) return Failure(EShaderError::PayloadBindingInvalid, Index);
				Descriptor.WriteString(Binding.Name); Descriptor.WriteU32(uint32(Binding.StageFlags));
				Descriptor.WriteU32(Binding.SetIndex); Descriptor.WriteU32(Binding.BindingIndex);
				Descriptor.WriteU32(uint32(Binding.Type)); Descriptor.WriteU32(Binding.ArraySize);
			}
			Descriptor.WriteU32(uint32(Reflection.PushConstantRanges.size()));
			for (const auto& Range : Reflection.PushConstantRanges)
			{
				if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
				if (!ValidRange(Range)) return Failure(EShaderError::PayloadPushConstantInvalid, Index);
				Descriptor.WriteU32(uint32(Range.StageFlags)); Descriptor.WriteU32(Range.Offset); Descriptor.WriteU32(Range.Size); Descriptor.WriteU32(0);
			}
			Descriptor.WriteU32(uint32(Shader.MetalBindings.size()));
			for (const auto& Binding : Shader.MetalBindings)
			{
				Descriptor.WriteU32(Binding.SetIndex); Descriptor.WriteU32(Binding.BindingIndex);
				Descriptor.WriteU32(uint32(Binding.Type)); Descriptor.WriteU32(Binding.Slot);
				Descriptor.WriteU32(Binding.Count);
			}
			Descriptor.WriteU32(Shader.MetalPushConstantBufferSlot);
			Descriptor.WriteU64(Shader.BindingRemapIdentity.HashLow);
			Descriptor.WriteU64(Shader.BindingRemapIdentity.HashHigh);
			if (Descriptor.HasError()) return Failure(EShaderError::PayloadTooLarge, Index);
			DataBytes += Shader.Code->size() + Descriptor.Tell();
			Output.AddValue(ValueId(uint32(Index), false), *Shader.Code);
			Output.AddValue(ValueId(uint32(Index), true), FSharedByteBuffer::Take(Descriptor.TakeBytes()));
		}
		if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
		if (Metadata.HasError()) return Failure(EShaderError::PayloadTooLarge);
		auto Meta = MakeBuildMetadata(FSharedByteBuffer::Take(Metadata.TakeBytes()));
		if (!Meta || !Output.AddMeta(FValueId::FromName("Metadata"), std::move(*Meta))) return Failure(EShaderError::PayloadTooLarge);
		auto Built = std::move(Output).Build(); if (!Built) return Failure(EShaderError::PayloadTooLarge);
		return std::move(*Built);
	}

	auto AssembleImpl(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel)
		-> std::expected<FShaderCompilerOutput, FShaderError>;
	auto Validate(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel) -> FShaderOperationResult
	{
		auto Product = AssembleImpl(Options, Output, Cancel); if (!Product) return std::unexpected(std::move(Product.error())); return {};
	}

	auto AssembleImpl(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel)
		-> std::expected<FShaderCompilerOutput, FShaderError>
	{
		auto Entries = ReadLayout(Options, Output, Cancel); if (!Entries) return std::unexpected(std::move(Entries.error()));
		FShaderCompilerOutput Product{.Error = {}}; Product.CompiledShaders.reserve(Entries->size());
		for (size_t Index = 0; Index < Entries->size(); ++Index)
		{
			auto& Entry = (*Entries)[Index];
			FCompiledShader Shader;
			Shader.Target = Options.Target; Shader.CodeFormat = Options.Target.OutputFormat;
			Shader.Frequency = Entry.Frequency; Shader.SourceEntryPoint = std::move(Entry.SourceName);
			Shader.BinaryEntryPoint = std::move(Entry.BinaryName); Shader.Hash = Entry.Hash;
			Shader.DebugName = Options.VirtualShaderPath.empty() ? Shader.SourceEntryPoint : Options.VirtualShaderPath + "::" + Shader.SourceEntryPoint;
			Shader.Code = std::make_shared<const FSharedByteBuffer>(std::move(Entry.Code));
			if (auto Valid = ReadReflection(Entry.Reflection, Index, Shader, Cancel); !Valid) return std::unexpected(std::move(Valid.error()));
			Product.CompiledShaders.push_back(std::move(Shader));
		}
		if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
		return Product;
	}

	auto Assemble(const FShaderCompileOptions& Options, const FBuildOutput& Output,
		const std::function<bool()>& Cancel)
		-> std::expected<FShaderCompilerOutput, FShaderError>
	{ return AssembleImpl(Options, Output, Cancel); }
}
