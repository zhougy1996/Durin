#pragma once

#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"
#include "DObject/ObjectKey.h"

#include "MaterialFunctionTypes.gen.h"

namespace Durin
{
	class DMaterialFunctionInterface;
	class DMaterialExpression;

	// Owning-thread admission metadata. Never included in a compiler snapshot.
	struct FMaterialFunctionOwnerStamp
	{
		FObjectKey Owner;
		std::string AssetPath;
		uint64 Revision = 0;
		auto operator==(const FMaterialFunctionOwnerStamp&) const -> bool = default;
	};

	inline constexpr uint32 CurrentMaterialFunctionPresentationSchemaVersion = 1;
	inline constexpr uint32 MaterialFunctionMaxInputs = 64;
	inline constexpr uint32 MaterialFunctionMaxOutputs = 16;
	inline constexpr uint32 MaterialFunctionMaxCallDepth = 16;
	inline constexpr uint32 MaterialFunctionMaxDependencies = 64;
	inline constexpr uint32 MaterialFunctionMaxExpandedNodes = 4096;
	inline constexpr uint32 MaterialFunctionMaxExpandedLinks = 16384;
	inline constexpr uint32 MaterialFunctionMaxClosureBytes = 8 * 1024 * 1024;

	DENUM()
	enum class EMaterialFunctionDefaultKind : uint8
	{
		None,
		Numeric,
		Texture,
		Surface,
		Input,
		UV0,
	};

	// Only the selected alternative participates in effective function semantics.
	DSTRUCT()
	struct FMaterialFunctionDefault
	{
		GENERATED_BODY()

		DPROPERTY()
		EMaterialFunctionDefaultKind Kind = EMaterialFunctionDefaultKind::None;

		DPROPERTY()
		FMaterialProgramLiteral Numeric;

		DPROPERTY()
		FMaterialSamplerState Sampler;

		DPROPERTY()
		EMaterialTextureFallback TextureFallback = EMaterialTextureFallback::White;

		// This value carries attributes only; links must be disconnected.

		DPROPERTY()
		FMaterialSurfaceOutputs Surface;

		DPROPERTY()
		FGuid InputId;
		auto operator==(const FMaterialFunctionDefault&) const -> bool = default;
	};

	// GUID and exact type define connectivity; names and display order are presentation.
	DSTRUCT()
	struct FMaterialFunctionPort
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid Id;

		DPROPERTY()
		EMaterialProgramValueType Type = EMaterialProgramValueType::Float;

		DPROPERTY()
		std::string Name;

		DPROPERTY()
		int32 DisplayOrder = 0;

		DPROPERTY()
		bool bAdvanced = false;

		// Required/default apply only to inputs. Outputs have neither.

		DPROPERTY()
		bool bRequired = false;

		DPROPERTY()
		FMaterialFunctionDefault Default;
		auto operator==(const FMaterialFunctionPort&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialFunctionSignature
	{
		GENERATED_BODY()

		DPROPERTY()
		std::vector<FMaterialFunctionPort> Inputs;

		DPROPERTY()
		std::vector<FMaterialFunctionPort> Outputs;
		auto operator==(const FMaterialFunctionSignature&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialFunctionInputBinding
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid InputId;

		DPROPERTY()
		EMaterialProgramValueType ExpectedType = EMaterialProgramValueType::Float;

		DPROPERTY()
		FMaterialProgramLink Source;

		DPROPERTY()
		FMaterialInputDefault Default;
		auto operator==(const FMaterialFunctionInputBinding&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialFunctionOutputBinding
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid OutputId;

		DPROPERTY()
		EMaterialProgramValueType ExpectedType = EMaterialProgramValueType::Float;
		auto operator==(const FMaterialFunctionOutputBinding&) const -> bool = default;
	};

	DSTRUCT()
	struct FMaterialFunctionPresentation
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 SchemaVersion = CurrentMaterialFunctionPresentationSchemaVersion;

		DPROPERTY()
		std::vector<FMaterialGraphNodePresentation> Nodes;
		auto operator==(const FMaterialFunctionPresentation&) const -> bool = default;
	};

	// Derived from terminal-owned port definitions, in expression collection order.
	ENGINE_API auto DeriveMaterialFunctionSignature(std::span<DMaterialExpression* const> Expressions)
		-> FMaterialFunctionSignature;

	ENGINE_API auto ValidateMaterialFunctionSignature(const FMaterialFunctionSignature& Signature)
		-> FMaterialProgramValidationResult;
}
