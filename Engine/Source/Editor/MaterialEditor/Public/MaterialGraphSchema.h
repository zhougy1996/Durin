#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialOutput.h"
#include "Materials/MaterialExpressionParameter.h"

#include "MaterialGraphOperations.h"

namespace Durin::MaterialEditor
{
	class DMaterialGraph;
	struct FMaterialGraphPin;
	enum class EMaterialGraphConnectionResponse : uint8 { Disallow, Make, ReplaceInput };
	struct FMaterialGraphConnectionResponse
	{
		EMaterialGraphConnectionResponse Response;
		std::string_view Reason;
		auto Allows(bool Replace) const -> bool
		{
			return Response == EMaterialGraphConnectionResponse::Make
				|| (Response == EMaterialGraphConnectionResponse::ReplaceInput && Replace);
		}
	};
	// Stateless graph policy. Asset loading, transactions and editor services belong to callers.
	class FMaterialGraphSchema
	{
	public:
		MATERIALEDITOR_API static auto ForOwner(const DObject& Owner) -> const FMaterialGraphSchema&;
		virtual ~FMaterialGraphSchema() = default;
		MATERIALEDITOR_API virtual auto CanCreateConnection(const FMaterialGraphPin* A, const FMaterialGraphPin* B) const -> FMaterialGraphConnectionResponse;
		// True when a link changed. An unchanged link leaves the optional error empty.
		MATERIALEDITOR_API virtual auto TryCreateConnection(const FMaterialGraphPin* A, const FMaterialGraphPin* B,
			bool Replace = false, ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API virtual auto BreakPinLinks(const FMaterialGraphPin* Pin,
			ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		// Resolve retained UI addresses and dynamic attribute requests at the command boundary.
		MATERIALEDITOR_API auto CanCreateConnection(const DMaterialGraph& Graph,
			const FMaterialGraphPinAddress& A, const FMaterialGraphPinAddress& B) const -> FMaterialGraphConnectionResponse;
		MATERIALEDITOR_API auto TryCreateConnection(DMaterialGraph& Graph, const FMaterialGraphPinAddress& A,
			const FMaterialGraphPinAddress& B, bool Replace = false, ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API auto BreakPinLinks(DMaterialGraph& Graph, FMaterialGraphPinAddress Pin,
			ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API virtual auto GetGraphContextActions(const DMaterialGraph& Graph,
			std::optional<FMaterialGraphPinAddress> Source = {}) const -> std::vector<FMaterialGraphCreationAction>;
		virtual auto HasFunctionPorts() const -> bool = 0;
		virtual auto CanOwnParameters() const -> bool = 0;
		virtual auto CanCreateExpression(const DMaterialExpression& Expression) const -> bool
		{
			if (Cast<DMaterialExpressionFunctionInput>(&Expression) || Cast<DMaterialExpressionFunctionOutput>(&Expression)) return HasFunctionPorts();
			if (Cast<DMaterialExpressionMaterialOutput>(&Expression) || Cast<DMaterialExpressionParameter>(&Expression)) return CanOwnParameters();
			return true;
		}
		MATERIALEDITOR_API auto CanCreate(const FMaterialGraphCreationAction& Action,
			std::optional<EMaterialProgramValueType> SourceType = {}) const -> bool;
		auto AllowsCreation(const FMaterialGraphCreationAction& Action,
			std::optional<EMaterialProgramValueType> Source = {}) const -> bool
		{
			if (!AllowsAction(Action)) return false;
			if (const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Action.Payload))
			{
				if (!Source) return true;
				if (Entry->AcceptedInputTypes.empty()) return false;
				if (Accepts(Entry->AcceptedInputTypes.front(), *Source)) return true;
				if (!IsMaterialAdaptiveNumeric(Entry->Opcode)) return false;
				const auto Inferred = GetMaterialProgramNodeSignature(Entry->Opcode, *Source);
				return Inferred && Inferred->InputCount && Accepts(Inferred->Inputs[0], *Source);
			}
			if (const auto* Port = std::get_if<FMaterialGraphPortCreation>(&Action.Payload))
				return HasFunctionPorts() && Port->Type <= EMaterialProgramValueType::MaterialAttributes
					&& (!Source || (Port->bOutput && AcceptsPort(Port->Type, *Source)));
			if (std::holds_alternative<FMaterialGraphCollectionParameterCreation>(Action.Payload)) return !Source;
			return true; // Function calls use the signature of the loaded asset.
		}
		static auto Accepts(std::span<const EMaterialProgramValueType> Types, EMaterialProgramValueType Source) -> bool
		{
			return std::ranges::contains(Types, Source);
		}
		static auto AcceptsPort(EMaterialProgramValueType Type, EMaterialProgramValueType Source) -> bool
		{
			return Type == Source || (Source == EMaterialProgramValueType::Float
				&& Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4);
		}
		static auto IsConnectionTypeCompatible(const FMaterialGraphPinView& Input, EMaterialProgramValueType Source) -> bool
		{
			return Accepts(Input.AcceptedTypes, Source);
		}
		static auto IsSourceAddressValid(const FMaterialGraphPinAddress& Source) -> bool
		{
			return (Source.Kind == EMaterialGraphPinKind::Output || Source.Kind == EMaterialGraphPinKind::FunctionOutput)
				&& Source.Index <= 255
				&& (Source.NodeId.IsValid() || (Source.Index == 0 && !Source.PortId.IsValid()));
		}
		static auto IsSourceKeyValid(const FMaterialGraphPinAddress& Source) -> bool
		{
			return (Source.Kind == EMaterialGraphPinKind::FunctionOutput) == Source.PortId.IsValid()
				&& (!Source.PortId.IsValid() || Source.Index == 0);
		}
		static auto CanReplaceConnection(const FMaterialExpressionInput& Previous,
			const FMaterialExpressionInput& Next, bool bReplace) -> bool
		{
			return !Previous.IsConnected() || Previous == Next || bReplace;
		}
		static auto CanRemove(bool bMaterialOutput) -> bool
		{
			return !bMaterialOutput;
		}
		static auto CanRemove(const DMaterialExpression& Expression) -> bool
		{
			return CanRemove(Cast<DMaterialExpressionMaterialOutput>(&Expression) != nullptr);
		}
	protected:
		virtual auto AllowsAction(const FMaterialGraphCreationAction& Action) const -> bool = 0;
	};

	class FMaterialSchema final : public FMaterialGraphSchema
	{
	public:
		auto HasFunctionPorts() const -> bool override { return false; }
		auto CanOwnParameters() const -> bool override { return true; }
	protected:
		auto AllowsAction(const FMaterialGraphCreationAction& Action) const -> bool override { return Action.bMaterial; }
	};

	class FMaterialFunctionSchema final : public FMaterialGraphSchema
	{
	public:
		auto HasFunctionPorts() const -> bool override { return true; }
		auto CanOwnParameters() const -> bool override { return false; }
	protected:
		auto AllowsAction(const FMaterialGraphCreationAction& Action) const -> bool override { return Action.bFunction; }
	};
}
