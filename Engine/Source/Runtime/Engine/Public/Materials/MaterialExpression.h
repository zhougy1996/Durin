#pragma once

#include "CoreMinimal.h"

#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "EngineAPI.h"
#include "Materials/MaterialExpressionIO.h"

#include "MaterialExpression.gen.h"

namespace Durin
{
	namespace MIR
	{
		class FEmitter;
	}

	DSTRUCT()
	struct FMaterialExpressionLayout
	{
		GENERATED_BODY()

		DPROPERTY()
		bool bAuthored = false;

		DPROPERTY()
		int32 X = 0;

		DPROPERTY()
		int32 Y = 0;

		DPROPERTY()
		std::string DisplayName;

		auto operator==(const FMaterialExpressionLayout&) const -> bool = default;
	};

	struct FMaterialExpressionOutput
	{
		std::string_view Name;
		EMaterialProgramValueType Type;
	};

	// Package-internal expression identity. Family-specific state lives in derived types.
	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialExpression : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpression(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
#if DURIN_WITH_EDITORONLY_DATA
		// Authored presentation. An unset layout uses the graph's default position.
		DPROPERTY(EditorOnly)
		FMaterialExpressionLayout EditorLayout;

		// Fixed output ports of a plugin expression, in Build output index order.
		virtual auto GetOutputs() const -> std::span<const FMaterialExpressionOutput> { return {}; }
		virtual auto CanRenameNode() const -> bool { return false; }
		virtual auto GetEditableName() const -> std::string { return {}; }
		// Changes authored name only. The editor owns validation, transactions and publication.
		virtual auto SetEditableName(std::string NewName) -> void {}
		ENGINE_API auto PostTransacted(const FTransactionObjectEvent& Event) -> void override;
		auto PostEditUndo() -> void override {}
#endif
		virtual auto GetAuthoredInputCount() const -> uint32 { return 0; }

		DPROPERTY()
		FGuid Id;

		// Emits this node once per graph invocation and registers all of its outputs.
		ENGINE_API virtual auto Build(MIR::FEmitter& Emitter) const -> void = 0;

	};

	// Compare semantic fields across independently owned graphs using node identities.
	ENGINE_API auto HaveSameMaterialExpressionFields(DMaterialExpression& Left, DMaterialExpression& Right) -> bool;

	inline auto FindMaterialExpression(std::span<const TObjectPtr<DMaterialExpression>> Nodes, const FGuid& Id) -> DMaterialExpression*
	{
		const auto It = std::ranges::find_if(Nodes, [&](const auto& Node) { return Node && Node->Id == Id; });
		return It != Nodes.end() ? It->Get() : nullptr;
	}

	// The asset owns every expression referenced by this collection.
	DSTRUCT()
	struct FMaterialExpressionCollection
	{
		GENERATED_BODY()

		DPROPERTY()
		std::vector<TObjectPtr<DMaterialExpression>> Expressions;
	};
}
