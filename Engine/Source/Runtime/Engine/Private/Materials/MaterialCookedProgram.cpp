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
				? FByteBuffer(Shader.Code->begin(), Shader.Code->end()) : FByteBuffer{};
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
				Shader.Code = std::make_shared<const FSharedByteBuffer>(FSharedByteBuffer::Take(std::move(Code)));
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

		auto SerializeCollectionLayout(FArchive& Ar,
			FMaterialParameterCollectionLayout& Collection) -> void
		{
			SerializeBoundedString(Ar, Collection.AssetPath,
				MaterialCookedProgramMaxStringBytes);
			Ar << Collection.CollectionId << Collection.SchemaVersion;
			SerializeLayout(Ar, Collection.UniformLayout);
			SerializeByteBuffer(Ar, Collection.DefaultPayload,
				MaterialRenderMaxUniformPayloadBytes);
		}

		auto SerializeProgramConfiguration(FArchive& Ar,
			FMaterialCompilerResult& Program) -> void
		{
			Ar << Program.Quality << Program.FeatureLevel;
			SerializeBoundedSequence(Ar, Program.StaticBools,
				MaterialMaxStaticBoolDeclarations,
				[](FArchive& Inner, FMaterialCompilerEnvironment::FStaticBoolValue& Value) {
					Inner << Value.DeclarationId << Value.Value;
				});
		}

		auto SerializeRequirements(FArchive& Ar,
			FMaterialProgramRequirements& Requirements) -> void
		{
			Ar << Requirements.bMaterialView << Requirements.bMaterialPrimitive
				<< Requirements.bCameraPosition << Requirements.bViewport
				<< Requirements.bViewTransforms << Requirements.bObjectTransforms
				<< Requirements.bBoundsCenter << Requirements.bTangentFrame
				<< Requirements.bVertexNormal;
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
			SerializeProgramConfiguration(Ar, Program);
			SerializeLayout(Ar, Program.Layout);
			SerializeRequirements(Ar, Program.Requirements);
			SerializeBoundedSequence(
				Ar, Program.ActiveParameters, MaterialProgramMaxReferencedParameterCount,
				[](FArchive& Inner, FMaterialCompilerParameterDeclaration& Parameter) {
					Inner << Parameter.Id << Parameter.Type;
				});
			SerializeBoundedSequence(Ar, Program.ActiveCollections,
				MaterialParameterCollectionMaxPerMaterial,
				[](FArchive& Inner, FMaterialParameterCollectionLayout& Collection) {
					SerializeCollectionLayout(Inner, Collection);
				});
			SerializeBoundedSequence(
				Ar, Program.Dependencies, 64,
				[](FArchive& Inner, FMaterialCompilerDependency& Dependency) {
					SerializeDependency(Inner, Dependency);
				});
			SerializeBoundedSequence(
				Ar, Program.CompiledShaders, MaterialCompiledEntryPoints.size(),
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
			if (!IsSupportedMaterialQualityLevel(Program.Quality)
				|| !IsSupportedMaterialFeatureLevel(Program.FeatureLevel)
				|| Program.StaticBools.size() > MaterialMaxStaticBoolDeclarations
				|| !std::ranges::is_sorted(Program.StaticBools, {},
					&FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId))
				return {EMaterialCookError::CookedProgramIdentityEnvironmentInvalid};
			for (size_t Index = 0; Index < Program.StaticBools.size(); ++Index)
				if (!Program.StaticBools[Index].DeclarationId.IsValid()
					|| (Index && Program.StaticBools[Index - 1].DeclarationId
						== Program.StaticBools[Index].DeclarationId))
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
			FGuid PreviousCollection;
			for (const auto& Collection : Program.ActiveCollections)
			{
				FObjectPath AssetPath;
				if ((!Collection.AssetPath.empty()
						&& (!FObjectPath::TryCreate(Collection.AssetPath, AssetPath)
							|| !AssetPath.IsTopLevelAsset()))
					|| !Collection.CollectionId.IsValid()
					|| (PreviousCollection.IsValid()
						&& !(PreviousCollection < Collection.CollectionId))
					|| Collection.SchemaVersion
						!= CurrentMaterialParameterCollectionSchemaVersion
					|| !ValidateCompiledMaterialLayout(Collection.UniformLayout)
					|| Collection.DefaultPayload.size()
						!= Collection.UniformLayout.UniformPayloadSize)
					return {EMaterialCookError::ActiveParameterContractInvalid};
				PreviousCollection = Collection.CollectionId;
			}
			for (const FCompiledShader& Shader : Program.CompiledShaders)
			{
				if (!Shader.Code || Shader.Code->IsEmpty() || Shader.BinaryEntryPoint.empty()
					|| FXxHash128::HashBuffer(*Shader.Code) != Shader.Hash)
					return {EMaterialCookError::CookedShaderCodeHashInvalid};
			}
			const auto Contract = ValidateMaterialCompilerResult(Program);
			if (!Contract) return {FMaterialError(Contract)};
			return {};
		}
	}

	namespace
	{
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

	namespace
	{
		constexpr uint32 MaterialCookedProgramFamilyMagic = 0x464d4444; // DDMF

		struct FCookedConfigurationRecord
		{
			EMaterialQualityLevel Quality = EMaterialQualityLevel::High;
			ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::SM5;
			std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
			uint32 ArtifactIndex = 0;
		};

		auto SerializeConfigurationRecord(FArchive& Ar,
			FCookedConfigurationRecord& Record) -> void
		{
			Ar << Record.Quality << Record.FeatureLevel;
			SerializeBoundedSequence(Ar, Record.StaticBools,
				MaterialMaxStaticBoolDeclarations,
				[](FArchive& Inner,
					FMaterialCompilerEnvironment::FStaticBoolValue& Value) {
					Inner << Value.DeclarationId << Value.Value;
				});
			Ar << Record.ArtifactIndex;
		}

		auto IsValidConfiguration(const FCookedConfigurationRecord& Record) -> bool
		{
			if (!IsSupportedMaterialQualityLevel(Record.Quality)
				|| !IsSupportedMaterialFeatureLevel(Record.FeatureLevel)
				|| !std::ranges::is_sorted(Record.StaticBools, {},
					&FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId)) return false;
			for (size_t Index = 0; Index < Record.StaticBools.size(); ++Index)
				if (!Record.StaticBools[Index].DeclarationId.IsValid()
					|| (Index && Record.StaticBools[Index - 1].DeclarationId
						== Record.StaticBools[Index].DeclarationId)) return false;
			return true;
		}
	}

	auto EncodeMaterialCookedProgramFamily(
		std::span<const FMaterialCompilerResult* const> Programs,
		const FMaterialStaticProperties& StaticProperties,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile,
		FByteBuffer& OutBytes) -> FMaterialOperationResult
	{
		OutBytes.clear();
		if (Programs.empty() || Programs.size() > MaterialCookedProgramMaxConfigurations)
			return {EMaterialCookError::ProgramUnavailable};
		std::vector<FCookedConfigurationRecord> Configurations;
		std::vector<FByteBuffer> Artifacts;
		std::vector<FMaterialProgramIdentity> ArtifactIdentities;
		for (const auto* Program : Programs)
		{
			if (!Program) return {EMaterialCookError::ProgramUnavailable};
			FCookedConfigurationRecord Configuration{
				.Quality = Program->Quality,
				.FeatureLevel = Program->FeatureLevel,
				.StaticBools = Program->StaticBools};
			if (!IsValidConfiguration(Configuration))
				return {EMaterialCookError::CookedProgramIdentityEnvironmentInvalid};
			if (std::ranges::any_of(Configurations, [&](const auto& Existing) {
				return Existing.Quality == Configuration.Quality
					&& Existing.FeatureLevel == Configuration.FeatureLevel
					&& Existing.StaticBools == Configuration.StaticBools;
			})) return {EMaterialCookError::CookedProgramIdentityEnvironmentInvalid};
			const auto Existing = std::ranges::find(ArtifactIdentities, Program->Identity);
			if (Existing == ArtifactIdentities.end())
			{
				FByteBuffer Artifact;
				if (auto Encoded = EncodeMaterialCookedProgram(*Program, StaticProperties,
					TargetPlatform, TargetProfile, Artifact); !Encoded) return Encoded;
				Configuration.ArtifactIndex = static_cast<uint32>(Artifacts.size());
				ArtifactIdentities.push_back(Program->Identity);
				Artifacts.push_back(std::move(Artifact));
			}
			else Configuration.ArtifactIndex = static_cast<uint32>(Existing - ArtifactIdentities.begin());
			Configurations.push_back(std::move(Configuration));
		}
		std::ranges::sort(Configurations, [](const auto& Left, const auto& Right) {
			if (Left.Quality != Right.Quality) return Left.Quality < Right.Quality;
			if (Left.FeatureLevel != Right.FeatureLevel) return Left.FeatureLevel < Right.FeatureLevel;
			return Left.StaticBools < Right.StaticBools;
		});
		FCanonicalMemoryWriter Ar(OutBytes, EArchivePurpose::CookedPayload);
		uint32 Magic = MaterialCookedProgramFamilyMagic;
		uint32 Version = MaterialCookedProgramPayloadSchemaVersion;
		Ar << Magic << Version << TargetPlatform << TargetProfile;
		SerializeBoundedSequence(Ar, Configurations,
			MaterialCookedProgramMaxConfigurations, SerializeConfigurationRecord);
		SerializeBoundedSequence(Ar, Artifacts,
			MaterialCookedProgramMaxConfigurations,
			[](FArchive& Inner, FByteBuffer& Artifact) {
				SerializeByteBuffer(Inner, Artifact, MaterialCookedProgramMaxPayloadBytes);
			});
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

	auto DecodeMaterialCookedProgramFamily(
		FByteView Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		EMaterialQualityLevel Quality,
		ERHIFeatureLevel FeatureLevel,
		std::span<const FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools,
		FMaterialStaticProperties& OutStaticProperties,
		std::shared_ptr<const FMaterialCompilerResult>& OutProgram) -> FMaterialOperationResult
	{
		if (Bytes.size() < 32 || Bytes.size() > MaterialCookedProgramMaxPayloadBytes)
			return {EMaterialCookError::CookedProgramByteExtentInvalid};
		const FByteView Payload = Bytes.first(Bytes.size() - 16);
		FCanonicalMemoryReader ChecksumReader(Bytes.last(16), EArchivePurpose::CookedPayload);
		FXxHash128 StoredChecksum;
		SerializeHash(ChecksumReader, StoredChecksum);
		if (ChecksumReader.IsError() || StoredChecksum != FXxHash128::HashBuffer(Payload))
			return {EMaterialCookError::CookedProgramChecksumInvalid};
		uint32 Magic = 0, Version = 0;
		ECookTargetPlatform Platform = ECookTargetPlatform::Invalid;
		ECookTargetProfile Profile = ECookTargetProfile::Invalid;
		std::vector<FCookedConfigurationRecord> Configurations;
		std::vector<FByteBuffer> Artifacts;
		FCanonicalMemoryReader Ar(Payload, EArchivePurpose::CookedPayload);
		Ar << Magic << Version << Platform << Profile;
		SerializeBoundedSequence(Ar, Configurations,
			MaterialCookedProgramMaxConfigurations, SerializeConfigurationRecord);
		SerializeBoundedSequence(Ar, Artifacts,
			MaterialCookedProgramMaxConfigurations,
			[](FArchive& Inner, FByteBuffer& Artifact) {
				SerializeByteBuffer(Inner, Artifact, MaterialCookedProgramMaxPayloadBytes);
			});
		if (Ar.IsError() || !RequireArchiveEnd(Ar))
			return {FMaterialError::FromArchive(*Ar.GetFailure())};
		if (Magic != MaterialCookedProgramFamilyMagic
			|| Version != MaterialCookedProgramPayloadSchemaVersion)
			return {EMaterialCookError::IncompatiblePayloadFormat};
		if (Platform != ExpectedPlatform || Profile != ExpectedProfile)
			return {EMaterialCookError::CookedProgramTargetIncompatible};
		if (Configurations.empty() || Artifacts.empty())
			return {EMaterialCookError::ProgramUnavailable};
		std::vector<std::shared_ptr<const FMaterialCompilerResult>> DecodedArtifacts;
		DecodedArtifacts.reserve(Artifacts.size());
		std::optional<FMaterialStaticProperties> CommonProperties;
		for (const auto& Artifact : Artifacts)
		{
			FMaterialStaticProperties Properties;
			std::shared_ptr<const FMaterialCompilerResult> Program;
			if (auto Decoded = DecodeMaterialCookedProgram(Artifact, ExpectedPlatform,
				ExpectedProfile, Properties, Program); !Decoded) return Decoded;
			if (CommonProperties && *CommonProperties != Properties)
				return {EMaterialCookError::StaticPropertiesMismatch};
			CommonProperties = Properties;
			DecodedArtifacts.push_back(std::move(Program));
		}
		const FCookedConfigurationRecord* Selected = nullptr;
		for (const auto& Configuration : Configurations)
		{
			if (!IsValidConfiguration(Configuration)
				|| Configuration.ArtifactIndex >= DecodedArtifacts.size())
				return {EMaterialCookError::CookedProgramIdentityEnvironmentInvalid};
			if (Configuration.Quality == Quality
				&& Configuration.FeatureLevel == FeatureLevel
				&& (StaticBools.empty()
					|| std::ranges::equal(Configuration.StaticBools, StaticBools)))
			{
				if (Selected && StaticBools.empty()
					&& Selected->StaticBools != Configuration.StaticBools)
					return {EMaterialCookError::CookedProgramIdentityEnvironmentInvalid};
				Selected = &Configuration;
			}
		}
		if (!Selected) return {EMaterialCookError::CookedProgramConfigurationMissing};
		auto Configured = std::make_shared<FMaterialCompilerResult>(
			*DecodedArtifacts[Selected->ArtifactIndex]);
		Configured->Quality = Quality;
		Configured->FeatureLevel = FeatureLevel;
		Configured->StaticBools = Selected->StaticBools;
		OutStaticProperties = *CommonProperties;
		OutProgram = std::move(Configured);
		return {};
	}
}
