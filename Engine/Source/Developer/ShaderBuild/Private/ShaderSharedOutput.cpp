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
		constexpr uint64 MaximumMetadataBytes = 4 + 32 * (2 * (8 + MaximumStringBytes) + 4 + 16);
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
			if (Options.EntryPoints.empty() || Options.EntryPoints.size() > ShaderCompiledOutput::MaximumEntryPoints
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
		auto ValidCode(FByteView Code) -> bool
		{
			uint32 Magic = 0;
			return Code.size() >= 20 && Code.size() <= MaximumCodeBytes && Code.size() % 4 == 0
				&& ReadLittleEndianAt(Code, 0, Magic) && Magic == 0x07230203u;
		}
		auto ReadReflection(const FSharedByteBuffer& Block, size_t Entry, FShaderReflectionData* Output,
			const std::function<bool()>& Cancel) -> FShaderOperationResult
		{
			FBinaryReader Reader(Block.GetBytes(), {.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
			uint32 Count = 0;
			if (!Reader.ReadU32(Count) || Count > MaximumReflectionEntries) return Failure(EShaderError::PayloadBindingCountInvalid, Entry);
			if (Output) Output->ResourceBindings.reserve(Count);
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
				if (Output) Output->ResourceBindings.push_back(std::move(Binding));
			}
			if (!Reader.ReadU32(Count) || Count > MaximumReflectionEntries) return Failure(EShaderError::PayloadPushConstantCountInvalid, Entry);
			if (Output) Output->PushConstantRanges.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				if (Index % 256 == 0 && Cancel && Cancel()) return Failure(EShaderError::Cancelled);
				FPushConstantRange Range{}; uint32 Flags = 0, Reserved = 0;
				if (!Reader.ReadU32(Flags) || !Reader.ReadU32(Range.Offset) || !Reader.ReadU32(Range.Size)
					|| !Reader.ReadU32(Reserved) || Reserved || !ValidFlags(Flags))
					return Failure(EShaderError::PayloadPushConstantInvalid, Entry);
				Range.StageFlags = EShaderStageFlags(Flags);
				if (!ValidRange(Range)) return Failure(EShaderError::PayloadPushConstantInvalid, Entry);
				if (Output) Output->PushConstantRanges.push_back(Range);
			}
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
		struct FValidatedShaderReceipt final : FBuildValidationReceipt
		{
			FBuildOutput Output;
			std::string VirtualPath;
			FShaderCompilerOutput Product;
			auto Matches(const FShaderCompileOptions& Options, const FBuildOutput& Candidate) const -> bool
			{
				if (!Output.SharesStateWith(Candidate) || VirtualPath != Options.VirtualShaderPath
					|| Product.CompiledShaders.size() != Options.EntryPoints.size()
					|| Options.Frequencies.size() != Options.EntryPoints.size()) return false;
				for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
					if (!Options.EntryPoints[Index]
						|| Product.CompiledShaders[Index].SourceEntryPoint != Options.EntryPoints[Index]
						|| Product.CompiledShaders[Index].Frequency != Options.Frequencies[Index]) return false;
				return true;
			}
		};
		auto ReadLayout(const FShaderCompileOptions& Options, const FBuildOutput& Output,
			const std::function<bool()>& Cancel) -> std::expected<std::vector<FEntry>, FShaderError>
		{
			if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
			if (!ValidRequest(Options)) return Failure(EShaderError::PayloadRequestInvalid);
			if (Output.GetSchema() != "Shader.Output" || Output.GetSchemaVersion() != 1
				|| Output.GetMetadata().size() > MaximumMetadataBytes
				|| !Output.CheckLimits({.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes}))
				return Failure(EShaderError::PayloadHeaderInvalid);
			FBinaryReader Reader(Output.GetMetadata().GetBytes(), {.MaximumTotalBytes = MaximumMetadataBytes});
			uint32 Count = 0;
			if (!Reader.ReadU32(Count) || Count != Options.EntryPoints.size() || Output.GetValues().size() != Count * 2)
				return Failure(EShaderError::PayloadHeaderInvalid);
			std::vector<FEntry> Entries; Entries.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
				FEntry Entry; uint32 Frequency = 0;
				if (!Reader.ReadString(Entry.SourceName, MaximumStringBytes) || !Reader.ReadString(Entry.BinaryName, MaximumStringBytes)
					|| !Reader.ReadU32(Frequency) || !Reader.ReadU64(Entry.Hash.HashLow) || !Reader.ReadU64(Entry.Hash.HashHigh)
					|| Entry.SourceName != Options.EntryPoints[Index] || Entry.BinaryName.empty()
					|| Frequency != uint32(Options.Frequencies[Index])) return Failure(EShaderError::PayloadEntryInvalid, Index);
				Entry.Frequency = EShaderFrequency(Frequency);
				const auto Prefix = "Entry/" + std::to_string(Index);
				const auto* Code = Output.FindValue(Prefix + "/Code");
				const auto* Reflection = Output.FindValue(Prefix + "/Reflection");
				if (!Code || !Reflection) return Failure(EShaderError::PayloadEntryInvalid, Index);
				if (!ValidCode(Code->Data.GetBytes())) return Failure(EShaderError::PayloadSpirvInvalid, Index);
				if (FXxHash128::HashBuffer(Code->Data.GetBytes()) != Entry.Hash) return Failure(EShaderError::PayloadSpirvHashMismatch, Index);
				Entry.Code = Code->Data; Entry.Reflection = Reflection->Data;
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
		FBuildOutputData Data{.Schema = "Shader.Output", .SchemaVersion = 1};
		uint64 DataBytes = 0;
		FBinaryWriter Metadata({.MaximumTotalBytes = MaximumMetadataBytes}); Metadata.WriteU32(uint32(Product.CompiledShaders.size()));
		for (size_t Index = 0; Index < Product.CompiledShaders.size(); ++Index)
		{
			if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
			const auto& Shader = Product.CompiledShaders[Index];
			if (!Shader.Code || Shader.SourceEntryPoint != Options.EntryPoints[Index] || Shader.Frequency != Options.Frequencies[Index]
				|| Shader.SourceEntryPoint.size() > MaximumStringBytes || Shader.BinaryEntryPoint.empty() || Shader.BinaryEntryPoint.size() > MaximumStringBytes
				|| !ValidCode(Shader.Code->GetBytes()) || FXxHash128::HashBuffer(Shader.Code->GetBytes()) != Shader.Hash)
				return Failure(EShaderError::PayloadOutputInvalid, Index);
			Metadata.WriteString(Shader.SourceEntryPoint); Metadata.WriteString(Shader.BinaryEntryPoint); Metadata.WriteU32(uint32(Shader.Frequency));
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
			if (Descriptor.HasError()) return Failure(EShaderError::PayloadTooLarge, Index);
			DataBytes += Shader.Code->size() + Descriptor.Tell();
			const auto Prefix = "Entry/" + std::to_string(Index);
			Data.Values.push_back({Prefix + "/Code", *Shader.Code});
			Data.Values.push_back({Prefix + "/Reflection", FSharedByteBuffer::Take(Descriptor.TakeBytes())});
		}
		if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
		if (Metadata.HasError()) return Failure(EShaderError::PayloadTooLarge);
		Data.Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes());
		auto Output = FBuildOutput::TryCreate(std::move(Data), {.MaximumTotalBytes = ShaderCompiledOutput::MaximumValueBytes});
		if (!Output) return Failure(EShaderError::PayloadTooLarge);
		return std::move(*Output);
	}

	auto Validate(const FShaderCompileOptions& Options, const FBuildOutput& Output, const std::function<bool()>& Cancel) -> FShaderOperationResult
	{
		auto Receipt = ValidateWithReceipt(Options, Output, Cancel);
		if (!Receipt) return std::unexpected(std::move(Receipt.error()));
		return {};
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
			Shader.Frequency = Entry.Frequency; Shader.SourceEntryPoint = std::move(Entry.SourceName);
			Shader.BinaryEntryPoint = std::move(Entry.BinaryName); Shader.Hash = Entry.Hash;
			Shader.DebugName = Options.VirtualShaderPath.empty() ? Shader.SourceEntryPoint : Options.VirtualShaderPath + "::" + Shader.SourceEntryPoint;
			Shader.Code = std::make_shared<const FSharedByteBuffer>(std::move(Entry.Code));
			if (auto Valid = ReadReflection(Entry.Reflection, Index, &Shader.Reflection, Cancel); !Valid) return std::unexpected(std::move(Valid.error()));
			Product.CompiledShaders.push_back(std::move(Shader));
		}
		if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
		return Product;
	}

	auto ValidateWithReceipt(const FShaderCompileOptions& Options, const FBuildOutput& Output,
		const std::function<bool()>& Cancel)
		-> std::expected<std::shared_ptr<const FBuildValidationReceipt>, FShaderError>
	{
		auto Product = AssembleImpl(Options, Output, Cancel);
		if (!Product) return std::unexpected(std::move(Product.error()));
		try
		{
			auto Receipt = std::make_shared<FValidatedShaderReceipt>();
			Receipt->Output = Output; Receipt->VirtualPath = Options.VirtualShaderPath;
			Receipt->Product = std::move(*Product);
			return std::shared_ptr<const FBuildValidationReceipt>(std::move(Receipt));
		}
		catch (const std::bad_alloc&) { return Failure(EShaderError::PayloadTooLarge); }
	}

	auto Assemble(const FShaderCompileOptions& Options, const FBuildOutput& Output,
		const std::function<bool()>& Cancel, const FBuildValidationReceipt* Receipt)
		-> std::expected<FShaderCompilerOutput, FShaderError>
	{
		if (!Receipt) return AssembleImpl(Options, Output, Cancel);
		const auto* Validated = dynamic_cast<const FValidatedShaderReceipt*>(Receipt);
		if (!Validated || !Validated->Matches(Options, Output)) return Failure(EShaderError::PayloadRequestInvalid);
		if (Cancel && Cancel()) return Failure(EShaderError::Cancelled);
		return Validated->Product;
	}
}
