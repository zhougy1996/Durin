#include "Materials/Material.h"
#include "Logging/LogMacros.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderTypes.h"
#include "Asset/Asset.h"
#include "DObject/Property.h"
#include "DObject/Package.h"
#include "Modules/ModuleManager.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto AdvanceRevision(uint64& Revision) -> void
		{
			Revision = Revision == std::numeric_limits<uint64>::max()
				? 1 : Revision + 1;
		}


	}

	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, ParameterDefinitions(MakePBRMaterialParameterDefinitions())
		, Program(MakeDefaultMaterialProgram())
		, GraphPresentation({
			.bHasMaterialOutputPosition = true,
			.MaterialOutputX = 96,
			.MaterialOutputY = 0})
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose))
		{
			if (!IsMaterialCompilationAcceptingRequests())
				RequestProgramCompile(Program, StaticProperties);
			PublishMaterialRenderProxyState();
		}
	}

	auto DMaterial::AdvanceAuthoredRevision() -> void
	{
		AdvanceRevision(CompilationOwner.MaterialCompileStatus.AuthoredRevision);
		InvalidateMaterialCompilation(false);
	}

	auto DMaterial::GetRenderableStaticProperties() const
		-> FMaterialStaticProperties
	{
		FMaterialStaticProperties Result = CompilationOwner.AcceptedCompiledProgram
			? CompilationOwner.AcceptedCompiledStaticProperties : StaticProperties;
		Result.bTwoSided = StaticProperties.bTwoSided;
		Result.DepthWritePolicy = StaticProperties.DepthWritePolicy;
		Result.OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(Result).OpacityMaskThreshold;
		return Result;
	}

	auto DMaterial::SetMaterialProgram(
		FMaterialProgram InProgram) -> FMaterialProgramValidationResult
	{
		if (InProgram.SchemaVersion != CurrentMaterialProgramSchemaVersion)
		{
			FMaterialProgramValidationResult Validation;
			Validation.Diagnostics.push_back({
				.Category = EMaterialProgramDiagnosticCategory::Schema,
				.Message = "Material program schema version is unsupported."});
			return Validation;
		}
		auto Validation = ValidateMaterialProgram(
			InProgram, ParameterDefinitions);
		if (!Validation) return Validation;
		if (Program == InProgram) return Validation;
		Program = std::move(InProgram);
		AdvanceRevision(MaterialProgramRevision);
		AdvanceAuthoredRevision();
		RequestProgramCompile(Program, StaticProperties);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap);
		return Validation;
	}

	auto DMaterial::SetMaterialDefinitionsAndProgram(
		std::vector<FMaterialParameterDefinition> Definitions,
		FMaterialProgram InProgram) -> FMaterialParameterEditResult
	{
		const auto Declarations = ValidateMaterialParameterDefinitions(Definitions);
		if (!Declarations) return {Declarations.Error, Declarations.ParameterId};
		if (InProgram.SchemaVersion != CurrentMaterialProgramSchemaVersion)
			return {.Error = EMaterialParameterError::UnsupportedProgramSchema};
		auto Validation = ValidateMaterialProgram(InProgram, Definitions);
		if (!Validation)
			return {.Error = EMaterialParameterError::InvalidProgram,
				.Diagnostics = std::move(Validation.Diagnostics)};
		for (const auto& Definition : Definitions)
		{
			const auto* Previous = FindParameterDefinition(Definition.Id);
			if (Previous && Previous->Type != Definition.Type)
				return {EMaterialParameterError::TypeConflict, Definition.Id};
		}
		if (Definitions == ParameterDefinitions && InProgram == Program) return {};
		CompilationOwner.RetainedAcceptedParameters = BuildMaterialLocalRenderLayer().Parameters;
		ParameterDefinitions = std::move(Definitions);
		ParameterDeclarationSchemaVersion = 2;
		Program = std::move(InProgram);
		GraphPresentation = SanitizeMaterialGraphPresentation(GraphPresentation, Program);
		AdvanceRevision(ParameterDefinitionSchemaRevision);
		AdvanceRevision(MaterialProgramRevision);
		AdvanceRevision(MaterialGraphPresentationRevision);
		AdvanceAuthoredRevision();
		RequestProgramCompile(Program, StaticProperties);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::AllRenderState);
		return {};
	}

	auto DMaterial::CreateParameterDefinition(
		FMaterialParameterDefinition Definition) -> FMaterialParameterEditResult
	{
		if (const auto* Existing = FindParameterDefinition(Definition.Name))
		{
			if (Existing->Type != Definition.Type)
				return {EMaterialParameterError::TypeConflict, Existing->Id};
			return {.ParameterId = Existing->Id};
		}
		if (!Definition.Id.IsValid()) Definition.Id = FGuid::NewGuid();
		const FGuid Id = Definition.Id;
		auto Candidate = ParameterDefinitions;
		Candidate.push_back(std::move(Definition));
		auto Result = SetMaterialDefinitionsAndProgram(std::move(Candidate), Program);
		if (Result) Result.ParameterId = Id;
		return Result;
	}

	auto DMaterial::RenameParameterDefinition(
		const FGuid& Id, FName Name) -> FMaterialParameterEditResult
	{
		const auto* Existing = FindParameterDefinition(Id);
		if (!Existing) return {EMaterialParameterError::NotFound, Id};
		if (Name.IsNone()) return {EMaterialParameterError::InvalidName, Id};
		if (const auto* Occupant = FindParameterDefinition(Name); Occupant && Occupant->Id != Id)
			return {EMaterialParameterError::DuplicateName, Id};
		if (Existing->Name == Name) return {.ParameterId = Id};
		auto Candidate = ParameterDefinitions;
		auto& Definition = *std::ranges::find(Candidate, Id, &FMaterialParameterDefinition::Id);
		Definition.Name = Name;
		Definition.DisplayName = Name.ToString();
		auto CandidateProgram = Program;
		for (auto& Node : CandidateProgram.Nodes)
			if (Node.ParameterId == Id) Node.DisplayName = Definition.DisplayName;
		auto Result = SetMaterialDefinitionsAndProgram(std::move(Candidate), std::move(CandidateProgram));
		if (!Result.ParameterId.IsValid()) Result.ParameterId = Id;
		return Result;
	}

	auto DMaterial::DeleteParameterDefinition(const FGuid& Id) -> FMaterialParameterEditResult
	{
		auto Candidate = ParameterDefinitions;
		if (!std::erase_if(Candidate, [&](const auto& Definition) { return Definition.Id == Id; }))
			return {EMaterialParameterError::NotFound, Id};
		auto Result = SetMaterialDefinitionsAndProgram(std::move(Candidate), Program);
		if (!Result.ParameterId.IsValid()) Result.ParameterId = Id;
		return Result;
	}

	auto DMaterial::SetMaterialGraphPresentation(
		FMaterialGraphPresentation InPresentation) -> bool
	{
		InPresentation = SanitizeMaterialGraphPresentation(
			InPresentation, Program);
		if (GraphPresentation == InPresentation) return true;
		GraphPresentation = std::move(InPresentation);
		AdvanceRevision(MaterialGraphPresentationRevision);
		MarkPackageDirty();
		return true;
	}

	auto DMaterial::ApplyMaterialGraphNodePositions(
		std::span<const FMaterialGraphNodePresentation> Positions,
		uint64 ExpectedAuthoredRevision) -> bool
	{
		if (CompilationOwner.MaterialCompileStatus.AuthoredRevision != ExpectedAuthoredRevision
			|| Positions.size() > MaterialProgramMaxNodeCount)
			return false;
		std::unordered_set<FGuid> RequestedNodes;
		RequestedNodes.reserve(Positions.size());
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			if (!Position.NodeId.IsValid()
				|| !RequestedNodes.insert(Position.NodeId).second
				|| Position.X < -MaterialGraphPresentationCoordinateLimit
				|| Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit
				|| Position.Y > MaterialGraphPresentationCoordinateLimit
				|| std::ranges::find(Program.Nodes, Position.NodeId,
					&FMaterialProgramNode::Id) == Program.Nodes.end())
				return false;
		}

		bool bChanged = false;
		for (const FMaterialGraphNodePresentation& Position : Positions)
		{
			auto It = std::ranges::lower_bound(
				GraphPresentation.Nodes, Position.NodeId, {},
				&FMaterialGraphNodePresentation::NodeId);
			if (It == GraphPresentation.Nodes.end() || It->NodeId != Position.NodeId)
			{
				GraphPresentation.Nodes.insert(It, Position);
				bChanged = true;
			}
			else if (*It != Position)
			{
				*It = Position;
				bChanged = true;
			}
		}
		if (!bChanged) return true;
		AdvanceRevision(MaterialGraphPresentationRevision);
		MarkPackageDirty();
		return true;
	}

	auto DMaterial::ApplyMaterialGraphOutputPosition(
		int32 X, int32 Y, uint64 ExpectedAuthoredRevision) -> bool
	{
		if (CompilationOwner.MaterialCompileStatus.AuthoredRevision != ExpectedAuthoredRevision
			|| X < -MaterialGraphPresentationCoordinateLimit
			|| X > MaterialGraphPresentationCoordinateLimit
			|| Y < -MaterialGraphPresentationCoordinateLimit
			|| Y > MaterialGraphPresentationCoordinateLimit)
			return false;
		if (GraphPresentation.bHasMaterialOutputPosition
			&& GraphPresentation.MaterialOutputX == X
			&& GraphPresentation.MaterialOutputY == Y)
			return true;
		GraphPresentation.bHasMaterialOutputPosition = true;
		GraphPresentation.MaterialOutputX = X;
		GraphPresentation.MaterialOutputY = Y;
		AdvanceRevision(MaterialGraphPresentationRevision);
		MarkPackageDirty();
		return true;
	}

	auto DMaterial::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return ParameterDefinitions;
	}

	auto DMaterial::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		OutParameter.Definition = Definition;
		OutParameter.Value = Definition->Value;
		OutParameter.Source = const_cast<DMaterial*>(this);
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterial::SetStaticProperties(const FMaterialStaticProperties& InProperties) -> bool
	{
		std::string Error;
		if (!ValidateMaterialStaticProperties(InProperties, Error)) return false;
		if (StaticProperties == InProperties) return true;
		const bool bShaderIdentityChanged =
			CanonicalizeMaterialShaderProperties(StaticProperties)
				!= CanonicalizeMaterialShaderProperties(InProperties);
		StaticProperties = InProperties;
		if (bShaderIdentityChanged)
		{
			AdvanceRevision(CompilationOwner.MaterialCompileStatus.AuthoredRevision);
			RequestProgramCompile(Program, StaticProperties);
		}
		InvalidateMaterialCompilation(false, true);
		MarkPackageDirty();
		MarkRenderDataDirty(
			EMaterialRenderDirtyFlags::ShaderMap
				| EMaterialRenderDirtyFlags::PipelineState);
		return true;
	}

	auto DMaterial::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.ScalarValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.Vector2Value;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.VectorValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.TextureValue;
		if (Mutable.Get() == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetParameterValue(
		const FGuid& Id, const FMaterialParameterValue& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		switch (Definition->Type)
		{
		case EMaterialParameterType::Scalar:
			return SetScalarParameterValue(Definition->Name, Value.ScalarValue);
		case EMaterialParameterType::Vector2:
			return SetVector2ParameterValue(Definition->Name, Value.Vector2Value);
		case EMaterialParameterType::Vector4:
		{
			if (!std::isfinite(Value.Vector4Value.x) || !std::isfinite(Value.Vector4Value.y)
				|| !std::isfinite(Value.Vector4Value.z) || !std::isfinite(Value.Vector4Value.w))
				return false;
			auto& Mutable = ParameterDefinitions[static_cast<size_t>(
				Definition - ParameterDefinitions.data())].Value.Vector4Value;
			if (Mutable == Value.Vector4Value) return true;
			Mutable = Value.Vector4Value;
			MarkPackageDirty();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
			return true;
		}
		case EMaterialParameterType::Vector:
			return SetVectorParameterValue(Definition->Name, Value.VectorValue);
		case EMaterialParameterType::Texture:
		{
			if (!IsValidMaterialSampling(Value.SamplerState, Value.TextureFallback)) return false;
			auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value;
			const auto Canonical = FMaterialParameterValue::MakeTexture(Value.TextureValue.Get(), Value.SamplerState, Value.TextureFallback);
			if (Mutable == Canonical) return true;
			Mutable = Canonical;
			MarkPackageDirty();
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
			return true;
		}
		}
		return false;
	}

	auto DMaterial::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		OutValue = Definition->Value.ScalarValue;
		return true;
	}

	auto DMaterial::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		OutValue = Definition->Value.Vector2Value;
		return true;
	}

	auto DMaterial::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		OutValue = Definition->Value.VectorValue;
		return true;
	}

	auto DMaterial::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		OutValue = Definition->Value.TextureValue.Get();
		return true;
	}

	auto DMaterial::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		FMaterialLocalRenderLayer Result;
		Result.CompiledProgram = GetAcceptedCompiledProgram();
		Result.StaticProperties = GetRenderableStaticProperties();
		if (!Result.CompiledProgram) return Result;
		Result.Parameters.reserve(Result.CompiledProgram->ActiveParameters.size());
		for (const auto& Parameter : Result.CompiledProgram->ActiveParameters)
		{
			const FMaterialParameterDefinition* Definition =
				FindParameterDefinition(Parameter.Id);
			if (!Definition || Definition->Type != Parameter.Type)
			{
				const auto Retained = std::ranges::find(CompilationOwner.RetainedAcceptedParameters,
					Parameter.Id, &FMaterialLocalRenderParameter::Id);
				if (Retained != CompilationOwner.RetainedAcceptedParameters.end() && Retained->Type == Parameter.Type)
					Result.Parameters.push_back(*Retained);
				continue;
			}
			Result.Parameters.push_back(
				BuildMaterialLocalRenderParameter(
					Definition->Id,
					Definition->Type,
					Definition->Value));
		}
		return Result;
	}

	auto DMaterial::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		if (!CompilationOwner.AcceptedCompiledProgram
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedProgramData.GetMetadata().LogicalSize != 0)
		{
			std::string Error;
			const_cast<DMaterial*>(this)->LoadCookedProgram(Error);
		}
		return CompilationOwner.AcceptedCompiledProgram;
	}

	auto DMaterial::PostLoad() -> void
	{
		std::string Error;
		Super::PostLoad();
		const auto Validation = ValidateMaterialParameterDefinitions(ParameterDefinitions);
		if (ParameterDeclarationSchemaVersion != 2 || !Validation)
		{
			DURIN_ERROR("PostLoad '{}': invalid or unsupported material declarations.", GetObjectPath());
			return;
		}
		if (!ValidateMaterialStaticProperties(StaticProperties, Error))
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (CookedProgramData.GetMetadata().LogicalSize == 0)
			{
				Error = std::format(
					"Cooked Material '{}': required ProgramData field is missing.",
					GetObjectPath());
				MaterialCookDiagnostic = Error;
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			CompilationOwner.AcceptedCompiledProgram.reset();
			CompilationOwner.MaterialCompileDiagnostics.clear();
			MaterialCookDiagnostic = std::format(
				"Loaded cooked Material metadata for '{}'.", GetObjectPath());
			return;
		}
		const FMaterialProgramValidationResult ProgramValidation =
			ValidateMaterialProgram(Program, ParameterDefinitions);
		if (!ProgramValidation)
		{
			Error = ProgramValidation.Diagnostics.empty()
				? "Material program validation failed."
				: ProgramValidation.Diagnostics.front().Message;
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		GraphPresentation = SanitizeMaterialGraphPresentation(
			GraphPresentation, Program);
		AdvanceRevision(MaterialProgramRevision);
		AdvanceRevision(MaterialGraphPresentationRevision);
		AdvanceRevision(ParameterDefinitionSchemaRevision);
		RequestProgramCompile(Program, StaticProperties);
		PublishMaterialRenderProxyState();
	}

	auto DMaterial::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("Program") || (Name == FName("StaticProperties")
			&& CanonicalizeMaterialShaderProperties(StaticProperties) != CompilationOwner.LastRequestedShaderProperties))
		{
			if (Name == FName("Program"))
				AdvanceRevision(MaterialProgramRevision);
			AdvanceAuthoredRevision();
			RequestProgramCompile(Program, StaticProperties);
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap);
		}
		else if (Name == FName("GraphPresentation"))
		{
			AdvanceRevision(MaterialGraphPresentationRevision);
		}
		else if (Name == FName("ParameterDefinitions"))
		{
			AdvanceRevision(ParameterDefinitionSchemaRevision);
		}
	}

	auto DMaterial::BeginDestroy() -> void
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		Super::BeginDestroy();
	}
}
