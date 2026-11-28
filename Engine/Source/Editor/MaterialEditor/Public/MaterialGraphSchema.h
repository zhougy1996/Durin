#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

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
			bool Replace = false, bool bShouldTransact = true, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API virtual auto BreakPinLinks(const FMaterialGraphPin* Pin,
			bool bShouldTransact = true, std::string* OutError = nullptr) const -> bool;
		MATERIALEDITOR_API virtual auto GetGraphContextActions(const DMaterialGraph& Graph,
			const FMaterialGraphPin* Source = nullptr) const -> std::vector<FMaterialGraphCreationAction>;
		virtual auto HasFunctionPorts() const -> bool = 0;
		virtual auto CanOwnParameters() const -> bool = 0;
		MATERIALEDITOR_API auto CanCreate(const DObject& Owner, const FMaterialGraphCreationAction& Action,
			std::optional<EMaterialProgramValueType> SourceType = {}) const -> bool;
		MATERIALEDITOR_API auto AllowsCreation(const DObject& Owner, const FMaterialGraphCreationAction& Action,
			std::optional<EMaterialProgramValueType> Source = {}) const -> bool;
		static auto Accepts(std::span<const EMaterialProgramValueType> Types, EMaterialProgramValueType Source) -> bool
		{
			return std::ranges::contains(Types, Source);
		}
		static auto AcceptsPort(EMaterialProgramValueType Type, EMaterialProgramValueType Source) -> bool
		{
			return CanValidCast(Source, Type);
		}
		static auto IsConnectionTypeCompatible(const FMaterialGraphPinView& Input, EMaterialProgramValueType Source) -> bool
		{
			return Accepts(Input.AcceptedTypes, Source);
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
	};

	class FMaterialSchema final : public FMaterialGraphSchema
	{
	public:
		auto HasFunctionPorts() const -> bool override { return false; }
		auto CanOwnParameters() const -> bool override { return true; }
	};

	class FMaterialFunctionSchema final : public FMaterialGraphSchema
	{
	public:
		auto HasFunctionPorts() const -> bool override { return true; }
		auto CanOwnParameters() const -> bool override { return false; }
	};
}
