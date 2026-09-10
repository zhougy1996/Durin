#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialRenderTypes.h"
#include "Asset/Load.h"

#include "Serialization/Archive.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 MaterialCookedProgramMagic = 0x54414d44; // DMAT
		constexpr uint64 MaterialCookedProgramMaxStringBytes = 4096;
		constexpr uint64 MaterialCookedProgramMaxBindingsPerStage = 128;
		constexpr uint64 MaterialCookedProgramMaxPushRangesPerStage = 16;

		auto SerializeHash(FArchive& Ar, FXxHash128& Hash) -> void
		{
			Ar << Hash.HashLow << Hash.HashHigh;
		}

		auto SerializeStaticProperties(
			FArchive& Ar, FMaterialStaticProperties& Properties) -> void
		{
			Ar << Properties.BlendMode
				<< Properties.ShadingModel
				<< Properties.bTwoSided
				<< Properties.DepthWritePolicy
				<< Properties.OpacityMaskThreshold;
		}

		auto SerializeDependency(
			FArchive& Ar, FMaterialCompilerDependency& Dependency) -> void
		{
			SerializeBoundedString(
				Ar, Dependency.VirtualPath, MaterialCookedProgramMaxStringBytes);
			SerializeHash(Ar, Dependency.ContentHash);
		}

		auto SerializeBinding(FArchive& Ar, FShaderResourceBinding& Binding)
			-> void
		{
			SerializeBoundedString(
				Ar, Binding.Name, MaterialCookedProgramMaxStringBytes);
			Ar << Binding.StageFlags << Binding.SetIndex << Binding.BindingIndex
				<< Binding.Type << Binding.ArraySize;
		}

		auto SerializePushRange(FArchive& Ar, FPushConstantRange& Range) -> void
		{
			Ar << Range.StageFlags << Range.Offset << Range.Size;
		}

		auto SerializeShader(FArchive& Ar, FCompiledShader& Shader) -> void
		{
			Ar << Shader.Frequency;
			SerializeBoundedString(
				Ar, Shader.SourceEntryPoint, MaterialCookedProgramMaxStringBytes);
			SerializeBoundedString(
				Ar, Shader.BinaryEntryPoint, MaterialCookedProgramMaxStringBytes);
			SerializeBoundedString(
				Ar, Shader.DebugName, MaterialCookedProgramMaxStringBytes);
			FByteBuffer Code = Ar.IsSaving() && Shader.Code
				? *Shader.Code : FByteBuffer{};
			SerializeByteBuffer(Ar, Code, MaterialCookedProgramMaxPayloadBytes);
			SerializeHash(Ar, Shader.Hash);
			SerializeBoundedSequence(
				Ar, Shader.Reflection.ResourceBindings,
				MaterialCookedProgramMaxBindingsPerStage,
				[](FArchive& Inner, FShaderResourceBinding& Binding) {
					SerializeBinding(Inner, Binding);
				});
			SerializeBoundedSequence(
				Ar, Shader.Reflection.PushConstantRanges,
				MaterialCookedProgramMaxPushRangesPerStage,
				[](FArchive& Inner, FPushConstantRange& Range) {
					SerializePushRange(Inner, Range);
				});
			if (Ar.IsLoading() && !Ar.HasError())
				Shader.Code = std::make_shared<FByteBuffer>(
					std::move(Code));
		}

		auto SerializeLayout(FArchive& Ar, FMaterialRenderLayout& Layout) -> void
		{
			Ar << Layout.Identity.Version << Layout.Identity.Id << Layout.UniformPayloadSize
				<< Layout.UniformFieldCount << Layout.ResourceFieldCount;
			SerializeBoundedSequence(Ar, Layout.Fields, MaterialRenderMaxFieldCount,
				[](FArchive& Inner, FMaterialRenderField& Field) {
					Inner << Field.ParameterId << Field.Storage << Field.Type
						<< Field.CompactIndex << Field.Offset << Field.Size;
				});
		}

		auto SerializePayload(
			FArchive& Ar,
			FMaterialCompilerResult& Program,
			FMaterialStaticProperties& StaticProperties,
			ECookTargetPlatform& TargetPlatform,
			ECookTargetProfile& TargetProfile) -> void
		{
			uint32 Magic = MaterialCookedProgramMagic;
			uint32 SchemaVersion = MaterialCookedProgramPayloadSchemaVersion;
			uint32 ProgramSchemaVersion = CurrentMaterialProgramSchemaVersion;
			uint32 IRVersion = CurrentMaterialIRVersion;
			uint32 GeneratorVersion = CurrentMaterialGeneratorVersion;
			uint32 EnvelopeVersion = CurrentMaterialCompilerEnvelopeVersion;
			Ar << Magic << SchemaVersion << ProgramSchemaVersion << IRVersion
				<< GeneratorVersion << EnvelopeVersion
				<< Program.PassContractVersion << TargetPlatform << TargetProfile;
			if (Ar.IsLoading() && !Ar.HasError()
				&& (Magic != MaterialCookedProgramMagic
					|| SchemaVersion != MaterialCookedProgramPayloadSchemaVersion
					|| ProgramSchemaVersion != CurrentMaterialProgramSchemaVersion
					|| IRVersion != CurrentMaterialIRVersion
					|| GeneratorVersion != CurrentMaterialGeneratorVersion
					|| EnvelopeVersion != CurrentMaterialCompilerEnvelopeVersion
					|| Program.PassContractVersion
						!= CurrentMaterialPassContractVersion))
			{
				Ar.Fail(EArchiveFailureCode::UnsupportedVersion,
					"Material cooked program versions are incompatible.");
				return;
			}

			SerializeHash(Ar, Program.Identity.Digest);
			SerializeBoundedString(
				Ar, Program.CompilerIdentity, MaterialCookedProgramMaxStringBytes);
			SerializeBoundedString(
				Ar, Program.Target, MaterialCookedProgramMaxStringBytes);
			SerializeStaticProperties(Ar, StaticProperties);
			SerializeLayout(Ar, Program.Layout);
			SerializeBoundedSequence(
				Ar, Program.ActiveParameters, MaterialProgramMaxReferencedParameterCount,
				[](FArchive& Inner, FMaterialCompilerParameterDeclaration& Parameter) {
					Inner << Parameter.Id << Parameter.Type;
				});
			SerializeBoundedSequence(
				Ar, Program.Dependencies, 64,
				[](FArchive& Inner, FMaterialCompilerDependency& Dependency) {
					SerializeDependency(Inner, Dependency);
				});
			SerializeBoundedSequence(
				Ar, Program.CompiledShaders, 3,
				[](FArchive& Inner, FCompiledShader& Shader) {
					SerializeShader(Inner, Shader);
				});
		}

		auto ValidateDecodedProgram(
			const FMaterialCompilerResult& Program,
			const FMaterialStaticProperties& StaticProperties,
			bool bRequireCurrentEnvironment,
			std::string& OutError) -> bool
		{
			if (!Program.Identity.IsValid() || Program.CompilerIdentity.empty()
				|| Program.Target.empty()
				|| Program.PassContractVersion
					!= CurrentMaterialPassContractVersion)
				return Fail("Material cooked program identity or environment is invalid.",
					&OutError);
			if (bRequireCurrentEnvironment && Program.Target != "vulkan-spirv-1.5")
				return Fail("Material cooked program target is incompatible.", &OutError);
			// Cooked execution has no compiler provider. Versions, target, layout,
			// stage contracts and byte hashes validate its source-independent ABI.
			if (bRequireCurrentEnvironment && !GetAssetRuntimeConfiguration().IsCooked())
			{
				const std::string CurrentCompilerIdentity =
					GetShaderCompilerEnvironmentIdentity();
				if (CurrentCompilerIdentity.empty()
					|| Program.CompilerIdentity != CurrentCompilerIdentity
					|| Program.Target != "vulkan-spirv-1.5")
					return Fail(
						"Material cooked program compiler or target identity is incompatible.",
						&OutError);
			}
			if (!ValidateMaterialStaticProperties(StaticProperties, OutError))
				return false;
			if (Program.ActiveParameters.size() > MaterialProgramMaxReferencedParameterCount)
				return Fail("Material active parameter count exceeds its limit.", &OutError);
			FGuid PreviousId;
			for (const auto& Parameter : Program.ActiveParameters)
			{

				if (!Parameter.Id.IsValid()
					|| (PreviousId.IsValid() && !(PreviousId < Parameter.Id)))
					return Fail("Material active parameter contract is invalid.", &OutError);
				PreviousId = Parameter.Id;
			}
			for (const FCompiledShader& Shader : Program.CompiledShaders)
			{
				if (!Shader.Code || Shader.Code->empty() || Shader.BinaryEntryPoint.empty()
					|| FXxHash128::HashBuffer(*Shader.Code) != Shader.Hash)
					return Fail("Material cooked shader code hash is invalid.", &OutError);
			}
			const auto Contract = ValidateMaterialCompilerResult(Program);
			if (!Contract) return Fail(std::string(GetMaterialLayoutErrorText(Contract.Error)), &OutError);
			OutError.clear();
			return true;
		}
	}

	auto EncodeMaterialCookedProgram(
		const FMaterialCompilerResult& Program,
		const FMaterialStaticProperties& StaticProperties,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteBuffer& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (TargetPlatform != ECookTargetPlatform::Win64 || TargetProfile != ECookTargetProfile::Game)
			return Fail("Material cooked program target is unsupported.", &OutError);
		if (!Program || !ValidateDecodedProgram(
				Program, StaticProperties, false, OutError)) return false;
		FMaterialCompilerResult Copy = Program;
		FMaterialStaticProperties PropertyCopy = StaticProperties;
		FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::CookedPayload);
		SerializePayload(
			Ar, Copy, PropertyCopy, TargetPlatform, TargetProfile);
		if (Ar.HasError())
		{
			OutError = Ar.GetFailure()->Message;
			OutBytes.clear();
			return false;
		}
		if (!Ar.HasError())
		{
			auto Checksum = FXxHash128::HashBuffer(OutBytes);
			SerializeHash(Ar, Checksum);
		}
		if (Ar.HasError() || OutBytes.size() > MaterialCookedProgramMaxPayloadBytes)
		{
			OutBytes.clear();
			return Fail("Material cooked program exceeds its payload byte limit.",
				&OutError);
		}
		OutError.clear();
		return true;
	}

	auto DecodeMaterialCookedProgram(
		FByteView Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram,
		std::string& OutError) -> bool
	{
		if (Bytes.size() < 24 || Bytes.size() > MaterialCookedProgramMaxPayloadBytes)
			return Fail("Material cooked program byte extent is invalid.", &OutError);
		FCanonicalMemoryReader Header(Bytes.first(8), EArchivePurpose::CookedPayload);
		uint32 Magic = 0, Version = 0;
		Header << Magic << Version;
		if (Header.HasError() || Magic != MaterialCookedProgramMagic || Version != MaterialCookedProgramPayloadSchemaVersion)
			return Fail("Material cooked program format is incompatible; recook from authored assets.", &OutError);
		const FByteView Payload = Bytes.first(Bytes.size() - 16);
		FCanonicalMemoryReader ChecksumReader(Bytes.last(16), EArchivePurpose::CookedPayload);
		FXxHash128 StoredChecksum;
		SerializeHash(ChecksumReader, StoredChecksum);
		if (ChecksumReader.HasError() || StoredChecksum != FXxHash128::HashBuffer(Payload))
			return Fail("Material cooked program checksum is invalid.", &OutError);
		FMaterialCompilerResult Candidate;
		FMaterialStaticProperties CandidateProperties;
		ECookTargetPlatform Platform = ECookTargetPlatform::Invalid;
		ECookTargetProfile Profile = ECookTargetProfile::Invalid;
		FCanonicalMemoryReader Ar(Payload, EArchivePurpose::CookedPayload);
		SerializePayload(Ar, Candidate, CandidateProperties, Platform, Profile);
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
			return Fail(Ar.GetFailure() ? Ar.GetFailure()->Message
				: "Material cooked program contains trailing data.", &OutError);
		if (Platform != ExpectedPlatform || Profile != ExpectedProfile)
			return Fail("Material cooked program target is incompatible.", &OutError);
		Candidate.bSucceeded = true;
		if (!ValidateDecodedProgram(
				Candidate, CandidateProperties, true, OutError))
			return false;
		OutStaticProperties = CandidateProperties;
		OutProgram = std::make_shared<const FMaterialCompilerResult>(
			std::move(Candidate));
		OutError.clear();
		return true;
	}
}
