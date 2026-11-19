#include "MaterialExpressionParameters.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/UnifiedError.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "Materials/MaterialExpressionInputs.h"
#include "MaterialGraphExpressionRegistry.h"
#include "MaterialGraphEditSession.h"
#include "DObject/Package.h"
#include "MaterialGraphValueTypes.h"
#include "Asset/Asset.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{

		class FMaterialConfigurationChange final : public FMaterialCommandChange
		{
		public:
			FMaterialConfigurationChange(DMaterial& Target, FMaterialConfiguration InBefore, FMaterialConfiguration InAfter)
				: Material(&Target), Before(InBefore), After(InAfter), Packages{Target.GetPackage()} {}
			auto GetDescription() const -> std::string_view override { return "Change Material Configuration"; }
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto GetModifiedObjects() const -> std::vector<DObject*> override { return {Material.Get()}; }
			auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult override
			{
				if (!Material.IsValid()) return {{.Code = EMaterialChangeError::TargetUnavailable}};
				const auto Result = Material.Get()->SetMaterialConfiguration(Operation == ETransactionOperation::Undo ? Before : After);
				if (!Result) return {{.Code = EMaterialChangeError::MaterialWrite, .Message = FormatMaterialProgramDiagnostics(Result.Diagnostics)}};
				return {};
			}
		private:
			TWeakObjectPtr<DMaterial> Material;
			FMaterialConfiguration Before, After;
			std::array<DPackage*, 1> Packages;
		};

		struct FCreationFunctionResult
		{
			DMaterialFunctionInterface* Function = nullptr;
			std::string Message;
			explicit operator bool() const { return Function != nullptr; }
		};
		auto LoadCreationFunction(const std::string& Name) -> FCreationFunctionResult
		{
			std::optional<Durin::UnifiedError::FError> LoadedError;
			FTopLevelAssetPath Path;
			if (const auto Parsed = FTopLevelAssetPath::TryCreateWithDiagnostic(Name, Path); !Parsed)
				return {.Message = "The material function path is invalid. " + ToString(Parsed.error())};
			DMaterialFunctionInterface* Function = nullptr;
			if (auto Loaded = LoadObject<DMaterialFunctionInterface>(Path, nullptr, &LoadedError); !Loaded)
				return {.Message = "Unable to load the material function. " + LoadedError->ToString()};
			else { Function = Loaded; }
			return {.Function = Function};
		}

	}
	auto FMaterialGraphSchema::ForOwner(const DObject& Owner) -> const FMaterialGraphSchema&
	{
		static const FMaterialSchema Material;
		static const FMaterialFunctionSchema Function;
		if (Cast<DMaterialFunction>(&Owner)) return Function;
		if (Cast<DMaterial>(&Owner)) return Material;
		check(false); std::terminate();
	}

	auto FMaterialGraphSchema::CanCreate(const FMaterialGraphCreationAction& Action,
		std::optional<EMaterialProgramValueType> SourceType) const -> bool
	{
		if (!AllowsCreation(Action, SourceType)) return false;
		if (std::holds_alternative<FMaterialGraphCollectionParameterCreation>(
			Action.Payload)) return !SourceType;
		if (!std::holds_alternative<std::string>(Action.Payload)) return true;
		if (!SourceType) return true;
		const auto Loaded = LoadCreationFunction(std::get<std::string>(Action.Payload));
		return Loaded && std::ranges::any_of(Loaded.Function->GetFunctionSignature().Inputs,
			[&](const auto& Port) { return FMaterialGraphSchema::AcceptsPort(Port.Type, *SourceType); });
	}
	auto FMaterialGraphDocument::Create(const FMaterialGraphCreationRequest& Request,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FMaterialProgramLink Source;
		std::optional<EMaterialProgramValueType> SourceType;
		if (Request.Source)
		{
			const auto& Address = *Request.Source;
			if ((Address.Kind != EMaterialGraphPinKind::Output && Address.Kind != EMaterialGraphPinKind::FunctionOutput)
				|| (Address.Kind == EMaterialGraphPinKind::FunctionOutput) != Address.PortId.IsValid())
				return RejectCommand("Creation requires an output pin.");
			const auto View = ProjectMaterialGraph(GetOwner());
			for (const auto& Node : View.Nodes)
				if (Node.Node.Id == Address.NodeId)
					for (const auto& Pin : Node.Outputs)
						if (Pin.OutputIndex == Address.Index && Pin.PortId == Address.PortId && !Pin.bMissing)
							SourceType = Pin.Type;
			if (!SourceType) return RejectCommand("The source output no longer exists.");
			Source = {Address.NodeId, static_cast<uint8>(Address.Index), Address.PortId};
		}

		if (!Schema.AllowsCreation(Request.Action, SourceType))
			return RejectCommand("This action is not compatible with the graph or source output.");
		if (const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Request.Action.Payload))
			return CreateCatalogNode(*Entry, Request.X, Request.Y, Source, Mode);
		if (const auto* Spec = std::get_if<FMaterialGraphPortCreation>(&Request.Action.Payload))
		{
			const auto* Function = Cast<DMaterialFunction>(Owner.Get());
			if (!Function) return RejectCommand("Only function graphs have ports.");
			const auto& Signature = Function->GetFunctionSignature();
			const auto& Ports = Spec->bOutput ? Signature.Outputs : Signature.Inputs;
			FMaterialFunctionPort Port;
			Port.Type = Spec->Type;
			for (uint32 Index = 1;; ++Index)
			{
				Port.Name = std::format("{} {}", Spec->bOutput ? "Output" : "Input", Index);
				if (std::ranges::none_of(Ports, [&](const auto& Existing) { return Existing.Name == Port.Name; })) break;
			}
			if (!Spec->bOutput)
				Port.Default.Kind = Port.Type == EMaterialProgramValueType::MaterialAttributes ? EMaterialFunctionDefaultKind::MaterialAttributes
					: Port.Type == EMaterialProgramValueType::Texture2D ? EMaterialFunctionDefaultKind::Texture
					: EMaterialFunctionDefaultKind::Numeric;
			return AddPort(Spec->bOutput, Port, Source, Request.X, Request.Y, Mode);
		}
		if (const auto* Spec = std::get_if<
			FMaterialGraphCollectionParameterCreation>(&Request.Action.Payload))
		{
			if (SourceType) return RejectCommand(
				"Collection parameters do not accept an input connection.");
			FTopLevelAssetPath Path;
			if (!FTopLevelAssetPath::TryCreateWithDiagnostic(
				Spec->CollectionPath, Path))
				return RejectCommand("The collection asset path is invalid.");
			auto Loaded = LoadObject<DMaterialParameterCollection>(Path);
			if (!Loaded) return RejectCommand(
				"The collection asset could not be loaded.");
			if (!Loaded->FindDeclaration(Spec->ParameterId))
				return RejectCommand("The selected collection declaration no longer exists.");
			TStrongObjectPtr<DMaterialExpressionCollectionParameter> Expression(
				NewObject<DMaterialExpressionCollectionParameter>(nullptr, NAME_None));
			Expression->Id = FGuid::NewGuid();
			Expression->Collection = Loaded;
			Expression->ParameterId = Spec->ParameterId;
			return CreateExpression(*Expression, Request.X, Request.Y, Mode);
		}
		auto Loaded = LoadCreationFunction(std::get<std::string>(Request.Action.Payload));
		if (!Loaded)
		{
			return RejectCommand(std::move(Loaded.Message));
		}
		auto* Function = Loaded.Function;
		std::vector<FMaterialFunctionInputBinding> Inputs;
		if (SourceType)
		{
			for (const auto& Port : Function->GetFunctionSignature().Inputs)
				if (FMaterialGraphSchema::AcceptsPort(Port.Type, *SourceType)) { Inputs.push_back({Port.Id, Port.Type, Source}); break; }
			if (Inputs.empty()) return RejectCommand("This function has no compatible input.");
		}
		return InsertFunctionCall(*Function, Request.X, Request.Y, Inputs, Mode);
	}
	namespace GraphEditInternals
	{
		auto FExpressionEdit::Commit(DObject& Owner, DTransactor* Transactions, std::string Description) -> FMaterialGraphCommandResult
		{
			const bool Nested = Transactions && Transactions->IsActive();
			FScopedTransaction Recording(Transactions, {"MaterialGraph", std::move(Description), &Owner});
			if (Transactions)
			{
				if (!Recording.IsOutstanding()) return RejectCommand("Unable to start the graph transaction.");
				FScopedPackageDirtySuppression Suppress;
				for (auto* Object : Objects)
				{
					Object->SetFlags(EObjectFlags::Transactional);
					if (!Object->Modify(false))
					{
						if (Nested) (void)Recording.End();
						else Recording.Cancel();
						return RejectCommand("Unable to record the expression.");
					}
				}
			}
			for (auto& Write : Writes) Write();
			// The transactor advances the package checkpoint when it finalizes.
			(void)Recording.End();
			if (Transactions && !Transactions->GetLastResult().IsSuccess())
			{
				for (auto& Write : Writes | std::views::reverse) Write();
				return RejectCommand(FormatTransactorResult(Transactions->GetLastResult()));
			}
			if (!Transactions) Owner.MarkPackageDirty();
			Objects.front()->PostEditChange();
			return {};
		}

		auto FExpressionEdit::CommitRename(DObject& Owner, DMaterialExpression& Expression, std::string NewName,
			DTransactor* Transactions) -> FMaterialGraphCommandResult
		{
			auto SwapName = [&Expression, Next = std::move(NewName)]() mutable {
				auto Previous = Expression.GetEditableName();
				Expression.SetEditableName(std::move(Next));
				Next = std::move(Previous);
			};
			// Validate the candidate through the asset before Modify captures the original expression.
			SwapName();
			auto Valid = ValidateGraphEdit(Owner);
			SwapName();
			if (!Valid) return Valid;
			Objects.push_back(&Expression);
			Writes.emplace_back(std::move(SwapName));
			return Commit(Owner, Transactions, "Rename Material Expression");
		}

		auto IncludeCallOutput(FGraphEditSession& State, DMaterialExpression* Source, FGuid OutputId) -> void
		{
			auto* Call = Cast<DMaterialExpressionFunctionCall>(Source);
			if (!OutputId.IsValid() || !Call || !Call->Function.IsValid()
				|| std::ranges::contains(Call->Outputs, OutputId, &FMaterialFunctionOutputBinding::OutputId)) return;
			const auto& Ports = Call->Function->GetFunctionSignature().Outputs;
			if (const auto Port = std::ranges::find(Ports, OutputId, &FMaterialFunctionPort::Id); Port != Ports.end())
			{
				State.Modify(*Call);
				Call->Outputs.push_back({Port->Id, Port->Type});
			}
		}

	}

	FMaterialGraphDocument::FMaterialGraphDocument(DObject& InOwner)
		: Owner(&InOwner), Schema(FMaterialGraphSchema::ForOwner(InOwner)) {}

	auto FMaterialGraphDocument::AddPort(bool bOutput, FMaterialFunctionPort Port,
		FMaterialProgramLink Source, int32 X, int32 Y, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return RejectCommand("Only function documents have an interface.");
		FGraphEditSession State(*Function);
		if (!Port.Id.IsValid()) Port.Id = FGuid::NewGuid();
		const auto NodeId = FGuid::NewGuid();
		if (bOutput)
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionOutput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->Port = std::move(Port);
			auto* Upstream = FindMaterialExpression(State.Expressions, Source.SourceNodeId);
			if (Source.SourceNodeId.IsValid() && !Upstream) return RejectCommand("The source expression no longer exists.");
			IncludeCallOutput(State, Upstream, Source.SourceOutputId);
			Terminal->Source = {Upstream, Source.SourceOutputIndex, Source.SourceOutputId};
			State.Expressions.emplace_back(Terminal);
		}
		else
		{
			auto* Terminal = NewObject<DMaterialExpressionFunctionInput>(nullptr, NAME_None);
			Terminal->Id = NodeId; Terminal->Port = std::move(Port);
			State.Expressions.emplace_back(Terminal);
		}
		State.Presentation.Nodes.push_back({NodeId, X, Y});
		auto Result = State.Commit(
			bOutput ? "Add Function Output" : "Add Function Input", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {NodeId};
		return Result;
	}

	auto FMaterialGraphDocument::RemovePort(bool bOutput, const FGuid& PortId,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Function = Cast<DMaterialFunction>(Owner.Get());
		if (!Function) return RejectCommand("Only function documents have an interface.");
		FGraphEditSession State(*Function);
		const auto Removed = std::erase_if(State.Expressions, [&](const auto& Expression) {
			if (bOutput)
			{
				const auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression.Get());
				return Terminal && Terminal->Port.Id == PortId;
			}
			const auto* Terminal = Cast<DMaterialExpressionFunctionInput>(Expression.Get());
			return Terminal && Terminal->Port.Id == PortId;
		});
		if (!Removed) return {.Status = EMaterialGraphCommandStatus::NoChange};
		return State.Commit("Remove Function Port", Transactions);
	}

	auto FMaterialGraphDocument::CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X, int32 Y,
		FMaterialProgramLink Source, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const auto* Registration = FindMaterialGraphExpressionRegistration(Entry.ExpressionClass);
		if (!Registration || Registration->CreationKind != EMaterialGraphCreationKind::Direct)
			return RejectCommand("The catalog expression class is unavailable for direct creation.");
		const auto& Description = *Registration->Description;
		const auto Shape = std::ranges::find(Description.Shapes, Entry.ResultType, &FMaterialExpressionAuthoringShape::ResultType);
		if (Shape == Description.Shapes.end() || Entry.AcceptedInputTypes != Shape->AcceptedInputTypes
			|| Description.SemanticOpcode != Entry.Opcode)
			return RejectCommand("The catalog expression shape is stale.");
		auto* Upstream = FindMaterialExpression(FMaterialExpressionEditing::GetExpressions(*Owner.Get()), Source.SourceNodeId);
		if (Source.SourceNodeId.IsValid() && !Upstream) return RejectCommand("The source expression no longer exists.");
		FGraphEditSession State(*Owner.Get());
		IncludeCallOutput(State, Upstream, Source.SourceOutputId);
		FMaterialExpressionInput FirstInput{Upstream, Source.SourceOutputIndex, Source.SourceOutputId};
		if (FirstInput.IsConnected() && Entry.AcceptedInputTypes.empty())
			return RejectCommand("This catalog expression has no input pin.");
		auto MakeUniqueStaticBoolName = [&]() {
			const std::string BaseName = "StaticBool";
			FName Candidate(BaseName);
			for (uint32 Suffix = 1; std::ranges::any_of(State.Expressions, [&](const auto& Value) {
				const auto* Declaration = Cast<DMaterialExpressionStaticBool>(Value.Get());
				return Declaration && Declaration->Name == Candidate;
			}); ++Suffix)
				Candidate = FName(std::format("{}{}", BaseName, Suffix));
			return Candidate;
		};
		FGuid GeneratedStaticBoolId;
		if (Entry.Opcode == EMaterialProgramOpcode::StaticSwitch
			&& !FirstInput.IsConnected())
		{
			TStrongObjectPtr<DMaterialExpressionStaticBool> Condition(
				NewObject<DMaterialExpressionStaticBool>(nullptr, NAME_None));
			Condition->Id = GeneratedStaticBoolId = FGuid::NewGuid();
			Condition->DeclarationId = FGuid::NewGuid();
			Condition->Name = MakeUniqueStaticBoolName();
			FirstInput = {Condition.Get()};
			State.Presentation.Nodes.push_back({Condition->Id, X - 220, Y, "StaticBool"});
			State.Expressions.emplace_back(Condition.Get());
		}
		TStrongObjectPtr<DMaterialExpression> Expression(NewRegisteredMaterialExpression(Entry.ExpressionClass));
		if (!Expression) return RejectCommand("The catalog expression class is unavailable.");
		Expression->Id = FGuid::NewGuid();
		if (auto* Declaration = Cast<DMaterialExpressionStaticBool>(Expression.Get()))
		{
			Declaration->DeclarationId = FGuid::NewGuid();
			Declaration->Name = MakeUniqueStaticBoolName();
		}
		if (auto* Property = Expression->GetClass()->FindPropertyByName("ResultType"))
			*static_cast<EMaterialProgramValueType*>(Property->GetValuePtr(Expression.Get())) = Entry.ResultType;
		if (auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression.Get()))
		{
			if (Entry.ResultType > EMaterialProgramValueType::Float4) return RejectCommand("The swizzle width is invalid.");
			Swizzle->Components.clear();
			for (uint8 Index = 0; Index <= static_cast<uint8>(Entry.ResultType); ++Index) Swizzle->Components.push_back(Index);
		}
		FGuid ParameterId;
		std::string DisplayName;
		if (auto* Texture = Cast<DMaterialExpressionTextureParameter>(Expression.Get()); Texture && Entry.ResultType == EMaterialProgramValueType::TextureCube)
			Texture->DefaultValue.Dimension = EMaterialTextureDimension::TextureCube;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
		{
			if (!Schema.CanOwnParameters()) return RejectCommand("Functions cannot own root parameters.");
			ParameterId = Parameter->Metadata.Id = FGuid::NewGuid();
			const bool bTexture = Cast<DMaterialExpressionTextureParameter>(Expression.Get()) != nullptr;
			const auto BaseName = bTexture ? std::string("TextureParameter") : std::string(GetProgramTypeName(Entry.ResultType)) + "Parameter";
			Parameter->Metadata.Name = FName(BaseName);
			const auto* Material = Cast<DMaterial>(Owner.Get());
			for (uint32 Suffix = 1; Material->FindParameterDefinition(Parameter->Metadata.Name); ++Suffix)
				Parameter->Metadata.Name = FName(std::format("{}{}", BaseName, Suffix));
			DisplayName = Parameter->Metadata.DisplayName = Parameter->Metadata.Name.ToString();
		}
		if (Entry.AcceptedInputTypes.size() != Expression->GetAuthoredInputCount())
			return RejectCommand("The catalog input shape is stale.");
		std::optional<uint32> InvalidInput;
		if (auto* Blend = Cast<DMaterialExpressionBlendMaterialAttributes>(Expression.Get())) Blend->B = FirstInput;
		VisitMaterialExpressionInputs(*Expression, [&](uint32 Index, FMaterialExpressionInput& Input) {
			if (Index == 0 && FirstInput.IsConnected())
			{
				Input = FirstInput;
				if (!IsMaterialAdaptiveNumeric(Entry.Opcode)) return;
			}
			if (const auto* Sample = Cast<DMaterialExpressionTextureSample2D>(Expression.Get()); Sample && &Input == &Sample->UV.Connection) return;
			if (const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()); Sample && &Input == &Sample->UV.Connection) return;
			if ((Entry.Opcode == EMaterialProgramOpcode::QualitySwitch
				|| Entry.Opcode == EMaterialProgramOpcode::FeatureLevelSwitch)
				&& Index > 0) return;
			if (!FindMaterialNumericInput(*Expression, Input) && !Input.IsConnected())
				if (!InvalidInput) InvalidInput = Index;
		});
		if (InvalidInput) return RejectCommand("Create this node from a compatible texture or Surface output.");
		const auto Id = Expression->Id;
		State.Presentation.Nodes.push_back({Id, X, Y, DisplayName});
		State.Expressions.emplace_back(Expression.Get());
		auto Result = State.Commit("Create Graph Expression", Transactions);
		if (Result)
		{
			Result.GeneratedNodeIds = Result.AffectedNodeIds = {Id};
			if (GeneratedStaticBoolId.IsValid())
			{
				Result.GeneratedNodeIds.push_back(GeneratedStaticBoolId);
				Result.AffectedNodeIds.push_back(GeneratedStaticBoolId);
			}
			if (ParameterId.IsValid()) Result.AffectedParameterIds = {ParameterId};
		}
		return Result;
	}

	auto FMaterialGraphDocument::CreateExpression(const DMaterialExpression& Expression, int32 X, int32 Y,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		if (!Schema.CanCreateExpression(Expression)) return RejectCommand("This expression is not supported by the graph.");
		FGraphEditSession State(*Owner.Get());
		if (std::ranges::any_of(State.Expressions, [&](const auto& Value) { return Value->Id == Expression.Id; }))
			return RejectCommand("The expression GUID already exists.");
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		auto* Copy = Duplicated.value();
		if (!Copy) return RejectCommand(ToString(Duplicated.error()));
		State.Expressions.emplace_back(Copy);
		if (!Copy->Id.IsValid()) Copy->Id = FGuid::NewGuid();
		const auto Id = Copy->Id;
		FGuid ParameterId;
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Copy))
		{
			if (const auto Error = ResolveParameterExpression(State, *Parameter); !Error) return Error;
			ParameterId = Parameter->Metadata.Id;
		}
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = State.Commit("Create Graph Expression", Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded)
		{
			Result.GeneratedNodeIds = {Id}; Result.AffectedNodeIds = {Id};
			if (ParameterId.IsValid()) Result.AffectedParameterIds.push_back(ParameterId);
		}
		return Result;
	}

	auto FMaterialGraphDocument::ReplaceExpression(const DMaterialExpression& Expression,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		const auto Duplicated = DuplicateObject(&Expression, nullptr, NAME_None);
		if (!Duplicated) return RejectCommand(ToString(Duplicated.error()));
		TStrongObjectPtr<DMaterialExpression> Replacement(Duplicated.value());
		return CommitExpressionReplacement(*Owner.Get(), *Replacement, Transactions);
	}

	auto GraphEditInternals::CommitExpressionReplacement(DObject& Owner, DMaterialExpression& Replacement,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		FGraphEditSession State(Owner);
		const auto Id = Replacement.Id;
		const auto Existing = std::ranges::find_if(State.Expressions, [&](const auto& E) { return E->Id == Id; });
		if (Existing == State.Expressions.end()) return RejectCommand("The expression no longer exists.");
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(&Replacement))
			if (const auto Error = ResolveParameterExpression(State, *Parameter,
				Cast<DMaterialExpressionParameter>(Existing->Get())); !Error) return Error;
		if ((*Existing)->GetClass() == Replacement.GetClass())
		{
			if (const auto Assigned = State.Assign(**Existing, Replacement); !Assigned) return Assigned;
		}
		else *Existing = &Replacement;
		auto Result = State.Commit("Replace Graph Expression", Transactions);
		if (Result.GetStatus() == EMaterialGraphCommandStatus::Succeeded) Result.AffectedNodeIds.push_back(Id);
		return Result;
	}

	auto FMaterialGraphDocument::SetConfiguration(const FMaterialConfiguration& Configuration, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return RejectCommand("Only materials have a domain and shading configuration.");
		if (Transactions && !Transactions->CanTransact()) return RejectCommand("The editor transactor is busy.");
		if (const auto Valid = ValidateMaterialConfiguration(Configuration.Domain, Configuration.Properties); !Valid)
			return RejectCommand(Valid.error());
		const FMaterialConfiguration Before{Material->GetDomain(), Material->GetStaticProperties()};
		if (Before.Domain == Configuration.Domain && Before.Properties == Configuration.Properties)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		FScopedTransaction Recording(Transactions, {"MaterialGraph", "Change Material Configuration"});
		if (Transactions)
		{
			const auto Recorded = Transactions->StoreUndo(Material, std::make_unique<FMaterialConfigurationChange>(*Material, Before, Configuration));
			if (!Recorded) return RejectCommand("Unable to record the configuration edit. " + FormatTransactorResult(Recorded));
		}
		const auto Result = Material->SetMaterialConfiguration(Configuration);
		if (!Result) { Recording.Cancel(); return RejectCommand("The material configuration was rejected.", Result.Diagnostics); }
		(void)Recording.End();
		if (Transactions && Transactions->GetLastResult().Code == ETransactorResultCode::Failed)
			return RejectCommand(FormatTransactorResult(Transactions->GetLastResult()));
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphDocument::SetUseMaterialAttributes(bool bEnabled, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return RejectCommand("Only materials have a material output mode.");
		if (Material->GetExpressionOutputs().bUseMaterialAttributes == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FGraphEditSession State(*Material);
		State.GetOutputs().bUseMaterialAttributes = bEnabled;
		return State.Commit("Change Material Output Mode", Transactions);
	}

	auto FMaterialGraphDocument::InsertFunctionCall(DMaterialFunctionInterface& Function,
		int32 X, int32 Y, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		return InsertFunctionCall(Function, X, Y, {}, Mode);
	}

	auto FMaterialGraphDocument::InsertFunctionCall(DMaterialFunctionInterface& Function,
		int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
		ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		FGraphEditSession State(*Owner.Get());
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const std::array<DMaterialFunctionInterface*, 1> Roots{&Function};
		auto Validation = ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing);
		if (!Validation) return RejectCommand("The selected function dependency is invalid.", std::move(Validation.Diagnostics));
		if (std::ranges::any_of(Closure, [&](const auto& Dependency) { return Dependency.AssetPath == Owner.Get()->GetObjectPath(); }))
			return RejectCommand("This call would introduce recursive function dependencies.");
		const FGuid Id = FGuid::NewGuid();
		TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
		Call->Id = Id; Call->Function = &Function;
		for (const auto& Output : Function.GetFunctionSignature().Outputs) Call->Outputs.push_back({Output.Id, Output.Type});
		for (const auto& Input : Inputs)
		{
			auto* Source = FindMaterialExpression(FMaterialExpressionEditing::GetExpressions(*Owner.Get()), Input.Source.SourceNodeId);
			if (Input.Source.SourceNodeId.IsValid() && !Source) return RejectCommand("The source expression no longer exists.");
			IncludeCallOutput(State, Source, Input.Source.SourceOutputId);
			FMaterialExpressionFunctionInputBinding Binding{Input.InputId, Input.ExpectedType,
				{Source, Input.Source.SourceOutputIndex, Input.Source.SourceOutputId}};
			if (Input.Default.Kind != EMaterialInputDefaultKind::None)
			{
				const auto Count = static_cast<size_t>(Input.Default.Type) + 1;
				const std::array Values{Input.Default.Literal.X, Input.Default.Literal.Y, Input.Default.Literal.Z, Input.Default.Literal.W};
				if (Count > Values.size()) return RejectCommand("The function input default has an invalid numeric width.");
				Binding.InputDefault.assign(Values.begin(), Values.begin() + Count);
			}
			Call->Inputs.push_back(std::move(Binding));
		}
		Validation = ValidateMaterialFunctionCallSignature(*Call.Get(), Function.GetFunctionSignature(), EMaterialFunctionValidationMode::Editing);
		if (!Validation) return RejectCommand("The function call bindings are invalid.", std::move(Validation.Diagnostics));
		State.Expressions.emplace_back(Call.Get());
		State.Presentation.Nodes.push_back({Id, X, Y});
		auto Result = State.Commit("Insert Function Call", Transactions);
		if (Result) Result.AffectedNodeIds = Result.GeneratedNodeIds = {Id};
		return Result;
	}

}
