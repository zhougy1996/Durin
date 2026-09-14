#include "Materials/Material.h"
#include "Logging/LogMacros.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderTypes.h"
#include "Asset/Asset.h"
#include "DObject/Property.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Modules/ModuleManager.h"
#include "MaterialProgramValidation.h"
#include "MaterialExpressionAuthoring.h"

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

		auto MakeCodeOnlyProgram(FMaterialProgram Program) -> FMaterialProgram
		{
			for (auto& Node : Program.Nodes)
			{
				Node.Parameter = {.Id = Node.Parameter.Id, .Type = Node.Parameter.Type};
				Node.DisplayName.clear();
			}
			return Program;
		}


	}

	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, Program(MakeDefaultMaterialProgram())
		, GraphPresentation({
			.bHasMaterialOutputPosition = true,
			.MaterialOutputX = 96,
			.MaterialOutputY = 0})
	{
		ObservedCodeProgram = MakeCodeOnlyProgram(Program);
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose))
		{
			if (!IsMaterialCompilationAcceptingRequests())
				RequestProgramCompile(StaticProperties);
			PublishMaterialRenderProxyState();
		}
	}

	auto DMaterial::AdvanceAuthoredRevision() -> void
	{
		AdvanceRevision(CompilationOwner.MaterialCompileStatus.AuthoredRevision);
		InvalidateMaterialCompilation(false);
	}

	auto DMaterial::SetEditCompileMode(EMaterialEditCompileMode Mode) -> void
	{
		if (EditCompileMode == Mode) return;
		EditCompileMode = Mode;
		for (const auto Handle : GetLoadedMaterialDependents(this))
			if (auto* Owner = Cast<DMaterialInterface>(ResolveObjectHandle(Handle)); IsValid(Owner))
			{
				const auto State = Owner->GetMaterialCompileStatus().State;
				if (State == EMaterialCompileState::NeedsCompile || State == EMaterialCompileState::Scheduled)
					Private::FMaterialCompilationLifecycle::ScheduleEdit(*Owner);
			}
	}

	auto DMaterial::CompileEdits() -> bool
	{
		bool bAccepted = RequestMaterialRecompile(*this);
		for (const auto Handle : GetLoadedMaterialDependents(this))
			if (auto* Owner = Cast<DMaterialInterface>(ResolveObjectHandle(Handle));
				IsValid(Owner) && Owner != this)
				bAccepted = RequestMaterialRecompile(*Owner) && bAccepted;
		return bAccepted;
	}

	auto DMaterial::GetRenderableStaticProperties() const
		-> FMaterialStaticProperties
	{
		return Super::GetRenderableStaticProperties();
	}

	auto DMaterial::SetMaterialProgram(
		FMaterialProgram InProgram) -> FMaterialProgramValidationResult
	{
		const auto Calls = GetMaterialFunctionCalls();
		return SetMaterialProgramAndFunctionCalls(std::move(InProgram), {Calls.begin(), Calls.end()});
	}

	auto DMaterial::SetMaterialProgramAndFunctionCalls(FMaterialProgram InProgram,
		std::vector<FMaterialFunctionCall> InCalls) -> FMaterialProgramValidationResult
	{
		if (InProgram.SchemaVersion != CurrentMaterialProgramSchemaVersion)
		{
			FMaterialProgramValidationResult Validation;
			Validation.Diagnostics.push_back({
				.Category = EMaterialProgramDiagnosticCategory::Schema,
				.Message = "Material program schema version is unsupported."});
			return Validation;
		}
		std::vector<FMaterialParameterDefinition> Schema;
		auto Validation = DeriveMaterialParameterSchema(InProgram, Schema);
		if (!Validation) return Validation;
		Validation = ValidateMaterialProgramWithFunctions(InProgram, Schema, InCalls);
		if (!Validation) return Validation;
		RefreshExpressionProjection();
		if (Program == InProgram && FunctionCalls == InCalls) return Validation;
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "MaterialCandidate"));
		FMaterialExpressionCollection Candidate;
		if (!Private::ConstructMaterialExpressions(Staging.Get(), InProgram, InCalls, Candidate))
		{
			Validation.bSucceeded = false;
			Validation.Diagnostics.push_back({.Message = "Material candidate cannot be represented by supported expressions."});
			return Validation;
		}
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Candidate.Expressions) Expressions.push_back(Expression.Get());
		Validation = SetMaterialExpressions(Expressions, Private::ConstructMaterialOutputs(InProgram.Outputs));
		if (Validation)
			for (const auto& Node : InProgram.Nodes)
			{
				const auto Position = std::ranges::find(GraphPresentation.Nodes, Node.Id, &FMaterialGraphNodePresentation::NodeId);
				if (Position != GraphPresentation.Nodes.end()) Position->DisplayName = Node.DisplayName;
				else if (!Node.DisplayName.empty()) GraphPresentation.Nodes.push_back({.NodeId = Node.Id, .DisplayName = Node.DisplayName});
			}
		if (Validation) RefreshExpressionProjection();
		return Validation;
	}

	auto DMaterial::SetMaterialGraphPresentation(
		FMaterialGraphPresentation InPresentation) -> bool
	{
		InPresentation = SanitizeMaterialGraphPresentation(
			InPresentation, Program);
		for (const auto& Existing : GraphPresentation.Nodes)
		{
			if (Existing.DisplayName.empty() || std::ranges::find(Program.Nodes, Existing.NodeId, &FMaterialProgramNode::Id) == Program.Nodes.end()) continue;
			const auto Position = std::ranges::find(InPresentation.Nodes, Existing.NodeId, &FMaterialGraphNodePresentation::NodeId);
			if (Position != InPresentation.Nodes.end()) Position->DisplayName = Existing.DisplayName;
			else InPresentation.Nodes.push_back(Existing);
		}
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
			else if (It->X != Position.X || It->Y != Position.Y)
			{
				It->X = Position.X; It->Y = Position.Y;
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
		return ParameterSchema;
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
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this);
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
		const auto* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Scalar
			&& SetParameterValue(Definition->Id, FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterial::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector2
			&& SetParameterValue(Definition->Id, FMaterialParameterValue::MakeVector2(Value));
	}

	auto DMaterial::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		return Definition && Definition->Type == EMaterialParameterType::Vector
			&& SetParameterValue(Definition->Id, FMaterialParameterValue::MakeVector(Value));
	}

	auto DMaterial::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const auto* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		auto Candidate = Definition->Value;
		Candidate.TextureValue = Value;
		return SetParameterValue(Definition->Id, Candidate);
	}

	auto DMaterial::SetParameterValue(const FGuid& Id, const FMaterialParameterValue& Value) -> bool
	{
		if (!Id.IsValid()) return false;
		auto Entry = std::ranges::find(ParameterSchema, Id, &FMaterialParameterDefinition::Id);
		if (Entry == ParameterSchema.end()) return false;
		const bool bCooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
		auto Definition = *Entry;
		Definition.Value = Value;
		if (!ValidateMaterialParameterDefinitions(std::span(&Definition, 1))) return false;
		if (*Entry == Definition) return true;
		if (!bCooked)
		{
			DMaterialExpressionParameter* Owner = nullptr;
			for (const auto& Expression : ExpressionCollection.Expressions)
				if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()); Parameter && Parameter->Metadata.Id == Id) { Owner = Parameter; break; }
			if (!Owner) return false;
			if (auto* Parameter = Cast<DMaterialExpressionScalarParameter>(Owner)) Parameter->DefaultValue = Value.ScalarValue;
			else if (auto* Parameter = Cast<DMaterialExpressionVector2Parameter>(Owner)) Parameter->DefaultValue = Value.Vector2Value;
			else if (auto* Parameter = Cast<DMaterialExpressionVector3Parameter>(Owner)) Parameter->DefaultValue = Value.VectorValue;
			else if (auto* Parameter = Cast<DMaterialExpressionVector4Parameter>(Owner)) Parameter->DefaultValue = Value.Vector4Value;
			else if (auto* Parameter = Cast<DMaterialExpressionTextureParameter>(Owner)) Parameter->DefaultValue = {Value.TextureValue, Value.SamplerState, Value.TextureFallback};
			else return false;
		}

		*Entry = std::move(Definition);
		if (!bCooked) RefreshExpressionProjection();
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
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
		return Super::BuildMaterialLocalRenderLayer();
	}

	auto DMaterial::GetAcceptedCompiledProgram() const
		-> std::shared_ptr<const FMaterialCompilerResult>
	{
		return Super::GetAcceptedCompiledProgram();
	}

	auto DMaterial::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading()) GraphOwnershipVersion = 0;
		Super::Serialize(Ar);
		if (GraphOwnershipVersion != 2 || Ar.HasError())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedVersion, "Unsupported material expression schema; rebuild this material.");
			return;
		}
		if (!IsTemplateObject() && Ar.IsSaving() && Ar.GetPurpose() == EArchivePurpose::AuthoredPackage)
		{
			std::string Error;
			if (!ValidateLoadedObjectGraph({}, Error)) Ar.Fail(EArchiveFailureCode::InvalidData, Error);
		}
	}

	auto DMaterial::SerializeCooked(FArchive& Ar) -> void
	{
		RefreshExpressionProjection();
		if (Ar.IsSaving() && !GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& !DeriveMaterialParameterSchema(Program, ParameterSchema))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Cannot Cook an invalid material parameter schema.");
			return;
		}
		Super::SerializeCooked(Ar);
		if (Ar.HasError()) return;
		auto* Property = StaticClass()->FindPropertyByName(FName("ParameterSchema"));
		require(Property);
		SerializeReflectedPropertyValue(Ar, *Property, this);
		if (!Ar.HasError() && !ValidateMaterialParameterDefinitions(ParameterSchema))
			Ar.Fail(EArchiveFailureCode::InvalidData, "Invalid generated cooked material parameter schema.");
	}

	auto DMaterial::PostLoad() -> void
	{
		std::string Error;
		Super::PostLoad();
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
			CompilationOwner.RenderLayer.CompiledProgram.reset();
			CompilationOwner.MaterialCompileDiagnostics.clear();
			MaterialCookDiagnostic = std::format(
				"Loaded cooked Material metadata for '{}'.", GetObjectPath());
			return;
		}
		RefreshExpressionProjection();
		const auto SchemaValidation = DeriveMaterialParameterSchema(Program, ParameterSchema);
		if (!SchemaValidation)
		{
			DURIN_ERROR("PostLoad '{}': unsupported material graph; rebuild this material.", GetObjectPath());
			return;
		}
		const FMaterialProgramValidationResult ProgramValidation =
			ValidateMaterialProgramWithFunctions(Program, ParameterSchema, FunctionCalls);
		if (!ProgramValidation)
		{
			Error = ProgramValidation.Diagnostics.empty()
				? "Material program validation failed."
				: ProgramValidation.Diagnostics.front().Message;
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		ObservedCodeProgram = MakeCodeOnlyProgram(Program);
		GraphPresentation = SanitizeMaterialGraphPresentation(
			GraphPresentation, Program);
		AdvanceRevision(MaterialProgramRevision);
		AdvanceRevision(MaterialGraphPresentationRevision);
		AdvanceRevision(ParameterDefinitionSchemaRevision);
		RequestProgramCompile(StaticProperties);
		PublishMaterialRenderProxyState();
	}

	auto DMaterial::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("StaticProperties")) InvalidateMaterialCompilation(false, true);
		if (Name == FName("ExpressionCollection") || Name == FName("ExpressionOutputs") || (Name == FName("StaticProperties")
			&& CanonicalizeMaterialShaderProperties(StaticProperties) != CompilationOwner.LastObservedShaderProperties))
		{
			if (Name == FName("ExpressionCollection") || Name == FName("ExpressionOutputs"))
			{
				const auto PreviousCalls = FunctionCalls;
				RefreshExpressionProjection();
				std::vector<FMaterialParameterDefinition> Schema;
				if (!DeriveMaterialParameterSchema(Program, Schema)
					|| !ValidateMaterialProgramWithFunctions(Program, Schema, FunctionCalls)) return;
				ParameterSchema = std::move(Schema);
				AdvanceRevision(ParameterDefinitionSchemaRevision);
				auto CodeProgram = MakeCodeOnlyProgram(Program);
				const bool bShaderChanged = CodeProgram != ObservedCodeProgram || PreviousCalls != FunctionCalls;
				ObservedCodeProgram = std::move(CodeProgram);
				if (!bShaderChanged)
				{
					MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
					return;
				}
				AdvanceRevision(MaterialProgramRevision);
			}
			AdvanceAuthoredRevision();
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this);
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap);
		}
		else if (Name == FName("GraphPresentation"))
		{
			AdvanceRevision(MaterialGraphPresentationRevision);
		}

	}

	auto DMaterial::BeginDestroy() -> void
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		Super::BeginDestroy();
	}
}
