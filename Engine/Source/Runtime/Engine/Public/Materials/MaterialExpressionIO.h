#pragma once

#include "CoreMinimal.h"

#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "DObject/ObjectPtr.h"
#include "EngineAPI.h"
#include "Misc/Guid.h"

#include "MaterialExpressionIO.gen.h"

namespace Durin
{
	class DMaterialExpression;
	enum class EMaterialDomain : uint8;
	enum class EMaterialProgramOpcode : uint8;
	enum class EMaterialProgramValueType : uint8;
	enum class EMaterialProperty : uint8;

	// Graph connections carry identity only; numeric defaults belong to their expression.
	DSTRUCT()
	struct FMaterialExpressionInput
	{
		GENERATED_BODY()

		DPROPERTY()
		TObjectPtr<DMaterialExpression> Expression;

		DPROPERTY()
		int32 OutputIndex = 0;

		FMaterialExpressionInput() = default;
		FMaterialExpressionInput(DMaterialExpression* Upstream, int32 Index = 0)
			: Expression(Upstream), OutputIndex(Index) {}
		// Resolve an editor or recipe endpoint against the source's output table.
		ENGINE_API FMaterialExpressionInput(DMaterialExpression* Upstream, int32 Index, FGuid NamedOutput);
		auto IsConnected() const -> bool { return Expression.IsValid(); }
		ENGINE_API auto GetExpressionId() const -> FGuid;
		ENGINE_API auto GetOutputId() const -> FGuid;
		auto Connect(int32 Index, DMaterialExpression* Upstream) -> void
		{
			Expression = Upstream;
			OutputIndex = Index;
		}
		auto operator==(const FMaterialExpressionInput&) const -> bool = default;
	};

	// Numeric pins retain an explicit value independently of their connection.
	DSTRUCT()
	struct FMaterialNumericInput
	{
		GENERATED_BODY()
		FMaterialNumericInput() = default;
		explicit FMaterialNumericInput(uint32 Width) : Constant(Width, 0.f) {}
		DPROPERTY()
		FMaterialExpressionInput Connection;
		DPROPERTY()
		bool UseConstant = false;
		DPROPERTY()
		std::vector<float> Constant{0.f};

		auto SetConstant(std::vector<float> Value) -> void { Constant = std::move(Value); UseConstant = true; }
		auto operator=(const FMaterialExpressionInput& Value) -> FMaterialNumericInput& { Connection = Value; return *this; }
		operator FMaterialExpressionInput&() { return Connection; }
		operator const FMaterialExpressionInput&() const { return Connection; }
		auto operator==(const FMaterialNumericInput&) const -> bool = default;
	};

	ENGINE_API auto GetMaterialNumericInputFallback(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType Type, uint32 Slot) -> std::vector<float>;

	DSTRUCT()
	struct FMaterialAttributeBinding
	{
		GENERATED_BODY()
		DPROPERTY()
		FGuid AttributeID;
		DPROPERTY()
		FMaterialNumericInput Input;
		auto operator==(const FMaterialAttributeBinding&) const -> bool = default;
	};
	DSTRUCT()
	struct FMaterialAttributes
	{
		GENERATED_BODY()
		DPROPERTY()
		std::vector<FMaterialAttributeBinding> Bindings;
		auto operator==(const FMaterialAttributes&) const -> bool = default;
	};
	// Serialized bindings use stable attribute identities, not terminal pin order.
	DSTRUCT()
	struct FMaterialAttributeRoot
	{
		GENERATED_BODY()
		ENGINE_API FMaterialAttributeRoot();

		DPROPERTY()
		FMaterialExpressionInput Attributes;
		DPROPERTY()
		FMaterialAttributes Properties;

		// Both connection sets are retained; only the selected set drives the output.
		DPROPERTY()
		bool bUseMaterialAttributes = false;
		ENGINE_API auto Get(EMaterialProperty Property) -> FMaterialNumericInput&;
		ENGINE_API auto Get(EMaterialProperty Property) const -> const FMaterialNumericInput&;

		auto operator==(const FMaterialAttributeRoot&) const -> bool = default;

	};

	// Captures root values and upstream node identities independently of the owning graph.
	ENGINE_API auto CaptureMaterialOutputBindings(FMaterialAttributeRoot Root) -> std::pair<FMaterialAttributeRoot, std::vector<FGuid>>;

	// Persistent semantic identities; their values do not encode visual pin order.
	enum class EMaterialOutputPin : uint8
	{
		BaseColor = 0, Normal = 1, Metallic = 2, Roughness = 3,
		AmbientOcclusion = 4, Emissive = 5, Opacity = 6, OpacityMask = 7,
		WorldPositionOffset = 8, Specular = 9, ClearCoat = 10, ClearCoatRoughness = 11, Surface = 255,
	};
	struct FMaterialOutputPinDefinition
	{
		EMaterialOutputPin Id;
		std::string_view Name;
		EMaterialProgramValueType Type;
	};
	ENGINE_API auto GetMaterialDomainOutputPins(EMaterialDomain Domain) -> std::span<const FMaterialOutputPinDefinition>;
	ENGINE_API auto GetMaterialOutputInput(FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin) -> FMaterialExpressionInput*;

	ENGINE_API auto GetMaterialOutputNumericInput(FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin) -> FMaterialNumericInput*;
	ENGINE_API auto ReadMaterialOutputDefault(const FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin) -> std::vector<float>;
	ENGINE_API auto WriteMaterialOutputDefault(FMaterialAttributeRoot& Outputs, EMaterialOutputPin Pin, std::span<const float> Value) -> bool;
}
