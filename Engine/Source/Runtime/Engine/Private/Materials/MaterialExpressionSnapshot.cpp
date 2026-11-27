#include "MaterialExpressionGraphBuilder.h"
#include "MaterialPreparedProgram.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialShaderMap.h"
#include "Asset/Asset.h"

#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstance.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto MIR::FGraphBuilderImpl::BuildCustomOutputs(std::optional<EMaterialShadingModel> Model) -> void
	{
		std::set<std::string_view> Seen;
		for (const auto& [Id, Expression] : Expressions)
		{
			const auto* Output = Cast<DMaterialExpressionCustomOutput>(Expression);
			if (!Output) continue;
			const auto Name = Output->GetFunctionName();
			if (!Seen.insert(Name).second) { Fail(std::format("Duplicate custom material output {}.", Name)); return; }
			if (bValidateAuthoring) AuthoringCodeHash.UpdateValue(Id);
			const auto Families = GetMaterialCustomOutputs();
			const auto Family = std::ranges::find(Families, Name, &FMaterialCustomOutputDefinition::FunctionName);
			if (Family == Families.end()) { Fail(std::format("Unknown custom material output {}.", Name)); return; }
			if (Model && Family->ShadingModel != *Model)
			{ Fail(std::format("{} is incompatible with the selected shading model.", Name)); return; }
			if (Output->GetNumOutputs() != static_cast<int32>(Family->Outputs.size()))
			{ Fail(std::format("{} declares an incompatible number of outputs.", Name)); return; }
			BuildExpression(*Output);
			if (!Result.Diagnostics.empty()) return;
			for (const auto& Slot : Family->Outputs)
				if (!std::ranges::contains(Result.IR.CustomOutputRoots, Slot.Id, &FModule::FAttributeRoot::AttributeID))
				{ Fail(std::format("{} did not emit {}.", Name, Slot.Name)); return; }
		}
		for (const auto& Family : GetMaterialCustomOutputs())
			if (Model == Family.ShadingModel && !Seen.contains(Family.FunctionName))
				Fail(std::format("The selected shading model requires {}.", Family.FunctionName));
	}

	template<typename RootOwner>
	auto MIR::FGraphBuilderImpl::FinishSurface(const RootOwner& Outputs,
		EMaterialDomain Domain, const std::set<EMaterialProperty>* Demanded, std::optional<EMaterialShadingModel> Model)
		-> MIR::FBuildResult
	{
		Result.IR.Domain = Domain;
		const auto& Attributes = *Outputs.GetExpressionInputDescription(EMaterialProperty::MP_MaterialAttributes)->Connection;
		const bool UseAttributes = Outputs.UsesMaterialAttributes();
		if (bValidateAuthoring) BuildAllExpressions();
		if (bValidateAuthoring)
		{
			// Retained wiring must stay structurally valid even when its mode is inactive.
			if (!UseAttributes && Attributes.IsConnected()) ResolveIndex(Attributes);
			if (UseAttributes)
				for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
					if (const auto& Link = *Outputs.GetExpressionInputForProperty(Field.Property); Link.IsConnected()) ResolveIndex(Link);
		}
		if ((!Demanded || UseAttributes) && !Attributes.IsConnected() && Attributes.OutputIndex != 0)
		{ Fail("Disconnected material attributes input has an output selector."); return Finish({}); }
		AuthoringCodeHash.UpdateValue(UseAttributes);
		uint32 Aggregate = MIR::InvalidIndex;
		if (UseAttributes && Attributes.IsConnected() && (!Demanded || !Demanded->empty()))
		{
			Aggregate = ResolveIndex(Attributes);
			if (Aggregate != MIR::InvalidIndex && GetNode(Aggregate).ResultType != EMaterialProgramValueType::MaterialAttributes)
				Fail("Aggregate material output requires a Surface expression.");
		}
		for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
		{
			if (Demanded && !Demanded->contains(Definition.Property)) continue;
			auto& Root = Result.IR.GetAttribute(Definition.Property);
			uint32 Index = MIR::InvalidIndex;
			if (!UseAttributes || !Demanded)
			{
				const auto Description = Outputs.GetExpressionInputDescription(Definition.Property);
				const auto& Stored = *Description->Numeric;
				const auto& Link = Stored.Connection;
				const auto Width = uint32(Description->Type) + 1;
				if (Stored.Constant.size() != Width || !std::ranges::all_of(Stored.Constant, [](float V) { return std::isfinite(V); })
					|| (!Link.IsConnected() && Link.OutputIndex != 0))
				{ Fail("Material attribute has an invalid selector or retained default."); break; }
				if (bValidateAuthoring)
				{
					AuthoringCodeHash.UpdateValue(Definition.AttributeID);
					AuthoringCodeHash.UpdateValue(Stored.UseConstant);
					for (float V : Stored.Constant) AuthoringCodeHash.UpdateValue(V);
				}
				if (!UseAttributes)
				{
					if (SupportsMaterialRootConstant(Definition.Property) && Stored.UseConstant) Root.Literal = {Stored.Constant[0], Width > 1 ? Stored.Constant[1] : 0.f,
						Width > 2 ? Stored.Constant[2] : 0.f, Width > 3 ? Stored.Constant[3] : 0.f};
					if (Link.IsConnected()) Index = ResolveIndex(Link);
				}
			}
			if (Aggregate != MIR::InvalidIndex) Index = Attribute(Aggregate, Definition.Property);
			if (Index != MIR::InvalidIndex && MaterialInputAllowsValidCast(
				EMaterialProgramOpcode::MakeMaterialAttributes, uint32(Definition.Property)))
				Index = *TryValidCast(Index, Root.Type).GetIndex();
			if (Index == MIR::InvalidIndex || !Result.Diagnostics.empty()) continue;
			const auto Actual = GetNode(Index).GetSemantics();
			const bool Compatible = Definition.Property == EMaterialProperty::MP_WorldPositionOffset
				? IsMaterialWorldPositionOffsetSemantics(Actual)
				: Actual.Type == Definition.Semantics.Type && Actual.Kind == Definition.Semantics.Kind
					&& Actual.Space == Definition.Semantics.Space && MaterialStagesContain(Actual.Stages, Definition.Semantics.Stages);
			if (!Compatible)
			{
				Result.Diagnostics.push_back({.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					.LocationIndex = uint32(Definition.Property),
					.Message = Actual.Type != Root.Type
						? std::format("Material output source has an incompatible type. [expected type {}] [actual type {}]", uint8(Root.Type), uint8(Actual.Type))
						: "Material output source has an incompatible evaluation stage, spatial kind, or coordinate space.",
					.AttributeID = Definition.AttributeID});
				break;
			}
			Root.bExpression = true; Root.ExpressionIndex = Index;
		}

		if (Result.Diagnostics.empty()) BuildCustomOutputs(Model);
		return Finish({});
	}

	template<typename RootOwner>
	auto MIR::FGraphBuilderImpl::ValidateSurface(std::span<DMaterialExpression* const> Expressions,
		const RootOwner& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		MIR::FGraphBuilderImpl Context(Expressions);
		Context.bValidateAuthoring = true;
		auto Built = Context.FinishSurface(Outputs);
		if (Built && OutCodeFingerprint)
		{
			// A transient edit checkpoint, not shader identity or a persisted encoding.
			// Hash fields individually: no object pointers, aggregate padding, parameter defaults or presentation.
			auto& Hash = Context.AuthoringCodeHash;
			const auto Literal = [&](const FMaterialProgramLiteral& Value) {
				Hash.UpdateValue(Value.X); Hash.UpdateValue(Value.Y);
				Hash.UpdateValue(Value.Z); Hash.UpdateValue(Value.W);
			};
			Hash.UpdateValue(static_cast<uint32>(Built.IR.Nodes.size()));
			for (const auto& Node : Built.IR.Nodes)
			{
				Hash.UpdateValue(Node.Opcode); Hash.UpdateValue(Node.ResultType);
				Hash.UpdateValue(Node.LegalStages); Hash.UpdateValue(Node.SpatialKind);
				Hash.UpdateValue(Node.CoordinateSpace);
				Hash.UpdateValue(Node.ScalarBroadcastMask);
				Hash.UpdateValue(static_cast<uint32>(Node.Inputs.size()));
				for (const auto Input : Node.Inputs) Hash.UpdateValue(Input);
				Hash.UpdateValue(static_cast<uint32>(Node.Payload.index()));
				if (const auto* Value = std::get_if<FMaterialProgramLiteral>(&Node.Payload)) Literal(*Value);
				else if (const auto* Id = std::get_if<FGuid>(&Node.Payload)) Hash.UpdateValue(*Id);
				else if (const auto* Read = std::get_if<MIR::FTextureRead>(&Node.Payload)) Hash.UpdateValue(Read->Mode);
				else if (const auto* Read = std::get_if<MIR::FCustomDataRead>(&Node.Payload)) Hash.UpdateValue(Read->Index);
				else if (const auto* Read = std::get_if<MIR::FSceneTextureRead>(&Node.Payload)) { Hash.UpdateValue(Read->SceneTextureId); Hash.UpdateValue(Read->Filter); }
				else if (const auto* Collection = std::get_if<MIR::FCollectionParameter>(&Node.Payload))
				{
					Hash.UpdateValue(Collection->CollectionId);
					Hash.UpdateValue(Collection->ParameterId);
				}
				else if (const auto* Swizzle = std::get_if<MIR::FSwizzle>(&Node.Payload))
				{
					Hash.UpdateValue(Swizzle->Length);
					for (const auto Component : Swizzle->Components) Hash.UpdateValue(Component);
				}
				else if (const auto* Transform = std::get_if<FMaterialTransformPayload>(&Node.Payload))
				{
					Hash.UpdateValue(Transform->Source);
					Hash.UpdateValue(Transform->Destination);
				}
			}
			Hash.UpdateValue(Built.IR.bAggregateAttributes);
			Hash.UpdateValue(Built.IR.AggregateAttributesIndex);
			for (const auto& Input : Built.IR.GetRoots())
			{
				Hash.UpdateValue(Input.AttributeID);
				Hash.UpdateValue(Input.bExpression); Hash.UpdateValue(Input.ExpressionIndex);
				Hash.UpdateValue(Input.Type); Hash.UpdateValue(Input.LegalStages);
				Hash.UpdateValue(Input.SpatialKind); Hash.UpdateValue(Input.CoordinateSpace);
				Literal(Input.Literal);
			}
			*OutCodeFingerprint = Hash.Finalize();
		}
		return {.bSucceeded = static_cast<bool>(Built), .Diagnostics = std::move(Built.Diagnostics)};
	}

	auto MIR::FGraphBuilder::FinishSurface(const FMaterialRootInputs& Outputs) -> MIR::FBuildResult
	{
		return Impl->FinishSurface(Outputs);
	}

	auto MIR::FGraphBuilder::ValidateSurface(std::span<DMaterialExpression* const> Expressions,
		const FMaterialRootInputs& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		return FGraphBuilderImpl::ValidateSurface(Expressions, Outputs, OutCodeFingerprint);
	}

