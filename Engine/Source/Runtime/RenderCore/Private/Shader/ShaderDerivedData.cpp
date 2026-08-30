#include "ShaderDerivedData.h"

#include "Hash/XxHash.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ShaderDerivedData
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

		auto ValidateCode(std::span<const std::byte> Code) -> bool
		{
			if (Code.size() < 5 * sizeof(uint32)
				|| Code.size() > GMaximumCodeBytes
				|| Code.size() % sizeof(uint32) != 0) return false;
			uint32 Magic = 0;
			return ReadLittleEndianAt<uint32>(Code, 0, Magic)
				&& Magic == GSpirvMagic;
		}

		auto Fail(std::string& OutError, std::string_view Message) -> bool
		{
			OutError = Message;
			return false;
		}
	}

	auto GetBucket() -> DerivedData::FCacheBucket
	{
		return DerivedData::FCacheBucket::FromString("Shaders/CompiledOutput");
	}

	auto BuildKey(const FShaderVariantKey& VariantKey,
		const FShaderCompileOptions& Options) -> DerivedData::FCacheKey
	{
		if (VariantKey.Value.IsZero() || !IsValidRequest(Options)) return {};
		FXxHash128Builder Builder;
		UpdateString(Builder, "DurinShaderCompiledOutputKey");
		Builder.UpdateValue(PayloadSchemaVersion);
		Builder.UpdateValue(BuilderVersion);
		Builder.UpdateValue(VariantKey.Value);
		Builder.UpdateValue(static_cast<uint32>(Options.EntryPoints.size()));
		for (size_t Index = 0; Index < Options.EntryPoints.size(); ++Index)
		{
			UpdateString(Builder, EntryPoint(Options.EntryPoints[Index]));
			Builder.UpdateValue(static_cast<uint32>(Options.Frequencies[Index]));
		}
		return DerivedData::FCacheKey::FromString(
			Builder.Finalize().ToString());
	}

	auto Encode(const FShaderCompileOptions& Options,
		const FShaderCompilerOutput& Output, std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		OutError.clear();
		if (!Output || !IsValidRequest(Options)
			|| Output.CompiledShaders.size() != Options.EntryPoints.size())
			return Fail(OutError, "Shader payload request or output count is invalid.");

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
				return Fail(OutError, "Shader payload contains invalid compiled output.");

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
					return Fail(OutError, "Shader payload contains invalid resource reflection.");
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
					return Fail(OutError, "Shader payload contains invalid push constants.");
				Writer.WriteU32(Flags);
				Writer.WriteU32(Range.Offset);
				Writer.WriteU32(Range.Size);
				Writer.WriteU32(0);
			}
		}
		if (Writer.GetBytes().size() > MaximumValueBytes)
			return Fail(OutError, "Shader payload exceeds its DDC value limit.");
		OutBytes = Writer.TakeBytes();
		return true;
	}

	auto Decode(std::span<const std::byte> Bytes,
		const FShaderCompileOptions& Options, FShaderCompilerOutput& OutOutput,
		std::string& OutError) -> bool
	{
		OutOutput = {};
		OutError.clear();
		if (Bytes.empty() || Bytes.size() > MaximumValueBytes
			|| !IsValidRequest(Options))
			return Fail(OutError, "Shader payload request or byte count is invalid.");

		FBinaryReader Reader(Bytes);
		uint32 Reserved = 0;
		uint32 EntryCount = 0;
		if (!Reader.ReadAndValidateHeader(
			PayloadMagic, PayloadSchemaVersion, BuilderVersion)
			|| !Reader.ReadU32(Reserved) || Reserved != 0
			|| !Reader.ReadU32(EntryCount)
			|| EntryCount != Options.EntryPoints.size()
			|| EntryCount > MaximumEntryPoints)
			return Fail(OutError, "Shader payload header is incompatible or malformed.");

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
				return Fail(OutError, "Shader payload entry identity is invalid.");
			std::vector<std::byte> Code;
			if (!Reader.ReadBytes(Code, CodeBytes, GMaximumCodeBytes)
				|| !ValidateCode(Code))
				return Fail(OutError, "Shader payload SPIR-V is invalid.");
			Shader.Frequency = static_cast<EShaderFrequency>(Frequency);
			Shader.Hash = {HashLow, HashHigh};
			if (FXxHash128::HashBuffer(Code) != Shader.Hash)
				return Fail(OutError, "Shader payload SPIR-V hash is invalid.");
			Shader.Code = std::make_shared<std::vector<std::byte>>(std::move(Code));

			uint32 BindingCount = 0;
			if (!Reader.ReadU32(BindingCount)
				|| BindingCount > GMaximumReflectionEntries)
				return Fail(OutError, "Shader payload binding count is invalid.");
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
					return Fail(OutError, "Shader payload resource reflection is invalid.");
				Binding.StageFlags = static_cast<EShaderStageFlags>(Flags);
				Binding.Type = static_cast<ERHIBindingType>(Type);
				Shader.Reflection.ResourceBindings.push_back(std::move(Binding));
			}

			uint32 RangeCount = 0;
			if (!Reader.ReadU32(RangeCount)
				|| RangeCount > GMaximumReflectionEntries)
				return Fail(OutError, "Shader payload push-constant count is invalid.");
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
					return Fail(OutError, "Shader payload push constants are invalid.");
				Range.StageFlags = static_cast<EShaderStageFlags>(Flags);
				Shader.Reflection.PushConstantRanges.push_back(Range);
			}
			Candidate.CompiledShaders.push_back(std::move(Shader));
		}
		if (!Reader.IsAtEnd())
			return Fail(OutError, "Shader payload contains trailing bytes.");
		Candidate.bSucceeded = true;
		OutOutput = std::move(Candidate);
		return true;
	}
}
