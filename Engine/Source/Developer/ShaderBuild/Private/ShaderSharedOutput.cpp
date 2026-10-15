#include "ShaderFormat.h"
#include "ShaderSharedOutput.h"
#include "ShaderBuildSession.h"
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
		constexpr uint64 MaximumMetadataBytes = 4 + 8 + MaximumStringBytes + 4 * 4 + 16
			+ 32 * (2 * (8 + MaximumStringBytes) + 8 + 16 + 12);
		auto WriteTarget(FBinaryWriter& Writer, const FShaderTargetIdentity& Target) -> void
		{
			Writer.WriteU32(uint32(Target.Platform)); Writer.WriteU32(uint32(Target.Backend));
			Writer.WriteU32(uint32(Target.IntermediateFormat)); Writer.WriteU32(uint32(Target.OutputFormat));
			Writer.WriteU64(Target.SettingsDigest.HashLow); Writer.WriteU64(Target.SettingsDigest.HashHigh);
		}
		auto ReadTarget(FBinaryReader& Reader, FShaderTargetIdentity& Target) -> bool
		{
			uint32 Platform = 0, Backend = 0, Intermediate = 0, Output = 0;
			if (!Reader.ReadU32(Platform) || !Reader.ReadU32(Backend)
				|| !Reader.ReadU32(Intermediate) || !Reader.ReadU32(Output)
				|| !Reader.ReadU64(Target.SettingsDigest.HashLow)
				|| !Reader.ReadU64(Target.SettingsDigest.HashHigh)) return false;
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
		auto ReadReflection(const FSharedByteBuffer& Block, size_t Entry, FShaderCode& Shader) -> FShaderOperationResult
		{
			FBinaryReader Reader(Block.GetBytes(), {.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
			uint32 Count = 0;
			if (!Reader.ReadU32(Count) || Count > MaximumReflectionEntries) return Failure(EShaderError::PayloadBindingCountInvalid, Entry);
			Shader.Reflection.ResourceBindings.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
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
				FPushConstantRange Range{}; uint32 Flags = 0, Reserved = 0;
				if (!Reader.ReadU32(Flags) || !Reader.ReadU32(Range.Offset) || !Reader.ReadU32(Range.Size)
					|| !Reader.ReadU32(Reserved) || Reserved || !ValidFlags(Flags))
					return Failure(EShaderError::PayloadPushConstantInvalid, Entry);
				Range.StageFlags = EShaderStageFlags(Flags);
				if (!ValidRange(Range)) return Failure(EShaderError::PayloadPushConstantInvalid, Entry);
				Shader.Reflection.PushConstantRanges.push_back(Range);
			}
			uint64 Size = 0; FByteBuffer Bytes; FXxHash128 Identity{};
			if (!Reader.ReadU32(Shader.PlatformDataVersion) || !Reader.ReadU64(Size)
				|| !Reader.ReadBytes(Bytes, Size, ShaderFormat::MaximumMetadataBytes)
				|| !Reader.ReadU64(Identity.HashLow) || !Reader.ReadU64(Identity.HashHigh))
				return Failure(EShaderError::PayloadBindingInvalid, Entry);
			Shader.PlatformData = FSharedByteBuffer::Take(std::move(Bytes));
			const auto IdentityValue = ShaderFormat::ComputeShaderIdentity(Shader);
			if (!ShaderFormat::ValidateShaderCode(Shader) || IdentityValue != Identity) return Failure(EShaderError::PayloadBindingInvalid, Entry);
			if (!Reader.IsAtEnd()) return Failure(EShaderError::PayloadTrailingBytes, Entry);
			return {};
		}
		struct FEntry
		{
			std::string SourceName, BinaryName;
			EShaderFrequency Frequency;
			std::array<uint32, 3> ComputeThreadGroupSize{};
			FXxHash128 Hash;
			FSharedByteBuffer Code, Reflection;
		};
		auto ValueId(uint32 Index, bool Reflection) -> FValueId
		{ return FValueId::FromName("Durin.Shader.EntryValue").MakeIndexed(Index * 2 + uint32(Reflection)); }
		auto ReadLayout(const FShaderCompileOptions& Options, const FBuildOutput& Output) -> std::expected<std::vector<FEntry>, FShaderError>
		{
			if (!ValidRequest(Options)) return Failure(EShaderError::PayloadRequestInvalid);
			const auto Metadata = GetBuildMetadataPayload(Output);
			if (Output.GetSchema() != "Shader.Output" || Output.GetSchemaVersion() != ShaderCompileOutputSchemaVersion
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
			FEntry Entry; uint32 Frequency = 0, Format = 0;
				if (!Reader.ReadString(Entry.SourceName, MaximumStringBytes) || !Reader.ReadString(Entry.BinaryName, MaximumStringBytes)
					|| !Reader.ReadU32(Frequency) || !Reader.ReadU32(Format)
					|| !Reader.ReadU32(Entry.ComputeThreadGroupSize[0])
					|| !Reader.ReadU32(Entry.ComputeThreadGroupSize[1])
					|| !Reader.ReadU32(Entry.ComputeThreadGroupSize[2])
					|| !Reader.ReadU64(Entry.Hash.HashLow) || !Reader.ReadU64(Entry.Hash.HashHigh)
					|| Entry.SourceName != Options.EntryPoints[Index] || Entry.BinaryName.empty()
					|| Frequency != uint32(Options.Frequencies[Index])
					|| Format != uint32(Target.OutputFormat)
					|| !ShaderFormat::ValidateExecutionMetadata(Target, EShaderFrequency(Frequency), Entry.ComputeThreadGroupSize))
					return Failure(EShaderError::PayloadEntryInvalid, Index);
				Entry.Frequency = EShaderFrequency(Frequency);
				const auto* Code = Output.FindValue(ValueId(Index, false));
				const auto* Reflection = Output.FindValue(ValueId(Index, true));
				if (!Code || !Reflection) return Failure(EShaderError::PayloadEntryInvalid, Index);
				if (!ShaderFormat::ValidateCode(Target, Target.OutputFormat, Code->GetData().GetBytes()))
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
		-> TShaderValueResult<FBuildOutput>
	{
		if (Cancel && Cancel()) return FShaderCompletion::Canceled();
		if (!Product || !ValidRequest(Options) || Product.CompiledShaders.size() != Options.EntryPoints.size())
			return Failure(EShaderError::PayloadRequestInvalid);
		FBuildOutputBuilder Output("Shader.Output", ShaderCompileOutputSchemaVersion, {.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
		uint64 DataBytes = 0;
		FBinaryWriter Metadata({.MaximumTotalBytes = MaximumMetadataBytes});
		Metadata.WriteU32(uint32(Product.CompiledShaders.size())); Metadata.WriteString(Options.VirtualShaderPath);
		WriteTarget(Metadata, Options.Target);
		for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
		{
			if (Cancel && Cancel()) return FShaderCompletion::Canceled();
			const auto& Shader = Product.CompiledShaders[Index];
			if (Shader.Target != Options.Target || Shader.CodeFormat != Options.Target.OutputFormat
				|| Shader.SourceEntryPoint != Options.EntryPoints[Index] || Shader.Frequency != Options.Frequencies[Index]
				|| Shader.SourceEntryPoint.size() > MaximumStringBytes || Shader.BinaryEntryPoint.empty() || Shader.BinaryEntryPoint.size() > MaximumStringBytes
				|| !ShaderFormat::ValidateShaderCode(Shader))
				return Failure(EShaderError::PayloadOutputInvalid, Index);
			Metadata.WriteString(Shader.SourceEntryPoint); Metadata.WriteString(Shader.BinaryEntryPoint);
			Metadata.WriteU32(uint32(Shader.Frequency)); Metadata.WriteU32(uint32(Shader.CodeFormat));
			for (uint32 Axis : Shader.ComputeThreadGroupSize) Metadata.WriteU32(Axis);
			Metadata.WriteU64(Shader.CodeHash.HashLow); Metadata.WriteU64(Shader.CodeHash.HashHigh);
			const auto& Reflection = Shader.Reflection;
			if (Reflection.ResourceBindings.size() > MaximumReflectionEntries || Reflection.PushConstantRanges.size() > MaximumReflectionEntries)
				return Failure(EShaderError::PayloadOutputInvalid, Index);
			if (Shader.Code.size() > ShaderCompiledOutput::MaximumValueBytes - DataBytes
				|| Metadata.Tell() > ShaderCompiledOutput::MaximumValueBytes - DataBytes - Shader.Code.size())
				return Failure(EShaderError::PayloadTooLarge, Index);
			FBinaryWriter Descriptor({.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes - DataBytes - Shader.Code.size() - Metadata.Tell()});
			Descriptor.WriteU32(uint32(Reflection.ResourceBindings.size()));
			for (const auto& Binding : Reflection.ResourceBindings)
			{
				if (Cancel && Cancel()) return FShaderCompletion::Canceled();
				if (!ValidBinding(Binding)) return Failure(EShaderError::PayloadBindingInvalid, Index);
				Descriptor.WriteString(Binding.Name); Descriptor.WriteU32(uint32(Binding.StageFlags));
				Descriptor.WriteU32(Binding.SetIndex); Descriptor.WriteU32(Binding.BindingIndex);
				Descriptor.WriteU32(uint32(Binding.Type)); Descriptor.WriteU32(Binding.ArraySize);
			}
			Descriptor.WriteU32(uint32(Reflection.PushConstantRanges.size()));
			for (const auto& Range : Reflection.PushConstantRanges)
			{
				if (Cancel && Cancel()) return FShaderCompletion::Canceled();
				if (!ValidRange(Range)) return Failure(EShaderError::PayloadPushConstantInvalid, Index);
				Descriptor.WriteU32(uint32(Range.StageFlags)); Descriptor.WriteU32(Range.Offset); Descriptor.WriteU32(Range.Size); Descriptor.WriteU32(0);
			}
			Descriptor.WriteU32(Shader.PlatformDataVersion); Descriptor.WriteU64(Shader.PlatformData.GetSize());
			Descriptor.WriteBytes(Shader.PlatformData.GetBytes());
			const auto IdentityValue = ShaderFormat::ComputeShaderIdentity(Shader);
			Descriptor.WriteU64(IdentityValue.HashLow); Descriptor.WriteU64(IdentityValue.HashHigh);
			if (Descriptor.HasError()) return Failure(EShaderError::PayloadTooLarge, Index);
			DataBytes += Shader.Code.size() + Descriptor.Tell();
			Output.AddValue(ValueId(uint32(Index), false), Shader.Code);
			Output.AddValue(ValueId(uint32(Index), true), FSharedByteBuffer::Take(Descriptor.TakeBytes()));
		}
		if (Cancel && Cancel()) return FShaderCompletion::Canceled();
		if (Metadata.HasError()) return Failure(EShaderError::PayloadTooLarge);
		auto Meta = MakeBuildMetadata(FSharedByteBuffer::Take(Metadata.TakeBytes()));
		if (!Meta || !Output.AddMeta(FValueId::FromName("Metadata"), std::move(*Meta))) return Failure(EShaderError::PayloadTooLarge);
		auto Built = std::move(Output).Build(); if (!Built) return Failure(EShaderError::PayloadTooLarge);
		return std::move(*Built);
	}

	auto AssembleImpl(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel)
		-> TShaderValueResult<FShaderCompilerOutput>;
	auto Validate(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel) -> FShaderCompletion
	{
		auto Product = AssembleImpl(Options, Output, Cancel); if (!Product) return FShaderCompletion(Product); return {};
	}

	auto AssembleImpl(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel)
		-> TShaderValueResult<FShaderCompilerOutput>
	{
		if (Cancel && Cancel()) return FShaderCompletion::Canceled();
		auto Entries = ReadLayout(Options, Output); if (!Entries) return std::unexpected(Entries.error());
		FShaderCompilerOutput Product{.Error = {}}; Product.CompiledShaders.reserve(Entries->size());
		for (size_t Index = 0; Index < Entries->size(); ++Index)
		{
			if (Cancel && Cancel()) return FShaderCompletion::Canceled();
			auto& Entry = (*Entries)[Index];
			FShaderCode Shader;
			Shader.Target = Options.Target; Shader.CodeFormat = Options.Target.OutputFormat;
			Shader.Frequency = Entry.Frequency; Shader.SourceEntryPoint = std::move(Entry.SourceName);
			Shader.ComputeThreadGroupSize = Entry.ComputeThreadGroupSize;
			Shader.BinaryEntryPoint = std::move(Entry.BinaryName); Shader.CodeHash = Entry.Hash;
			Shader.DebugName = Options.VirtualShaderPath.empty() ? Shader.SourceEntryPoint : Options.VirtualShaderPath + "::" + Shader.SourceEntryPoint;
			Shader.Code = std::move(Entry.Code);
			if (auto Valid = ReadReflection(Entry.Reflection, Index, Shader); !Valid) return std::unexpected(Valid.error());
			Product.CompiledShaders.push_back(std::move(Shader));
		}
		if (Cancel && Cancel()) return FShaderCompletion::Canceled();
		return Product;
	}

	auto Assemble(const FShaderCompileOptions& Options, const FBuildOutput& Output,
		const std::function<bool()>& Cancel)
		-> TShaderValueResult<FShaderCompilerOutput>
	{ return AssembleImpl(Options, Output, Cancel); }
}
