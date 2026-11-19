#include "MaterialGraph.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
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
			FMaterialAttributeRoot* MaterialOutputs = nullptr;
			EMaterialOutputPin OutputPin = EMaterialOutputPin::Surface;
			std::string Message;
			FMaterialNumericInput* Numeric = nullptr;
			std::vector<float> Inherited;

			auto SupportsDefault() const -> bool { return Default != nullptr || (MaterialOutputs && OutputPin != EMaterialOutputPin::Surface); }
			auto Read() const -> std::vector<float> { return MaterialOutputs ? ReadMaterialOutputDefault(*MaterialOutputs, OutputPin) : Numeric ? (Numeric->UseConstant ? Numeric->Constant : Inherited) : Default ? *Default : std::vector<float>{}; }
			auto Write(const std::vector<float>& Value) const -> bool
			{
				if (MaterialOutputs) return WriteMaterialOutputDefault(*MaterialOutputs, OutputPin, Value);
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

		auto FindInput(DMaterialExpression* Expression, uint32 Index, FGuid PortId, FGraphEditSession* Compound = nullptr) -> FEditableInput
		{
			if (!Expression) return {.Message = "The input owner is unavailable."};
			if (Compound) Compound->Modify(*Expression);
			if (auto* Output = Cast<DMaterialExpressionMaterialOutput>(Expression))
			{
				if (PortId.IsValid() || (Index != static_cast<uint32>(EMaterialOutputPin::Surface)
					&& (Index >= FMaterialAttributeDefinitionMap::GetDefinitions().size()
						|| !FMaterialAttributeDefinitionMap::Find(static_cast<EMaterialProperty>(Index))))) return {.Message = "The material output input address is invalid."};
				return {.Source = GetMaterialOutputInput(Output->Outputs, static_cast<EMaterialOutputPin>(Index)),
					.MaterialOutputs = &Output->Outputs, .OutputPin = static_cast<EMaterialOutputPin>(Index),
					.Numeric = GetMaterialOutputNumericInput(Output->Outputs, static_cast<EMaterialOutputPin>(Index))};
			}
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
				auto Input = std::ranges::find(Call->Inputs, PortId, &FMaterialExpressionFunctionInputBinding::InputId);
				if (Input == Call->Inputs.end())
				{
					if (!Compound) return {.Message = "The function input has no binding."};
					Call->Inputs.push_back({PortId, Port->Type});
					Input = std::prev(Call->Inputs.end());
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

		auto MakeConstant(std::span<const float> Value) -> TStrongObjectPtr<DMaterialExpression>
		{
			TStrongObjectPtr<DMaterialExpression> Result;
			if (Value.size() == 1) { auto* E = NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None); E->Value = Value[0]; Result = E; }
			if (Value.size() == 2) { auto* E = NewObject<DMaterialExpressionVector2Constant>(nullptr, NAME_None); E->Value = FVector2(Value[0], Value[1]); Result = E; }
			if (Value.size() == 3) { auto* E = NewObject<DMaterialExpressionVector3Constant>(nullptr, NAME_None); E->Value = FVector3(Value[0], Value[1], Value[2]); Result = E; }
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

	auto GraphEditInternals::SetInputConstantEnabled(DObject* Owner, const FGuid& NodeId, uint32 InputIndex,
		bool Enabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Expression = FindExpression(FMaterialExpressionEditing::GetExpressions(*Owner), NodeId);
		const auto Input = FindInput(Expression, InputIndex, {});
		if (!Input.Message.empty()) return RejectCommand(Input.Message);
		if (!Input.Numeric) return RejectCommand("This input has no retained numeric constant.");
		if (Input.Numeric->UseConstant == Enabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		auto Next = *Input.Numeric;
		Next.UseConstant = Enabled;
		FExpressionEdit Edit;
		Edit.SetNumeric(*Expression, *Input.Numeric, std::move(Next));
		return Edit.CommitInferred(*Owner, Transactions, Enabled ? "Enable Input Constant" : "Use Definition Default");
	}

	auto GraphEditInternals::WriteInputDefault(DObject* Owner, const FGuid& NodeId, uint32 InputIndex,
		FMaterialInputDefault Value, FGuid PortId, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		std::optional<FGraphEditSession> BindingEdit;
		if (PortId.IsValid()) BindingEdit.emplace(*Owner);
		auto* Expression = FindExpression(FMaterialExpressionEditing::GetExpressions(*Owner), NodeId);
		const auto Input = FindInput(Expression, InputIndex, PortId, BindingEdit ? &*BindingEdit : nullptr);
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
		if (Input.MaterialOutputs)
		{
			auto Outputs = *Input.MaterialOutputs;
			if (!WriteMaterialOutputDefault(Outputs, Input.OutputPin, Components)) return RejectCommand("The input default has an incompatible width.");
			Next = *GetMaterialOutputNumericInput(Outputs, Input.OutputPin);
		}
		else if (Components.empty()) Next.UseConstant = false;
		else Next.SetConstant(Components);
		FExpressionEdit Edit;
		Edit.SetNumeric(*Expression, *Input.Numeric, std::move(Next));
		return Edit.CommitInferred(*Owner, Transactions, "Edit Input Default");
	}

	auto DMaterialGraphNode::ExtractInputDefault(const FMaterialGraphPinAddress& Pin, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		auto* Graph = ResolveInputGraph(Pin);
		auto* Expression = GetMaterialExpression();
		if (!Graph || !Expression) return RejectCommand("The input pin is unavailable.");
		FGraphEditSession State(*Graph->GetMaterialOrFunction());
		State.Modify(*Expression);
		const auto Input = FindInput(Expression, Pin.Index, Pin.PortId);
		if (!Input.Message.empty()) return RejectCommand(Input.Message);
		if (!Input.SupportsDefault() || Input.Source->IsConnected() || Input.Read().empty())
			return RejectCommand("Only an unconnected numeric literal can be extracted.");
		auto Constant = MakeConstant(Input.Read());
		if (!Constant) return RejectCommand("The input default has an unsupported width.");
		*Input.Source = {Constant.Get()};
		const auto Position = std::ranges::find(State.Presentation.Nodes, Pin.NodeId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == State.Presentation.Nodes.end()) return RejectCommand("The input owner has no authored position.");
		State.Presentation.Nodes.push_back({Constant->Id, Position->X - 320, Position->Y});
		const auto Id = Constant->Id;
		State.Expressions.emplace_back(Constant.Get());
		auto Result = State.Commit("Extract Input Node", Transactions);
		if (Result) Result.GeneratedNodeIds = {Id};
		return Result;
	}

	auto DMaterialGraphNode::InlineInputNode(const FMaterialGraphPinAddress& Pin, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		auto* Graph = ResolveInputGraph(Pin);
		auto* Expression = GetMaterialExpression();
		if (!Graph || !Expression) return RejectCommand("The input pin is unavailable.");
		FGraphEditSession State(*Graph->GetMaterialOrFunction());
		State.Modify(*Expression);
		const auto Input = FindInput(Expression, Pin.Index, Pin.PortId);
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
		if (!Graph->HasExpressionConsumer(*Source))
		{
			std::erase_if(State.Expressions, [&](const auto& Expression) { return Expression.Get() == Source; });
			std::erase_if(State.Presentation.Nodes, [&](const auto& Position) { return Position.NodeId == Source->Id; });
		}
		return State.Commit("Inline Input Node", Transactions);
	}

}
