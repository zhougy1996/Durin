#pragma once

#include "CoreMinimal.h"

#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "EngineAPI.h"
#include "Materials/MaterialExpressionIO.h"

#include "MaterialExpression.gen.h"

namespace Durin
{
	struct FMaterialParameterMetadata;
	enum class EMaterialTextureReadMode : uint8;

#if DURIN_WITH_EDITORONLY_DATA
	enum class EMaterialExpressionSetParameterValueFlags : uint32
	{
		None = 0,
		SendPostEditChangeProperty = 1 << 0,
		NoUpdateExpressionGuid = 1 << 1,
		AssignGroupAndSortPriority = 1 << 2,
	};
	ENUM_CLASS_FLAGS(EMaterialExpressionSetParameterValueFlags);
#endif

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

	struct FMaterialFunctionPort;

	struct FMaterialExpressionOutput
	{
		std::string_view Name;
		EMaterialProgramValueType Type;
		// Legacy selectors can differ from the ordinal passed to GetOutput.
		std::optional<int32> StoredIndex;
		FGuid PortId{};
		const FMaterialFunctionPort* FunctionPort = nullptr;
	};

	struct FMaterialExpressionInputInfo
	{
		std::string_view Name;
		std::optional<EMaterialProgramValueType> Type;
		bool bActive = true;
		bool bAllowImplicitCast = false;
		std::optional<int32> StoredIndex{};
		const FMaterialFunctionPort* FunctionPort = nullptr;
		std::span<const float> InlineDefault{};
		// Overrides the creation catalog label while retaining the instance name.
		std::string_view CatalogAlias{};
	};

	struct FMaterialExpressionCreationInfo
	{
		std::optional<EMaterialProgramOpcode> Opcode;
		std::string_view Name;
		std::string_view Description;
		std::string_view Category;
		bool bRequiresAsset = false;
		std::optional<EMaterialProgramValueType> PaletteDefault{};
		bool bShowInPalette = true;
		std::optional<EMaterialProgramValueType> FixedOutputType{};
		std::string_view SearchKeywords{};
		bool bRegisterForEditorCreation = true;
		bool bHasInstanceShape = false;
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
		// A creation name opts the concrete class into reflection-based discovery.
		virtual auto GetCreationInfo() const -> FMaterialExpressionCreationInfo { return {}; }
		ENGINE_API virtual auto IsAllowedIn(const DObject* MaterialOrFunction) const -> bool;
		// A borrowed field for transactional width edits. Each class keeps its serialized storage.
		virtual auto GetAuthoredResultType() const -> const EMaterialProgramValueType* { return nullptr; }
		auto GetAuthoredResultType() -> EMaterialProgramValueType*
		{
			return const_cast<EMaterialProgramValueType*>(std::as_const(*this).GetAuthoredResultType());
		}
		virtual auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo { return {}; }
		virtual auto GetInputName(int32 Index) const -> std::string_view { return GetInputInfo(Index).Name; }
#if DURIN_WITH_EDITORONLY_DATA
		// Authored presentation. An unset layout uses the graph's default position.
		DPROPERTY(EditorOnly)
		FMaterialExpressionLayout EditorLayout;

		// Query indices describe the current instance. Persisted links may use other selectors.
		ENGINE_API virtual auto GetInput(int32 Index) -> FMaterialExpressionInput*;
		auto GetInput(int32 Index) const -> const FMaterialExpressionInput*
		{
			return const_cast<DMaterialExpression*>(this)->GetInput(Index);
		}
		virtual auto GetInputValueType(int32 Index) const -> std::optional<EMaterialProgramValueType> { return GetInputInfo(Index).Type; }
		ENGINE_API virtual auto IsInputConnectionRequired(int32 Index) const -> bool;
		virtual auto GetTextureSampleReadMode() -> EMaterialTextureReadMode* { return nullptr; }
		virtual auto GetOutputs() const -> std::span<const FMaterialExpressionOutput> { return {}; }
		virtual auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput>
		{
			const auto Outputs = GetOutputs();
			if (!Outputs.empty())
			{
				if (Index < 0 || static_cast<size_t>(Index) >= Outputs.size()) return {};
				return Outputs[Index];
			}
			if (Index != 0) return {};
			if (const auto* Type = GetAuthoredResultType())
				return FMaterialExpressionOutput{.Name = {}, .Type = *Type};
			if (const auto Type = GetCreationInfo().FixedOutputType)
				return FMaterialExpressionOutput{.Name = {}, .Type = *Type};
			return {};
		}
		auto GetOutputValueType(int32 Index) const -> std::optional<EMaterialProgramValueType>
		{
			if (const auto Output = GetOutput(Index)) return Output->Type;
			return {};
		}
		virtual auto SetParameterValue(const FName& Name, const FMaterialParameterMetadata& Meta,
			EMaterialExpressionSetParameterValueFlags Flags = EMaterialExpressionSetParameterValueFlags::None) -> bool { return false; }
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
