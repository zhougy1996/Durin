#include "MaterialGraph.h"
#include "Graph/MaterialEditingOperations.h"
#include "MaterialExpressionParameters.h"
#include "MaterialGraphValueTypes.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/Material.h"
#include "MaterialGraphEditInternals.h"
#include "MaterialGraphEditSession.h"
#include "Materials/MaterialExpressionInputs.h"

namespace Durin::MaterialEditor
{
	using namespace GraphEditInternals;

	namespace
	{

		struct FEditableInput
		{
			FMaterialExpressionInput* Source = nullptr;
			std::vector<float>* Default = nullptr;
			std::optional<EMaterialProperty> RootProperty;
			std::string Message;
			FMaterialNumericInput* Numeric = nullptr;
			std::vector<float> Inherited;

			auto SupportsDefault() const -> bool { return Default && (!RootProperty || SupportsMaterialRootConstant(*RootProperty)); }
			auto Read() const -> std::vector<float> { return RootProperty ? ReadMaterialNumericDefault(*Numeric, *RootProperty) : Numeric ? (Numeric->UseConstant ? Numeric->Constant : Inherited) : Default ? *Default : std::vector<float>{}; }
			auto Write(const std::vector<float>& Value) const -> bool
			{
				if (RootProperty && !Value.empty() && Value.size() != Numeric->Constant.size()) return false;
				if (!Default) return false;
				if (Numeric)
				{
					if (Value.empty()) Numeric->UseConstant = false;
					else Numeric->SetConstant(Value);
					return true;
				}
				*Default = Value;
				return true;
			}
		};

		auto FindExpression(std::span<const TObjectPtr<DMaterialExpression>> Expressions, FGuid Id) -> DMaterialExpression*
		{
			const auto It = std::ranges::find(Expressions, Id, [](const auto& Expression) { return Expression->Id; });
			return It == Expressions.end() ? nullptr : It->Get();
		}

		auto FindInput(DMaterialExpression* Expression, const FMaterialGraphPinAddress& Address, FGraphEditSession* Compound = nullptr) -> FEditableInput
		{
			if (Address.IsMaterialRoot())
			{
				if (!Address.HasValidRootAddress() || Expression || !Compound || Compound->bFunction)
					return {.Message = "The material output input address is invalid."};
				if (Address.Property == EMaterialProperty::MP_MaterialAttributes)
					return {.Source = Compound->EditRootInput(Address.Property)};
				auto* Numeric = Compound->EditNumericRoot(Address.Property);
				if (!Numeric) return {.Message = "The material property is unavailable."};
				return {.Source = &Numeric->Connection, .Default = &Numeric->Constant,
					.RootProperty = Address.Property, .Numeric = Numeric};
			}
			const auto Index = Address.Index;
			const auto PortId = Address.PortId;
			if (!Expression) return {.Message = "The input owner is unavailable."};
			if (Compound) Compound->Modify(*Expression);
			if (PortId.IsValid())
			{
				auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression);
				if (!Call || !Call->Function.IsValid()) return {.Message = "The function input owner is unavailable."};
				const auto& Ports = Call->Function->GetFunctionSignature().Inputs;
				const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
				if (Port == Ports.end()) return {.Message = "The function input port is unavailable."};
				if (Port->Type > EMaterialProgramValueType::Float4)
				{
					return {.Message = "The function input port is not numeric."};
				}
				auto Input = std::ranges::find(Call->FunctionInputs, PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				if (Input == Call->FunctionInputs.end())
				{
					if (!Compound) return {.Message = "The function input has no binding."};
					Call->FunctionInputs.push_back({PortId, Port->Type});
					Input = std::prev(Call->FunctionInputs.end());
				}
				return {&Input->Input, &Input->InputDefault};
			}
			FEditableInput Result;
			VisitMaterialExpressionInputs(*Expression, [&](uint32 Pin, FMaterialExpressionInput& Input) {
				if (Pin == Index) Result.Source = &Input;
			});
			if (!Result.Source) return {.Message = "The expression input is unavailable."};
			if (Cast<DMaterialExpressionFunctionCall>(Expression)) return {.Message = "A function input requires a port identity."};
			Result.Numeric = FindMaterialNumericInput(*Expression, *Result.Source);
			Result.Default = Result.Numeric ? &Result.Numeric->Constant : nullptr;
			if (Result.Numeric)
			{
				const auto Catalog = EnumerateCatalog();
				const auto Shape = std::ranges::find(Catalog, Expression->GetClass(), &FMaterialGraphCatalogEntry::ExpressionClass);
				if (Shape != Catalog.end())
				{
					const auto* TypeProperty = Expression->GetClass()->FindPropertyByName("ResultType");
					auto Type = TypeProperty ? *static_cast<const EMaterialProgramValueType*>(TypeProperty->GetValuePtr(Expression)) : Shape->ResultType;
					if (const auto* Swizzle = Cast<DMaterialExpressionSwizzle>(Expression)) Type = static_cast<EMaterialProgramValueType>(Swizzle->Components.size() - 1);
					Result.Inherited = GetMaterialNumericInputFallback(Shape->Opcode, Type, Index);
				}
			}
			return Result;
		}