#if DURIN_WITH_EDITORONLY_DATA
	static auto ValidateOwnedExpressionGraph(const FMaterialExpressionCollection& Collection,
		const auto& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Collection.Expressions) Expressions.push_back(Expression.Get());
		const std::unordered_set<DMaterialExpression*> Members(Expressions.begin(), Expressions.end());
		const auto InBody = [&](const FMaterialExpressionInput& Link) {
			return Link.OutputIndex >= 0 && (!Link.Expression || Members.contains(Link.Expression.Get()));
		};
		if (!InBody(*Outputs.GetExpressionInputDescription(EMaterialProperty::MP_MaterialAttributes)->Connection))
			return {.Diagnostics = {{.Message = "Material Attributes refers outside the material body or has a negative selector."}}};
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			if (!InBody(*Outputs.GetExpressionInputForProperty(Field.Property)))
				return {.Diagnostics = {{.Message = "Property input refers outside the material body or has a negative selector.", .AttributeID = Field.AttributeID}}};
		return MIR::FGraphBuilderImpl::ValidateSurface(Expressions, Outputs, OutCodeFingerprint);
	}

	auto DMaterial::ValidateExpressionGraph(FXxHash128* Fingerprint) const -> FMaterialProgramValidationResult
	{
		return ValidateOwnedExpressionGraph(ExpressionCollection, *this, Fingerprint);
	}

	auto DMaterial::ValidateExpressionGraph(const FMaterialExpressionCollection& Collection,
		const FMaterialRootInputs& Outputs, FXxHash128* Fingerprint) -> FMaterialProgramValidationResult
	{
		return ValidateOwnedExpressionGraph(Collection, Outputs, Fingerprint);
	}
