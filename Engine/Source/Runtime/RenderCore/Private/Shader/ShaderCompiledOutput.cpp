#include "ShaderFormat.h"
#include "Shader/ShaderCompiledOutput.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderCompiledOutput
{
	namespace
	{
		constexpr uint64 GMaximumCodeBytes = 64ull * 1024ull * 1024ull;
		constexpr uint32 GMaximumReflectionEntries = 65536;
		constexpr uint32 GMaximumDescriptorIndex = 65535;
		constexpr uint32 GMaximumPushConstantBytes = 65536;
		constexpr uint64 GMaximumStringBytes = 32768;
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

		auto EntryPoint(const char8* Value) -> std::string_view
		{
			return Value ? std::string_view(Value) : std::string_view{};
		}

		auto IsValidFrequency(uint32 Value) -> bool
		{
			return Value <= static_cast<uint32>(EShaderFrequency::RayMiss);
		}

		auto IsValidStageFlags(uint32 Value) -> bool
		{
			constexpr uint32 Mask = static_cast<uint32>(EShaderStageFlags::Vertex)
				| static_cast<uint32>(EShaderStageFlags::Fragment)
				| static_cast<uint32>(EShaderStageFlags::Compute)
				| static_cast<uint32>(EShaderStageFlags::Geometry);
			return (Value & ~Mask) == 0;
		}

		auto IsValidBindingType(uint32 Value) -> bool
		{
			return Value <= static_cast<uint32>(ERHIBindingType::StorageImage);
		}

		auto IsValidRequest(const FShaderCompileOptions& Options) -> bool
		{
			if ((Options.Target != VulkanShaderTarget && !IsMetalShaderTarget(Options.Target))
				|| Options.EntryPoints.empty()
				|| Options.EntryPoints.size() != Options.Frequencies.size()
				|| Options.EntryPoints.size() > MaximumEntryPoints) return false;
			std::set<std::pair<std::string_view, uint32>> Entries;
			for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
			{
				const std::string_view Name = EntryPoint(Options.EntryPoints[Index]);
				const uint32 Frequency =
					static_cast<uint32>(Options.Frequencies[Index]);
				if (Name.empty() || Name.size() > GMaximumStringBytes
					|| !IsValidFrequency(Frequency)
					|| !Entries.emplace(Name, Frequency).second) return false;
			}
			return true;
		}

	}

	auto Encode(
		const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output,
		FByteBuffer& OutBytes) -> FShaderOperationResult
	{
		OutBytes.clear();
		if (!Output || !IsValidRequest(Options)
			|| Output.CompiledShaders.size() != Options.EntryPoints.size())
			return std::unexpected(std::string("Shader payload request or output count is invalid."));

		FBinaryWriter Writer;
		Writer.WriteU32(PayloadMagic);
		Writer.WriteU32(PayloadSchemaVersion);
		Writer.WriteU32(BinaryFormatMarker);
		Writer.WriteU32(0);
		WriteTarget(Writer, Options.Target);
		Writer.WriteU32(static_cast<uint32>(Output.CompiledShaders.size()));
		for (size_t Index = 0; Index < Output.CompiledShaders.size(); ++Index)
		{
			const FShaderCode& Shader = Output.CompiledShaders[Index];
			if (Shader.Code.IsEmpty() || Shader.Target != Options.Target || Shader.CodeFormat != Options.Target.OutputFormat
				|| Shader.SourceEntryPoint != EntryPoint(Options.EntryPoints[Index])
				|| Shader.Frequency != Options.Frequencies[Index]
				|| Shader.SourceEntryPoint.empty()
				|| Shader.BinaryEntryPoint.empty()
				|| Shader.SourceEntryPoint.size() > GMaximumStringBytes
				|| Shader.BinaryEntryPoint.size() > GMaximumStringBytes
				|| Shader.DebugName.size() > GMaximumStringBytes
				|| !ShaderFormat::ValidateCode(Options.Target, Options.Target.OutputFormat, Shader.Code)
				|| !ShaderFormat::ValidateCodeHash(Shader.Code, Shader.CodeHash)
				|| Shader.Reflection.ResourceBindings.size() > GMaximumReflectionEntries
				|| Shader.Reflection.PushConstantRanges.size() > GMaximumReflectionEntries
				|| !ShaderFormat::ValidateExecutionMetadata(Options.Target, EShaderFrequency(Shader.Frequency), Shader.ComputeThreadGroupSize))
				return std::unexpected(std::string("Shader payload contains invalid compiled output."));

			Writer.WriteString(Shader.SourceEntryPoint);
			Writer.WriteString(Shader.BinaryEntryPoint);
			Writer.WriteU32(static_cast<uint32>(Shader.Frequency));
			Writer.WriteU32(uint32(Shader.CodeFormat));
			for (uint32 Axis : Shader.ComputeThreadGroupSize) Writer.WriteU32(Axis);
			Writer.WriteString(Shader.DebugName);
			Writer.WriteU64(Shader.CodeHash.HashLow);
			Writer.WriteU64(Shader.CodeHash.HashHigh);
			Writer.WriteU64(static_cast<uint64>(Shader.Code.size()));
			Writer.WriteBytes(Shader.Code);
			Writer.WriteU32(static_cast<uint32>(
				Shader.Reflection.ResourceBindings.size()));
			for (const FShaderResourceBinding& Binding
				: Shader.Reflection.ResourceBindings)
			{
				const uint32 Flags = static_cast<uint32>(Binding.StageFlags);
				const uint32 Type = static_cast<uint32>(Binding.Type);
				if (Binding.Name.size() > GMaximumStringBytes
					|| !IsValidStageFlags(Flags)
					|| Binding.SetIndex > GMaximumDescriptorIndex
					|| Binding.BindingIndex > GMaximumDescriptorIndex
					|| !IsValidBindingType(Type) || Binding.ArraySize == 0
					|| Binding.ArraySize > GMaximumReflectionEntries)
					return std::unexpected(std::string("Shader payload resource reflection is invalid."));
				Writer.WriteString(Binding.Name);
				Writer.WriteU32(Flags);
				Writer.WriteU32(Binding.SetIndex);
				Writer.WriteU32(Binding.BindingIndex);
				Writer.WriteU32(Type);
				Writer.WriteU32(Binding.ArraySize);
			}
			Writer.WriteU32(static_cast<uint32>(
				Shader.Reflection.PushConstantRanges.size()));
			for (const FPushConstantRange& Range
				: Shader.Reflection.PushConstantRanges)
			{
				const uint32 Flags = static_cast<uint32>(Range.StageFlags);
				if (!IsValidStageFlags(Flags) || Range.Size == 0
					|| Range.Offset > GMaximumPushConstantBytes
					|| Range.Size > GMaximumPushConstantBytes
					|| Range.Offset > GMaximumPushConstantBytes - Range.Size)
					return std::unexpected(std::string("Shader payload push constants are invalid."));
				Writer.WriteU32(Flags);
				Writer.WriteU32(Range.Offset);
				Writer.WriteU32(Range.Size);
				Writer.WriteU32(0);
			}
			if (!ShaderFormat::ValidateShaderCode(Shader))
				return std::unexpected(std::string("Shader payload resource reflection is invalid."));
			Writer.WriteU32(Shader.PlatformDataVersion);
			Writer.WriteU64(Shader.PlatformData.GetSize());
			Writer.WriteBytes(Shader.PlatformData.GetBytes());
			const auto IdentityValue = ShaderFormat::ComputeShaderIdentity(Shader);
			Writer.WriteU64(IdentityValue.HashLow); Writer.WriteU64(IdentityValue.HashHigh);
		}
		if (Writer.GetBytes().size() > MaximumValueBytes)
			return std::unexpected(std::string("Shader payload exceeds its DDC value limit."));
		OutBytes = Writer.TakeBytes();
		return {};
	}

	auto Decode(
		FByteView Bytes,
		const FShaderCompileOptions& Options,
		FShaderCompilerOutput& OutOutput) -> FShaderOperationResult
	{
		OutOutput = {};
		if (Bytes.empty() || Bytes.size() > MaximumValueBytes
			|| !IsValidRequest(Options))
			return std::unexpected(std::string("Shader payload request or byte count is invalid."));

		FBinaryReader Reader(Bytes);
		uint32 Reserved = 0;
		uint32 EntryCount = 0;
		FShaderTargetIdentity Target;
		uint32 Magic = 0, Schema = 0, Marker = 0;
		if (!Reader.ReadU32(Magic) || !Reader.ReadU32(Schema)
			|| !Reader.ReadU32(Marker)
			|| Magic != PayloadMagic || Schema != PayloadSchemaVersion
			|| Marker != BinaryFormatMarker
			|| !Reader.ReadU32(Reserved) || Reserved != 0
			|| !ReadTarget(Reader, Target) || Target != Options.Target
			|| !Reader.ReadU32(EntryCount)
			|| EntryCount != Options.EntryPoints.size()
			|| EntryCount > MaximumEntryPoints)
			return std::unexpected(std::string("Shader payload header is incompatible or malformed."));

		FShaderCompilerOutput Candidate;
		Candidate.CompiledShaders.reserve(EntryCount);
		for (uint32 Index = 0; Index < EntryCount; ++Index)
		{
			FShaderCode Shader;
			uint32 Frequency = 0;
			uint32 Format = 0;
			uint64 HashLow = 0;
			uint64 HashHigh = 0;
			uint64 CodeBytes = 0;
			if (!Reader.ReadString(Shader.SourceEntryPoint, GMaximumStringBytes)
				|| !Reader.ReadString(Shader.BinaryEntryPoint, GMaximumStringBytes)
				|| !Reader.ReadU32(Frequency) || !Reader.ReadU32(Format)
				|| !Reader.ReadU32(Shader.ComputeThreadGroupSize[0])
				|| !Reader.ReadU32(Shader.ComputeThreadGroupSize[1])
				|| !Reader.ReadU32(Shader.ComputeThreadGroupSize[2])
				|| Format != uint32(Target.OutputFormat)
				|| !Reader.ReadString(Shader.DebugName, GMaximumStringBytes)
				|| !Reader.ReadU64(HashLow) || !Reader.ReadU64(HashHigh)
				|| !Reader.ReadU64(CodeBytes)
				|| !IsValidFrequency(Frequency)
				|| Frequency != static_cast<uint32>(Options.Frequencies[Index])
				|| !ShaderFormat::ValidateExecutionMetadata(Target, EShaderFrequency(Frequency), Shader.ComputeThreadGroupSize)
				|| Shader.SourceEntryPoint != EntryPoint(Options.EntryPoints[Index])
				|| Shader.BinaryEntryPoint.empty())
				return std::unexpected(std::string("Shader payload entry identity is invalid."));
			FByteBuffer Code;
			if (!Reader.ReadBytes(Code, CodeBytes, GMaximumCodeBytes)
				|| !ShaderFormat::ValidateCode(Target, Target.OutputFormat, Code))
				return std::unexpected((IsMetalShaderTarget(Target) ? std::string("Shader payload Metal code is invalid.") : std::string("Shader payload SPIR-V is invalid.")));
			Shader.Frequency = static_cast<EShaderFrequency>(Frequency);
			Shader.Target = Target;
			Shader.CodeFormat = EShaderCodeFormat(Format);
			Shader.CodeHash = {HashLow, HashHigh};
			if (!ShaderFormat::ValidateCodeHash(Code, Shader.CodeHash))
				return std::unexpected((IsMetalShaderTarget(Target) ? std::string("Shader payload Metal code hash is invalid.") : std::string("Shader payload SPIR-V hash is invalid.")));
			Shader.Code = FSharedByteBuffer::Take(std::move(Code));

			uint32 BindingCount = 0;
			if (!Reader.ReadU32(BindingCount)
				|| BindingCount > GMaximumReflectionEntries)
				return std::unexpected(std::string("Shader payload binding count is invalid."));
			Shader.Reflection.ResourceBindings.reserve(BindingCount);
			for (uint32 BindingIndex = 0; BindingIndex < BindingCount;
				++BindingIndex)
			{
				FShaderResourceBinding Binding;
				uint32 Flags = 0;
				uint32 Type = 0;
				if (!Reader.ReadString(Binding.Name, GMaximumStringBytes)
					|| !Reader.ReadU32(Flags)
					|| !Reader.ReadU32(Binding.SetIndex)
					|| !Reader.ReadU32(Binding.BindingIndex)
					|| !Reader.ReadU32(Type)
					|| !Reader.ReadU32(Binding.ArraySize)
					|| !IsValidStageFlags(Flags)
					|| Binding.SetIndex > GMaximumDescriptorIndex
					|| Binding.BindingIndex > GMaximumDescriptorIndex
					|| !IsValidBindingType(Type) || Binding.ArraySize == 0
					|| Binding.ArraySize > GMaximumReflectionEntries)
					return std::unexpected(std::string("Shader payload resource reflection is invalid."));
				Binding.StageFlags = static_cast<EShaderStageFlags>(Flags);
				Binding.Type = static_cast<ERHIBindingType>(Type);
				Shader.Reflection.ResourceBindings.push_back(std::move(Binding));
			}

			uint32 RangeCount = 0;
			if (!Reader.ReadU32(RangeCount)
				|| RangeCount > GMaximumReflectionEntries)
				return std::unexpected(std::string("Shader payload push-constant count is invalid."));
			Shader.Reflection.PushConstantRanges.reserve(RangeCount);
			for (uint32 RangeIndex = 0; RangeIndex < RangeCount; ++RangeIndex)
			{
				FPushConstantRange Range{};
				uint32 Flags = 0;
				if (!Reader.ReadU32(Flags) || !Reader.ReadU32(Range.Offset)
					|| !Reader.ReadU32(Range.Size)
					|| !Reader.ReadU32(Reserved) || Reserved != 0
					|| !IsValidStageFlags(Flags) || Range.Size == 0
					|| Range.Offset > GMaximumPushConstantBytes
					|| Range.Size > GMaximumPushConstantBytes
					|| Range.Offset > GMaximumPushConstantBytes - Range.Size)
					return std::unexpected(std::string("Shader payload push constants are invalid."));
				Range.StageFlags = static_cast<EShaderStageFlags>(Flags);
				Shader.Reflection.PushConstantRanges.push_back(Range);
			}
			uint64 MetadataSize = 0; FByteBuffer Metadata; FXxHash128 ShaderIdentity{};
			if (!Reader.ReadU32(Shader.PlatformDataVersion) || !Reader.ReadU64(MetadataSize)
				|| !Reader.ReadBytes(Metadata, MetadataSize, ShaderFormat::MaximumMetadataBytes)
				|| !Reader.ReadU64(ShaderIdentity.HashLow) || !Reader.ReadU64(ShaderIdentity.HashHigh))
				return std::unexpected(std::string("Shader payload resource reflection is invalid."));
			Shader.PlatformData = FSharedByteBuffer::Take(std::move(Metadata));
			const auto IdentityValue = ShaderFormat::ComputeShaderIdentity(Shader);
			if (!ShaderFormat::ValidateShaderCode(Shader) || IdentityValue != ShaderIdentity)
				return std::unexpected(std::string("Shader payload resource reflection is invalid."));
			Candidate.CompiledShaders.push_back(std::move(Shader));
		}
		if (!Reader.IsAtEnd())
			return std::unexpected(std::string("Shader payload contains trailing bytes."));
		Candidate.Status = EShaderBuildStatus::Ok;
		OutOutput = std::move(Candidate);
		return {};
	}
}