		auto MakeConstant(std::span<const float> Value, bool bTangentSpaceNormal) -> TStrongObjectPtr<DMaterialExpression>
		{
			TStrongObjectPtr<DMaterialExpression> Result;
			if (Value.size() == 1) { auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); E->Value = Value[0]; Result = E; }
			if (Value.size() == 2) { auto* E = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); E->Value = FVector2(Value[0], Value[1]); Result = E; }
			if (Value.size() == 3)
			{
				auto* E = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None);
				E->Value = FVector3(Value[0], Value[1], Value[2]);
				E->bTangentSpaceNormal = bTangentSpaceNormal;
				Result = E;
			}
			if (Value.size() == 4) { auto* E = NewObject<DMaterialExpressionVector4Constant>(nullptr, NAME_None); E->Value = FVector4(Value[0], Value[1], Value[2], Value[3]); Result = E; }
			if (Result) Result->Id = FGuid::NewGuid();
			return Result;
		}

		auto ReadConstant(const DMaterialExpression* Source) -> std::vector<float>
		{
			if (const auto* E = Cast<DMaterialExpressionScalarConstant>(Source)) return {E->Value};
			if (const auto* E = Cast<DMaterialExpressionVector2Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y)};
			if (const auto* E = Cast<DMaterialExpressionVector3Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z)};
			if (const auto* E = Cast<DMaterialExpressionVector4Constant>(Source)) return {static_cast<float>(E->Value.x), static_cast<float>(E->Value.y), static_cast<float>(E->Value.z), static_cast<float>(E->Value.w)};
			return {};
		}
	}

	auto GraphEditInternals::SetInputConstantEnabled(DObject* Owner, const FMaterialGraphPinAddress& Pin,
		bool Enabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (Pin.IsMaterialRoot() && (!Pin.HasValidRootAddress() || Pin.Property == EMaterialProperty::MP_MaterialAttributes))
			return RejectCommand("Invalid material property address.");
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Expression = FindExpression(FMaterialExpressionEditing::GetExpressions(*Owner), Pin.NodeId);
		if (!Expression && !Pin.IsMaterialRoot()) return RejectCommand("The input owner is unavailable.");
		std::optional<FGraphEditSession> RootEdit;
		if (Pin.IsMaterialRoot() && Cast<DMaterial>(Owner)) RootEdit.emplace(*Owner);
		const auto Input = FindInput(Expression, Pin, RootEdit ? &*RootEdit : nullptr);
		if (!Input.Message.empty()) return RejectCommand(Input.Message);
		if (!Input.Numeric || !Input.SupportsDefault()) return RejectCommand("This input does not support an editable constant.");
		if (Input.Numeric->UseConstant == Enabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (RootEdit)
		{
			Input.Numeric->UseConstant = Enabled;
			return RootEdit->Commit("Set Material Input Constant", Transactions);
		}
		auto Next = *Input.Numeric;
		Next.UseConstant = Enabled;
		return EditExpressionInputs(*Owner, Transactions, Enabled ? "Enable Input Constant" : "Use Definition Default", {Expression},
			[&] { *Input.Numeric = std::move(Next); }, {}, {{Input.Numeric, Next}});
	}

	auto GraphEditInternals::WriteInputDefault(DObject* Owner, const FMaterialGraphPinAddress& Pin,
		FMaterialInputDefault Value, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (Pin.IsMaterialRoot() && (!Pin.HasValidRootAddress() || Pin.Property == EMaterialProperty::MP_MaterialAttributes))
			return RejectCommand("Invalid material property address.");
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		std::optional<FGraphEditSession> BindingEdit;
		if (Pin.IsMaterialRoot() || Pin.PortId.IsValid()) BindingEdit.emplace(*Owner);
		auto* Expression = FindExpression(FMaterialExpressionEditing::GetExpressions(*Owner), Pin.NodeId);
		if (!Expression && !Pin.IsMaterialRoot()) return RejectCommand("The input owner is unavailable.");
		const auto Input = FindInput(Expression, Pin, BindingEdit ? &*BindingEdit : nullptr);
		if (!Input.Message.empty()) return RejectCommand(Input.Message);
		if (!Input.SupportsDefault()) return RejectCommand("This input does not support an inline numeric value.");
		std::vector<float> Components;
		if (Value.Kind == EMaterialInputDefaultKind::Literal && Value.Type <= EMaterialProgramValueType::Float4)
		{
			const std::array Values{Value.Literal.X, Value.Literal.Y, Value.Literal.Z, Value.Literal.W};
			Components.assign(Values.begin(), Values.begin() + static_cast<uint32>(Value.Type) + 1);
		}
		else if (Value.Kind != EMaterialInputDefaultKind::None) return RejectCommand("The input default is not numeric.");
		if (Input.Numeric && Components.empty() && !Input.Numeric->UseConstant) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (Input.Read() == Components && (!Input.Numeric || Input.Numeric->UseConstant == !Components.empty())) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (BindingEdit)
		{
			if (!Input.Write(Components)) return RejectCommand("The input default has an incompatible width.");
			return BindingEdit->Commit("Edit Input Default", Transactions);
		}
		auto Next = *Input.Numeric;
		if (Components.empty()) Next.UseConstant = false;
		else Next.SetConstant(Components);
		return EditExpressionInputs(*Owner, Transactions, "Edit Input Default", {Expression},
			[&] { *Input.Numeric = std::move(Next); }, {}, {{Input.Numeric, Next}});
	}

	auto DMaterialGraph::PromoteInputToParameter(const FMaterialGraphPinAddress& Target, ETransactionMode Mode)
		-> TMaterialGraphResult<std::vector<DMaterialGraphNode*>>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Material = Cast<DMaterial>(GetMaterialOrFunction());
		if (!Material) return std::unexpected(RejectCommand("Input parameters require a base material."));
		if (Target.IsMaterialRoot() && !MaterialEditing::CanPromoteSurfaceOutput(*Material, Target.Property))
			return std::unexpected(RejectCommand("This root input cannot be promoted to a numeric parameter."));
		const auto* Pin = FindPin(Target);
		if (!Pin || Pin->Direction != EMaterialGraphPinDirection::Input || !Pin->LinkedTo.empty())
			return std::unexpected(RejectCommand("Select an unconnected input."));
		auto* Node = Pin->GetOwningNode();
		const auto& View = Node->GetView();
		const auto& Input = View.Inputs[Pin->DisplayIndex];
		if (Input.SourceType > EMaterialProgramValueType::Float4)
			return std::unexpected(RejectCommand("Only numeric inputs can be promoted."));
		auto Literal = Input.InlineDefault.Literal;
		const auto Type = Input.InlineDefault.Kind == EMaterialInputDefaultKind::Literal ? Input.InlineDefault.Type : Input.SourceType;
		if (Input.InlineDefault.Kind == EMaterialInputDefaultKind::None && Input.PortId.IsValid() && !Input.bRequired)
		{
			if (Input.Default.Kind == EMaterialFunctionDefaultKind::Numeric) Literal = Input.Default.Numeric;
			else if (Input.Default.Kind != EMaterialFunctionDefaultKind::None)
				return std::unexpected(RejectCommand("Override the inherited function default with a constant first."));
		}
		if (View.Node.Opcode == EMaterialProgramOpcode::MakeMaterialAttributes)
			Literal = FMaterialAttributeDefinitionMap::GetDefinitions()[Input.InputIndex].DefaultValue;
		if (View.Node.Opcode == EMaterialProgramOpcode::SetMaterialAttributes && Input.InputIndex != 0)
			return std::unexpected(RejectCommand("An unconnected attribute inherits its value from the incoming surface."));
		if (Input.bSupportsConstant && !Input.bUseConstant && View.Node.IsSampleUVInput(Input.InputIndex))
			return std::unexpected(RejectCommand("Enable a constant before promoting an inherited input."));

		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = "Parameter";
		for (uint32 Suffix = 1; Material->FindParameterDefinition(Definition.Name); ++Suffix)
			Definition.Name = FName(std::format("Parameter{}", Suffix));
		Definition.DisplayName = Definition.Name.ToString();
		Definition.Value = MakeParameterValue(Type, Literal);
		Definition.Type = Definition.Value.GetType();
		auto Created = MakeParameterExpression(Definition);
		if (!Created) return std::unexpected(RejectCommand(std::move(Created.Message)));
		auto& Parameter = *Created.Expression;
		FGraphEditSession State(*Material);
		auto Destination = FindInput(Node->GetMaterialExpression(), Target, &State);
		if (!Destination.Source) return std::unexpected(RejectCommand(std::move(Destination.Message)));
		if (Destination.Source->IsConnected()) return std::unexpected(RejectCommand("The input is already connected."));
		const bool NeedsMask = Type == EMaterialProgramValueType::Float2 || Type == EMaterialProgramValueType::Float3;
		const auto X = View.Presentation.X - 320;
		const auto Y = View.Presentation.Y;
		std::vector<FGuid> Generated{Parameter.Id};
		State.Expressions.emplace_back(&Parameter);
		State.Presentation.Nodes.push_back({Parameter.Id, NeedsMask ? X - 320 : X, Y});
		*Destination.Source = {&Parameter};
		if (NeedsMask)
		{
			auto Mask = MakeParameterMask(Parameter, Type);
			*Destination.Source = {Mask.Get()};
			State.Expressions.emplace_back(Mask.Get());
			State.Presentation.Nodes.push_back({Mask->Id, X, Y});
			Generated.push_back(Mask->Id);
		}
		auto Result = State.Commit("Promote Input Parameter", *Admission);
		if (!Result) return std::unexpected(std::move(Result));
		return GetCreatedNodes(Generated);
	}

	auto DMaterialGraphNode::ExtractInputDefault(const FMaterialGraphPinAddress& Pin, ETransactionMode Mode) -> TMaterialGraphResult<DMaterialGraphNode*>
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return std::unexpected(RejectCommand(Admission.error()));
		auto* Transactions = *Admission;
		auto* Graph = ResolveInputGraph(Pin);
		auto* Expression = GetMaterialExpression();
		if (!Graph || (!Expression && !Pin.IsMaterialRoot())) return std::unexpected(RejectCommand("The input pin is unavailable."));
		FGraphEditSession State(*Graph->GetMaterialOrFunction());
		const auto Input = FindInput(Expression, Pin, &State);
		if (!Input.Message.empty()) return std::unexpected(RejectCommand(Input.Message));
		const bool bRootNormal = Input.RootProperty == EMaterialProperty::MP_Normal;
		const auto Value = Input.Read();
		if (Input.Source->IsConnected() || Value.empty())
			return std::unexpected(RejectCommand("Only an unconnected numeric literal can be extracted."));
		auto Constant = MakeConstant(Value, bRootNormal);
		if (!Constant) return std::unexpected(RejectCommand("The input default has an unsupported width."));
		*Input.Source = {Constant.Get()};
		State.Presentation.Nodes.push_back({Constant->Id, View.Presentation.X - 320, View.Presentation.Y});
		const auto Id = Constant->Id;
		State.Expressions.emplace_back(Constant.Get());
		auto Result = State.Commit("Extract Input Node", Transactions);
		if (!Result) return std::unexpected(std::move(Result));
		return Graph->GetCreatedNode(Id);
	}

	auto DMaterialGraphNode::InlineInputNode(const FMaterialGraphPinAddress& Pin, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		auto* Graph = ResolveInputGraph(Pin);
		auto* Expression = GetMaterialExpression();
		if (!Graph || (!Expression && !Pin.IsMaterialRoot())) return RejectCommand("The input pin is unavailable.");
		FGraphEditSession State(*Graph->GetMaterialOrFunction());
		const auto Input = FindInput(Expression, Pin, &State);
		if (!Input.Message.empty()) return RejectCommand(Input.Message);
		if (!Input.SupportsDefault() || Input.Source->OutputIndex != 0)
			return RejectCommand("This source cannot be represented by an inline binding.");
		const auto* Source = Input.Source->Expression.Get();
		if (!Source || !std::ranges::contains(State.Expressions, Source, [](const auto& Member) { return Member.Get(); }))
			return RejectCommand("The source expression is unavailable.");
		const auto Value = ReadConstant(Source);
		if (Value.empty()) return RejectCommand("Only numeric constants can be inlined; parameter exposure remains an explicit node.");
		if (!Input.Write(Value)) return RejectCommand("The constant has an incompatible width.");
		*Input.Source = {};
		bool RootConsumer = false;
		if (!State.bFunction)
		{
			const auto UsesSource = [&](EMaterialProperty Property) {
				return State.GetRootInput(Property)->Expression.Get() == Source;
			};
			RootConsumer = UsesSource(EMaterialProperty::MP_MaterialAttributes)
				|| std::ranges::any_of(FMaterialAttributeDefinitionMap::GetDefinitions(), [&](const auto& Field) { return UsesSource(Field.Property); });
		}
		if (!RootConsumer && !Graph->HasExpressionConsumer(*Source))
		{
			std::erase_if(State.Expressions, [&](const auto& Expression) { return Expression.Get() == Source; });
			std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Position.NodeId == Source->Id; });
		}
		return State.Commit("Inline Input Node", Transactions);
	}

}
