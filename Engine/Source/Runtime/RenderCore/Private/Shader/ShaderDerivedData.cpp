#include "Shader/ShaderCompiledOutput.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderCompiledOutput
{
	namespace
	{
		constexpr uint32 GSpirvMagic = 0x07230203u;
		constexpr uint64 GMaximumCodeBytes = 64ull * 1024ull * 1024ull;
		constexpr uint32 GMaximumReflectionEntries = 65536;
		constexpr uint32 GMaximumDescriptorIndex = 65535;
		constexpr uint32 GMaximumPushConstantBytes = 65536;
		constexpr uint64 GMaximumStringBytes = 32768;

		template <typename TBuilder>
		auto UpdateString(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
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
			if (Options.EntryPoints.empty()
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

		auto ValidateCode(FByteView Code) -> bool
		{
			if (Code.size() < 5 * sizeof(uint32)
				|| Code.size() > GMaximumCodeBytes
				|| Code.size() % sizeof(uint32) != 0) return false;
			uint32 Magic = 0;
			return ReadLittleEndianAt<uint32>(Code, 0, Magic)
				&& Magic == GSpirvMagic;
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
			return std::unexpected(FShaderError{.Code = EShaderError::PayloadRequestInvalid});

		FBinaryWriter Writer;
		Writer.WriteHeader({PayloadMagic, PayloadSchemaVersion, BuilderVersion});
		Writer.WriteU32(0);
		Writer.WriteU32(static_cast<uint32>(Output.CompiledShaders.size()));
		for (size_t Index = 0; Index < Output.CompiledShaders.size(); ++Index)
		{
			const FCompiledShader& Shader = Output.CompiledShaders[Index];
			if (!Shader.Code || Shader.SourceEntryPoint != EntryPoint(Options.EntryPoints[Index])
				|| Shader.Frequency != Options.Frequencies[Index]
				|| Shader.SourceEntryPoint.empty()
				|| Shader.BinaryEntryPoint.empty()
				|| Shader.SourceEntryPoint.size() > GMaximumStringBytes
				|| Shader.BinaryEntryPoint.size() > GMaximumStringBytes
				|| Shader.DebugName.size() > GMaximumStringBytes
				|| !ValidateCode(*Shader.Code)
				|| FXxHash128::HashBuffer(*Shader.Code) != Shader.Hash
				|| Shader.Reflection.ResourceBindings.size() > GMaximumReflectionEntries
				|| Shader.Reflection.PushConstantRanges.size() > GMaximumReflectionEntries)
				return std::unexpected(FShaderError{.Code = EShaderError::PayloadOutputInvalid, .Index = Index});

			Writer.WriteString(Shader.SourceEntryPoint);
			Writer.WriteString(Shader.BinaryEntryPoint);
			Writer.WriteU32(static_cast<uint32>(Shader.Frequency));
			Writer.WriteU32(0);
			Writer.WriteString(Shader.DebugName);
			Writer.WriteU64(Shader.Hash.HashLow);
			Writer.WriteU64(Shader.Hash.HashHigh);
			Writer.WriteU64(static_cast<uint64>(Shader.Code->size()));
			Writer.WriteBytes(*Shader.Code);
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
					return std::unexpected(FShaderError{.Code = EShaderError::PayloadBindingInvalid,
						.Parameter = Binding.Name,
						.Index = Index,
						.SetIndex = Binding.SetIndex,
						.BindingIndex = Binding.BindingIndex});
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
					return std::unexpected(FShaderError{.Code = EShaderError::PayloadPushConstantInvalid,
						.Index = Index,
						.Actual = Range.Size,
						.NewBegin = Range.Offset});
				Writer.WriteU32(Flags);
				Writer.WriteU32(Range.Offset);
				Writer.WriteU32(Range.Size);
				Writer.WriteU32(0);
			}
		}
		if (Writer.GetBytes().size() > MaximumValueBytes)
			return std::unexpected(FShaderError{.Code = EShaderError::PayloadTooLarge,
				.Expected = MaximumValueBytes,
				.Actual = Writer.GetBytes().size()});
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
			return std::unexpected(FShaderError{.Code = EShaderError::PayloadInputInvalid, .Actual = Bytes.size()});

		FBinaryReader Reader(Bytes);
		uint32 Reserved = 0;
		uint32 EntryCount = 0;
		if (!Reader.ReadAndValidateHeader(
			PayloadMagic, PayloadSchemaVersion, BuilderVersion)
			|| !Reader.ReadU32(Reserved) || Reserved != 0
			|| !Reader.ReadU32(EntryCount)
			|| EntryCount != Options.EntryPoints.size()
			|| EntryCount > MaximumEntryPoints)
			return std::unexpected(FShaderError{.Code = EShaderError::PayloadHeaderInvalid});

		FShaderCompilerOutput Candidate;
		Candidate.CompiledShaders.reserve(EntryCount);
		for (uint32 Index = 0; Index < EntryCount; ++Index)
		{
			FCompiledShader Shader;
			uint32 Frequency = 0;
			uint64 HashLow = 0;
			uint64 HashHigh = 0;
			uint64 CodeBytes = 0;
			if (!Reader.ReadString(Shader.SourceEntryPoint, GMaximumStringBytes)
				|| !Reader.ReadString(Shader.BinaryEntryPoint, GMaximumStringBytes)
				|| !Reader.ReadU32(Frequency) || !Reader.ReadU32(Reserved)
				|| Reserved != 0
				|| !Reader.ReadString(Shader.DebugName, GMaximumStringBytes)
				|| !Reader.ReadU64(HashLow) || !Reader.ReadU64(HashHigh)
				|| !Reader.ReadU64(CodeBytes)
				|| !IsValidFrequency(Frequency)
				|| Frequency != static_cast<uint32>(Options.Frequencies[Index])
				|| Shader.SourceEntryPoint != EntryPoint(Options.EntryPoints[Index])
				|| Shader.BinaryEntryPoint.empty())
				return std::unexpected(FShaderError{.Code = EShaderError::PayloadEntryInvalid, .Index = Index});
			FByteBuffer Code;
			if (!Reader.ReadBytes(Code, CodeBytes, GMaximumCodeBytes)
				|| !ValidateCode(Code))
				return std::unexpected(FShaderError{.Code = EShaderError::PayloadSpirvInvalid, .Index = Index});
			Shader.Frequency = static_cast<EShaderFrequency>(Frequency);
			Shader.Hash = {HashLow, HashHigh};
			if (FXxHash128::HashBuffer(Code) != Shader.Hash)
				return std::unexpected(FShaderError{.Code = EShaderError::PayloadSpirvHashMismatch, .Index = Index});
			Shader.Code = std::make_shared<FByteBuffer>(std::move(Code));

			uint32 BindingCount = 0;
			if (!Reader.ReadU32(BindingCount)
				|| BindingCount > GMaximumReflectionEntries)
				return std::unexpected(FShaderError{.Code = EShaderError::PayloadBindingCountInvalid,
					.Index = Index,
					.Expected = GMaximumReflectionEntries,
					.Actual = BindingCount});
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
					return std::unexpected(FShaderError{.Code = EShaderError::PayloadBindingInvalid,
						.Index = Index,
						.ElementIndex = BindingIndex,
						.SetIndex = Binding.SetIndex,
						.BindingIndex = Binding.BindingIndex});
				Binding.StageFlags = static_cast<EShaderStageFlags>(Flags);
				Binding.Type = static_cast<ERHIBindingType>(Type);
				Shader.Reflection.ResourceBindings.push_back(std::move(Binding));
			}

			uint32 RangeCount = 0;
			if (!Reader.ReadU32(RangeCount)
				|| RangeCount > GMaximumReflectionEntries)
				return std::unexpected(FShaderError{.Code = EShaderError::PayloadPushConstantCountInvalid,
					.Index = Index,
					.Expected = GMaximumReflectionEntries,
					.Actual = RangeCount});
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
					return std::unexpected(FShaderError{.Code = EShaderError::PayloadPushConstantInvalid,
						.Index = Index,
						.ElementIndex = RangeIndex});
				Range.StageFlags = static_cast<EShaderStageFlags>(Flags);
				Shader.Reflection.PushConstantRanges.push_back(Range);
			}
			Candidate.CompiledShaders.push_back(std::move(Shader));
		}
		if (!Reader.IsAtEnd())
			return std::unexpected(FShaderError{.Code = EShaderError::PayloadTrailingBytes});
		Candidate.Error = {};
		OutOutput = std::move(Candidate);
		return {};
	}
}
