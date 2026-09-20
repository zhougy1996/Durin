#pragma once

#include "MaterialGraphOperations.h"

namespace Durin::Editor::Material
{
	enum class EMaterialGraphKind : uint8 { Material, Function, Unsupported };

	// Stateless graph policy. Asset loading, transactions and editor services belong to callers.
	class FMaterialGraphSchema
	{
	public:
		explicit FMaterialGraphSchema(EMaterialGraphKind InKind) : Kind(InKind) {}
		auto HasFunctionPorts() const -> bool { return Kind == EMaterialGraphKind::Function; }
		auto CanOwnParameters() const -> bool { return Kind == EMaterialGraphKind::Material; }
		auto CanCreateExpression(const DMaterialExpression& Expression) const -> bool
		{
			if (Kind == EMaterialGraphKind::Unsupported) return false;
			if (Cast<DMaterialExpressionFunctionInput>(&Expression) || Cast<DMaterialExpressionFunctionOutput>(&Expression)) return HasFunctionPorts();
			if (Cast<DMaterialExpressionMaterialOutput>(&Expression) || Cast<DMaterialExpressionParameter>(&Expression)) return CanOwnParameters();
			return true;
		}
		auto CanCreate(const FMaterialGraphCreationAction& Action,
			std::optional<EMaterialProgramValueType> Source = {}) const -> bool
		{
			if (Kind == EMaterialGraphKind::Unsupported || (HasFunctionPorts() ? !Action.bFunction : !Action.bMaterial)) return false;
			if (const auto* Entry = std::get_if<FMaterialGraphCatalogEntry>(&Action.Payload))
				return !Source || (!Entry->AcceptedInputTypes.empty() && Accepts(Entry->AcceptedInputTypes.front(), *Source));
			if (const auto* Port = std::get_if<FMaterialGraphPortCreation>(&Action.Payload))
				return HasFunctionPorts() && Port->Type <= EMaterialProgramValueType::Surface
					&& (!Source || (Port->bOutput && AcceptsPort(Port->Type, *Source)));
			return true; // Function signatures are resolved by the document.
		}
		static auto Accepts(std::span<const EMaterialProgramValueType> Types, EMaterialProgramValueType Source) -> bool
		{
			return std::ranges::find(Types, Source) != Types.end();
		}
		static auto AcceptsPort(EMaterialProgramValueType Type, EMaterialProgramValueType Source) -> bool
		{
			return Type == Source || (Source == EMaterialProgramValueType::Float
				&& Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4);
		}
		static auto IsConnectionTypeCompatible(const FMaterialGraphPinView& Input, EMaterialProgramValueType Source) -> bool
		{
			return !Input.bMissing && Accepts(Input.AcceptedTypes, Source);
		}
		static auto IsSourceAddressValid(const FMaterialGraphPinAddress& Source) -> bool
		{
			return (Source.Kind == EMaterialGraphPinKind::Output || Source.Kind == EMaterialGraphPinKind::FunctionOutput)
				&& Source.Index <= 255
				&& (Source.NodeId.IsValid() || (Source.Index == 0 && !Source.PortId.IsValid()));
		}
		static auto IsSourceKeyValid(const FMaterialGraphPinAddress& Source) -> bool
		{
			return (Source.Kind == EMaterialGraphPinKind::FunctionOutput) == Source.PortId.IsValid();
		}
		static auto CanReplaceConnection(const FMaterialExpressionInput& Previous,
			const FMaterialExpressionInput& Next, bool bReplace) -> bool
		{
			return !Previous.ExpressionId.IsValid() || Previous == Next || bReplace;
		}
		static auto CanRemove(bool bMaterialOutput) -> bool
		{
			return !bMaterialOutput;
		}
		static auto CanRemove(const DMaterialExpression& Expression) -> bool
		{
			return CanRemove(Cast<DMaterialExpressionMaterialOutput>(&Expression) != nullptr);
		}
	private:
		EMaterialGraphKind Kind;
	};
}
