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
			FMaterialStaticProperties Shader = CanonicalizeMaterialShaderProperties(Properties);
			Ar << Shader.BlendMode << Shader.ShadingModel << Shader.OpacityMaskThreshold;
			if (Ar.IsLoading())
			{
				Durin::FMaterialOperationResult Error;
				if (!(Error = ValidateMaterialStaticProperties(Shader))
					|| Shader != CanonicalizeMaterialShaderProperties(Shader))
				{
					Ar.Fail(EArchiveFailureCode::InvalidData, "Noncanonical cooked material shader properties.");
					return;
				}
				Properties = Shader;
			}
			Ar << Properties.bTwoSided << Properties.DepthWritePolicy;
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
			if (Ar.IsLoading() && !Ar.IsError())
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
			uint32 IRVersion = MIR::CurrentVersion;
			uint32 GeneratorVersion = CurrentMaterialGeneratorVersion;
			uint32 EnvelopeVersion = CurrentMaterialCompilerEnvelopeVersion;
			Ar << Magic << SchemaVersion << IRVersion
				<< GeneratorVersion << EnvelopeVersion
				<< Program.PassContractVersion << TargetPlatform << TargetProfile;
			if (Ar.IsLoading() && !Ar.IsError()
				&& (Magic != MaterialCookedProgramMagic
					|| SchemaVersion != MaterialCookedProgramPayloadSchemaVersion
					|| IRVersion != MIR::CurrentVersion
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
				Ar, Program.CompiledShaders, 4,
				[](FArchive& Inner, FCompiledShader& Shader) {
					SerializeShader(Inner, Shader);
				});
		}

		auto ValidateDecodedProgram(
			const FMaterialCompilerResult& Program,
			const FMaterialStaticProperties& StaticProperties,
			bool bRequireCurrentEnvironment) -> FMaterialOperationResult
		{
			if (!Program.Identity.IsValid() || Program.CompilerIdentity.empty()
				|| Program.Target.empty()
				|| Program.PassContractVersion
					!= CurrentMaterialPassContractVersion)
				return {EMaterialCookError::CookedProgramIdentityEnvironmentInvalid};
			if (bRequireCurrentEnvironment && Program.Target != "vulkan-spirv-1.5")
				return {EMaterialCookError::CookedProgramTargetIncompatible};
			// Cooked execution has no compiler provider. Versions, target, layout,
			// stage contracts and byte hashes validate its source-independent ABI.
			if (bRequireCurrentEnvironment && !GetAssetRuntimeConfiguration().IsCooked())
			{
				const std::string CurrentCompilerIdentity =
					GetShaderCompilerEnvironmentIdentity();
				if (CurrentCompilerIdentity.empty()
					|| Program.CompilerIdentity != CurrentCompilerIdentity
					|| Program.Target != "vulkan-spirv-1.5")
					return {EMaterialCookError::CookedProgramCompilerTargetIdentityIncompatible};
			}
			if (const auto Validation = ValidateMaterialStaticProperties(StaticProperties); !Validation) return Validation;
			if (Program.ActiveParameters.size() > MaterialProgramMaxReferencedParameterCount)
				return {EMaterialCookError::ActiveParameterCountExceedsLimit};
			FGuid PreviousId;
			for (const auto& Parameter : Program.ActiveParameters)
			{

				if (!Parameter.Id.IsValid()
					|| (PreviousId.IsValid() && !(PreviousId < Parameter.Id)))
					return {EMaterialCookError::ActiveParameterContractInvalid};
				PreviousId = Parameter.Id;
			}
			for (const FCompiledShader& Shader : Program.CompiledShaders)
			{
				if (!Shader.Code || Shader.Code->empty() || Shader.BinaryEntryPoint.empty()
					|| FXxHash128::HashBuffer(*Shader.Code) != Shader.Hash)
					return {EMaterialCookError::CookedShaderCodeHashInvalid};
			}
			const auto Contract = ValidateMaterialCompilerResult(Program);
			if (!Contract) return {FMaterialError(Contract)};
			return {};
		}
	}

	auto EncodeMaterialCookedProgram(
		const FMaterialCompilerResult& Program,
		const FMaterialStaticProperties& StaticProperties,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteBuffer& OutBytes) -> FMaterialOperationResult
	{
		OutBytes.clear();
		if (TargetPlatform != ECookTargetPlatform::Win64 || TargetProfile != ECookTargetProfile::Game)
			return {EMaterialCookError::CookedProgramTargetUnsupported};
		if (!Program) return {EMaterialCookError::ProgramUnavailable};
		if (const auto Validation = ValidateDecodedProgram(
				Program, StaticProperties, false); !Validation) return Validation;
		FMaterialCompilerResult Copy = Program;
		FMaterialStaticProperties PropertyCopy = StaticProperties;
		FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::CookedPayload);
		SerializePayload(
			Ar, Copy, PropertyCopy, TargetPlatform, TargetProfile);
		if (Ar.IsError())
		{
			const auto Error = FMaterialError::FromArchive(*Ar.GetFailure());
			OutBytes.clear();
			return {Error};
		}
		if (!Ar.IsError())
		{
			auto Checksum = FXxHash128::HashBuffer(OutBytes);
			SerializeHash(Ar, Checksum);
		}
		if (Ar.IsError() || OutBytes.size() > MaterialCookedProgramMaxPayloadBytes)
		{
			OutBytes.clear();
			return {EMaterialCookError::CookedProgramExceedsPayloadByteLimit};
		}
		return {};
	}

	auto DecodeMaterialCookedProgram(
		FByteView Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram) -> FMaterialOperationResult
	{
		if (Bytes.size() < 24 || Bytes.size() > MaterialCookedProgramMaxPayloadBytes)
			return {EMaterialCookError::CookedProgramByteExtentInvalid};
		FCanonicalMemoryReader Header(Bytes.first(8), EArchivePurpose::CookedPayload);
		uint32 Magic = 0, Version = 0;
		Header << Magic << Version;
		if (Header.IsError() || Magic != MaterialCookedProgramMagic || Version != MaterialCookedProgramPayloadSchemaVersion)
			return {EMaterialCookError::IncompatiblePayloadFormat};
		const FByteView Payload = Bytes.first(Bytes.size() - 16);
		FCanonicalMemoryReader ChecksumReader(Bytes.last(16), EArchivePurpose::CookedPayload);
		FXxHash128 StoredChecksum;
		SerializeHash(ChecksumReader, StoredChecksum);
		if (ChecksumReader.IsError() || StoredChecksum != FXxHash128::HashBuffer(Payload))
			return {EMaterialCookError::CookedProgramChecksumInvalid};
		FMaterialCompilerResult Candidate;
		FMaterialStaticProperties CandidateProperties;
		ECookTargetPlatform Platform = ECookTargetPlatform::Invalid;
		ECookTargetProfile Profile = ECookTargetProfile::Invalid;
		FCanonicalMemoryReader Ar(Payload, EArchivePurpose::CookedPayload);
		SerializePayload(Ar, Candidate, CandidateProperties, Platform, Profile);
		if (Ar.IsError() || !RequireArchiveEnd(Ar))
		{
			return {FMaterialError::FromArchive(*Ar.GetFailure())};
		}
		if (Platform != ExpectedPlatform || Profile != ExpectedProfile)
			return {EMaterialCookError::CookedProgramTargetIncompatible};
		Candidate.bSucceeded = true;
		if (const auto Validation = ValidateDecodedProgram(
				Candidate, CandidateProperties, true); !Validation) return Validation;
		OutStaticProperties = CandidateProperties;
		OutProgram = std::make_shared<const FMaterialCompilerResult>(
			std::move(Candidate));
		return {};
	}
}