#endif

	enum class EMaterialCapturePurpose { Compilation, ParameterReachability };

	static auto CaptureMaterialInputs(const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment,
		EMaterialCapturePurpose Purpose)
		-> FMaterialCompilerSnapshotResult
	{
		check(IsInGameThread());
		FMaterialCompilerSnapshotResult Result;
#if !DURIN_WITH_EDITORONLY_DATA
		Result.Diagnostics.push_back({.Message = "Material has no typed expression owner."});
		return Result;
#else
		const DMaterialInterface* Root = &Material;
		for (uint32 Depth = 0; Root && Root->GetParent() && Depth < MaterialMaximumParentDepth; ++Depth) Root = Root->GetParent();
		const auto* Owner = Cast<DMaterial>(Root);
		if (!Owner)
		{
			Result.Diagnostics.push_back({.Message = "Material has no typed expression owner."});
			return Result;
		}
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Owner->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		std::vector<FMaterialCompilerEnvironment::FStaticBoolValue> StaticBools;
		std::map<FGuid, std::pair<FName, bool>> StaticDeclarations;
		std::unordered_map<FName, FGuid> StaticNames;
		for (const auto* Expression : Expressions)
			if (const auto* Declaration = Cast<DMaterialExpressionStaticBool>(Expression))
			{
				const auto Existing = StaticDeclarations.find(Declaration->DeclarationId);
				const auto ExistingName = StaticNames.find(Declaration->Name);
				if (!Declaration->DeclarationId.IsValid() || Declaration->Name.IsNone()
					|| (Existing == StaticDeclarations.end()
						&& StaticDeclarations.size() >= MaterialMaxStaticBoolDeclarations)
					|| (Existing != StaticDeclarations.end()
						&& Existing->second != std::pair{Declaration->Name, Declaration->DefaultValue})
					|| (ExistingName != StaticNames.end()
						&& ExistingName->second != Declaration->DeclarationId))
				{
					Result.Diagnostics.push_back({.Message = "Static bool declarations require unique valid GUIDs and names and are bounded to 32 per root."});
					return Result;
				}
				if (Existing == StaticDeclarations.end())
				{
					StaticDeclarations.emplace(Declaration->DeclarationId,
						std::pair{Declaration->Name, Declaration->DefaultValue});
					StaticNames.emplace(Declaration->Name, Declaration->DeclarationId);
					StaticBools.push_back({Declaration->DeclarationId, Declaration->DefaultValue});
				}
			}
		std::vector<const DMaterialInstance*> Instances;
		for (const DMaterialInterface* Layer = &Material; Layer && Layer != Owner; Layer = Layer->GetParent())
			if (const auto* Instance = Cast<DMaterialInstance>(Layer)) Instances.push_back(Instance);
		for (const auto* Instance : Instances | std::views::reverse)
			for (const auto& Override : Instance->GetStaticBoolOverrides())
			{
				if (!Override.bOverride) continue;
				auto Id = Override.DeclarationId;
				if (Override.ParameterInfo != FMaterialParameterInfo{})
				{
					const auto Named = StaticNames.find(Override.ParameterInfo.Name);
					Id = Override.ParameterInfo.IsGlobal() && Named != StaticNames.end() ? Named->second : FGuid{};
				}
				auto Found = std::ranges::find(StaticBools, Id,
					&FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId);
				if (Found == StaticBools.end())
				{
					Result.Diagnostics.push_back({.Message = "A static bool override does not match a root declaration."});
					return Result;
				}
				Found->Value = Override.Value;
			}
		std::ranges::sort(StaticBools, {}, &FMaterialCompilerEnvironment::FStaticBoolValue::DeclarationId);
		Environment.StaticBools = StaticBools;
		const bool ForCompilation = Purpose == EMaterialCapturePurpose::Compilation;
		const auto Properties = Material.GetStaticProperties();
		const auto Domain = Owner->GetDomain();
		const auto Demanded = ForCompilation
			? std::optional(GetMaterialCompilationProperties(Domain, Properties, Environment)) : std::nullopt;
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		MIR::FGraphBuilderImpl Context(Expressions, {.FindFunction = [&](const DMaterialFunctionInterface& Function)
			-> std::optional<MIR::FFunctionBody> {
			const auto* Concrete = Cast<DMaterialFunction>(&Function);
			if (!Concrete) return std::nullopt;
			Owners.push_back({FObjectKey(const_cast<DMaterialFunction*>(Concrete)), Concrete->GetObjectPath(), Concrete->GetFunctionRevision()});
			return Concrete->GetExpressionBody();
		}, .Quality = Environment.Quality, .FeatureLevel = Environment.FeatureLevel,
			.StaticBools = Environment.StaticBools}, ForCompilation);
		auto Built = Context.FinishSurface(*Owner, Domain, Demanded ? &*Demanded : nullptr,
			ForCompilation ? std::optional{Properties.ShadingModel} : std::nullopt);
		if (!Built)
		{
			Result.Diagnostics = std::move(Built.Diagnostics);
			return Result;
		}
		MIR::FCompilerInput Snapshot{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
			.Collections = std::move(Built.Collections),
			.StaticProperties = Properties, .Environment = std::move(Environment), .Sources = std::move(Built.Sources)};
		std::ranges::sort(Snapshot.Environment.Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		Result.Snapshot.emplace(FMaterialCompilerSnapshot{std::move(Snapshot), std::move(Owners)});
		return Result;
#endif
	}

	auto SnapshotMaterialCompilerInput(const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment)
		-> FMaterialCompilerSnapshotResult
	{
		return CaptureMaterialInputs(Material, std::move(Environment), EMaterialCapturePurpose::Compilation);
	}

	auto AreMaterialFunctionOwnersCurrent(std::span<const FMaterialFunctionOwnerStamp> Owners) -> bool
	{
		check(IsInGameThread());
		for (const auto& Stamp : Owners)
		{
			const auto* Owner = Cast<DMaterialFunctionInterface>(ResolveObjectKey(Stamp.Owner));
			if (!IsValid(Owner) || Owner->GetFunctionRevision() != Stamp.Revision || Owner->GetObjectPath() != Stamp.AssetPath) return false;
		}
		return true;
	}

	auto DMaterialInterface::GetParameterReachability() const
		-> std::shared_ptr<const FMaterialParameterReachability>
	{
		check(IsInGameThread());
		if (IsDynamicInstance() || GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			const auto Program = GetAcceptedCompiledProgram();
			if (Program && ParameterReachability && ParameterReachabilityCookedProgram.lock() == Program)
				return ParameterReachability;
			auto Result = std::make_shared<FMaterialParameterReachability>();
			if (!Program)
			{
				Result->Validation.Diagnostics.push_back({.Message = "Material has no accepted compiled program."});
				return Result;
			}
			for (const auto& Parameter : Program->GetShaderMap().ActiveParameters) Result->ParameterIds.insert(Parameter.Id);
			Result->Validation.bSucceeded = true;
			ParameterReachabilityProgramRevision = 0;
			ParameterReachabilityFunctionOwners.clear();
			ParameterReachabilityCookedProgram = Program;
			ParameterReachability = Result;
			return Result;
		}

		FResolvedMaterialProperties Properties;
		const auto Error = ResolveMaterialProperties(*this, Properties);
		if (!Error)
		{
			auto Result = std::make_shared<FMaterialParameterReachability>();
			Result->Validation.Diagnostics.push_back({.Message = std::move(Error.error())});
			return Result;
		}
		const auto* Root = Cast<DMaterial>(ResolveObjectKey(Properties.Root));
		// Structural reachability is independent of instance values and static properties.
		// Store it on the graph owner so all instances share the same analysis.
		if (Root != this) return Root->GetParameterReachability();
		const auto Revision = Root->GetMaterialProgramRevision();
		if (ParameterReachability && ParameterReachabilityProgramRevision == Revision
			&& AreMaterialFunctionOwnersCurrent(ParameterReachabilityFunctionOwners)) return ParameterReachability;

		auto Result = std::make_shared<FMaterialParameterReachability>();
		auto Capture = CaptureMaterialInputs(*this, {}, EMaterialCapturePurpose::ParameterReachability);
		Result->Validation = {.bSucceeded = static_cast<bool>(Capture), .Diagnostics = std::move(Capture.Diagnostics)};
		if (!Capture) return Result;
		const auto& Snapshot = Capture.Snapshot->Input;
		std::vector<uint32> Pending;
		if (Snapshot.IR.bAggregateAttributes) Pending.push_back(Snapshot.IR.AggregateAttributesIndex);
		for (const auto& Input : Snapshot.IR.GetRoots())
			if (Input.bExpression) Pending.push_back(Input.ExpressionIndex);
		std::vector<bool> Visited(Snapshot.IR.Nodes.size());
		while (!Pending.empty())
		{
			const auto Index = Pending.back(); Pending.pop_back();
			if (Visited[Index]) continue;
			Visited[Index] = true;
			const auto& Node = Snapshot.IR.Nodes[Index];
			const auto Id = Node.GetParameterId();
			if (Id.IsValid()) Result->ParameterIds.insert(Id);
			Pending.insert(Pending.end(), Node.Inputs.begin(), Node.Inputs.end());
		}
		ParameterReachabilityCookedProgram.reset();
		ParameterReachabilityProgramRevision = Revision;
		ParameterReachabilityFunctionOwners = std::move(Capture.Snapshot->FunctionOwners);
		ParameterReachability = Result;
		return Result;
	}
}
