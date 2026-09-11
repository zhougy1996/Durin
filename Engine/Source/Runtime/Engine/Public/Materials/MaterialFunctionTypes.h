#pragma once

#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"
#include "DObject/ObjectHandle.h"

#include "MaterialFunctionTypes.gen.h"

namespace Durin
{
	class DMaterialFunctionInterface;

	// Owning-thread admission metadata. Never included in a compiler snapshot.
	struct FMaterialFunctionOwnerStamp
	{
		FObjectHandle Owner;
		std::string AssetPath;
		uint64 Revision = 0;
		auto operator==(const FMaterialFunctionOwnerStamp&) const -> bool = default;
	};

	inline constexpr uint32 CurrentMaterialFunctionSchemaVersion = 1;
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

	// Asset references live outside common node values so detached nodes are pure values.
	DSTRUCT()
	struct FMaterialFunctionCall
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid NodeId;

		DPROPERTY()
		TObjectPtr<DMaterialFunctionInterface> Function;

		DPROPERTY()
		std::vector<FMaterialFunctionInputBinding> Inputs;

		DPROPERTY()
		std::vector<FMaterialFunctionOutputBinding> Outputs;
		auto operator==(const FMaterialFunctionCall&) const -> bool = default;
	};

	// Authored document with function terminals rather than a material Surface root.
	DSTRUCT()
	struct FMaterialFunctionGraph
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 SchemaVersion = CurrentMaterialFunctionSchemaVersion;

		DPROPERTY()
		FMaterialFunctionSignature Signature;

		DPROPERTY()
		std::vector<FMaterialProgramNode> Nodes;

		DPROPERTY()
		std::vector<FMaterialFunctionCall> Calls;
		auto operator==(const FMaterialFunctionGraph&) const -> bool = default;
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

	struct FMaterialFunctionCallSnapshot
	{
		FGuid NodeId;
		std::string FunctionPath;
		std::vector<FMaterialFunctionInputBinding> Inputs;
		std::vector<FMaterialFunctionOutputBinding> Outputs;
		auto operator==(const FMaterialFunctionCallSnapshot&) const -> bool = default;
	};

	// No reflected owner, strong/weak object pointer, load operation or presentation.
	struct FMaterialFunctionSnapshot
	{
		uint32 SchemaVersion = CurrentMaterialFunctionSchemaVersion;
		std::string AssetPath;
		uint64 Revision = 0;
		FMaterialFunctionSignature Signature;
		std::vector<FMaterialProgramNode> Nodes;
		std::vector<FMaterialFunctionCallSnapshot> Calls;
		auto operator==(const FMaterialFunctionSnapshot&) const -> bool = default;
	};

	struct FMaterialFunctionClosure
	{
		// Sorted by asset path, including unreachable authored dependencies.
		std::vector<FMaterialFunctionSnapshot> Functions;
		auto operator==(const FMaterialFunctionClosure&) const -> bool = default;
	};

	ENGINE_API auto ValidateMaterialFunctionSignature(const FMaterialFunctionSignature& Signature)
		-> FMaterialProgramValidationResult;
	ENGINE_API auto ValidateMaterialFunctionGraph(const FMaterialFunctionGraph& Graph)
		-> FMaterialProgramValidationResult;
	ENGINE_API auto ValidateMaterialFunctionCallSignature(const FMaterialFunctionCallSnapshot& Call,
		const FMaterialFunctionSignature& Signature) -> FMaterialProgramValidationResult;
	// Owning-thread capture. Failure preserves OutClosure; workers receive no asset references.
	ENGINE_API auto SnapshotMaterialFunctionClosure(
		std::span<DMaterialFunctionInterface* const> Roots, FMaterialFunctionClosure& OutClosure,
		std::span<const FGuid> RootCallIds = {}, std::vector<FMaterialFunctionOwnerStamp>* OutOwners = nullptr)
		-> FMaterialProgramValidationResult;
	ENGINE_API auto SnapshotMaterialFunctionCalls(std::span<const FMaterialFunctionCall> Calls,
		std::vector<FMaterialFunctionCallSnapshot>& OutCalls, FMaterialFunctionClosure& OutClosure,
		std::vector<FMaterialFunctionOwnerStamp>* OutOwners = nullptr)
		-> FMaterialProgramValidationResult;
	// Validates local links without requiring dependencies to be available or well formed.
	ENGINE_API auto ValidateMaterialProgramWithFunctions(const FMaterialProgram& Program,
		std::span<const FMaterialParameterDefinition> Definitions,
		std::span<const FMaterialFunctionCall> Calls) -> FMaterialProgramValidationResult;
}
