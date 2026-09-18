#pragma once

#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialProgramCompiler.h"

#include <memory>
#include <functional>

namespace Durin
{
	inline constexpr uint32 InvalidMaterialExpressionIndex = 0xffffffffu;
	struct FMaterialExpressionTextureDefault
	{
		FMaterialSamplerState Sampler;
		EMaterialTextureFallback Fallback = EMaterialTextureFallback::White;
	};

	// Function texture defaults are values, not fake resource nodes or IR indices.
	struct FMaterialExpressionBuildValue
	{
		std::variant<uint32, FMaterialExpressionTextureDefault> Value = InvalidMaterialExpressionIndex;
		FMaterialExpressionBuildValue() = default;
		FMaterialExpressionBuildValue(uint32 Index) : Value(Index) {}
		FMaterialExpressionBuildValue(FMaterialExpressionTextureDefault Texture) : Value(std::move(Texture)) {}
		auto GetIndex() const -> const uint32* { return std::get_if<uint32>(&Value); }
		auto GetTexture() const -> const FMaterialExpressionTextureDefault* { return std::get_if<FMaterialExpressionTextureDefault>(&Value); }
	};

	// Supplied by the owning function collection. The provider may not load assets.
	struct FMaterialExpressionFunctionBody
	{
		FMaterialFunctionSignature Signature;
		std::vector<DMaterialExpression*> Expressions;
		std::string AssetPath;
		uint64 Revision = 0;
	};
	struct FMaterialExpressionBuildEnvironment
	{
		std::function<std::optional<FMaterialExpressionFunctionBody>(const DMaterialFunctionInterface&)> FindFunction;
	};
	struct FMaterialExpressionFunctionDependency
	{
		std::string AssetPath;
		uint64 Revision = 0;
	};

	enum class EMaterialFunctionValidationMode : uint8 { Compilation, Editing };

	// Editing permits unfinished wiring; both modes enforce dependency bounds and cycles.
	// Publishes stamps only on success.
	ENGINE_API auto ValidateMaterialFunctionDependencies(std::span<DMaterialFunctionInterface* const> Roots,
		std::vector<FMaterialFunctionOwnerStamp>& OutOwners,
		EMaterialFunctionValidationMode Mode = EMaterialFunctionValidationMode::Compilation) -> FMaterialProgramValidationResult;
	ENGINE_API auto ValidateMaterialFunctionCallSignature(const DMaterialExpressionFunctionCall& Call,
		const FMaterialFunctionSignature& Signature,
		EMaterialFunctionValidationMode Mode = EMaterialFunctionValidationMode::Compilation) -> FMaterialProgramValidationResult;

	// Detached result. No expression, texture, or callee object is retained.
	struct FMaterialExpressionBuildResult
	{
		FMaterialIR IR;
		std::vector<uint32> Roots;
		std::vector<FMaterialCompilerParameterDeclaration> Parameters;
		std::vector<FMaterialExpressionSource> Sources;
		std::vector<FMaterialExpressionFunctionDependency> Dependencies;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		explicit operator bool() const { return Diagnostics.empty(); }
	};

	class FMaterialExpressionGraphBuilderImpl;

	// A call-local emission interface. The builder owns traversal and invocation state.
	// Register every output during Build; never retain the emitter beyond that call.
	class FMaterialExpressionEmitter
	{
	public:
		FMaterialExpressionEmitter(const FMaterialExpressionEmitter&) = delete;
		auto operator=(const FMaterialExpressionEmitter&) -> FMaterialExpressionEmitter& = delete;
		ENGINE_API auto Output(uint8 Index, FMaterialExpressionBuildValue Value) -> void;
		ENGINE_API auto Output(FGuid Id, FMaterialExpressionBuildValue Value) -> void;
		ENGINE_API auto Resolve(const FMaterialExpressionInput& Input) -> FMaterialExpressionBuildValue;
		ENGINE_API auto ResolveIndex(const FMaterialExpressionInput& Input) -> uint32;
		ENGINE_API auto FunctionInput(FGuid PortId) -> FMaterialExpressionBuildValue;
		ENGINE_API auto FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FMaterialExpressionBuildValue;
		ENGINE_API auto FunctionCall(const DMaterialExpressionFunctionCall& Call) -> void;
		ENGINE_API auto Emit(FMaterialIRNode Node) -> uint32;
		ENGINE_API auto Literal(std::span<const float> Components) -> uint32;
		ENGINE_API auto Parameter(FGuid Id, EMaterialParameterType Type) -> uint32;
		ENGINE_API auto Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::span<const FMaterialExpressionInput* const> Inputs,
			std::span<const std::vector<float>* const> Defaults, std::span<const uint8> Swizzle = {}) -> uint32;
		ENGINE_API auto Coordinates() -> uint32;
		ENGINE_API auto IsNormalTexture(FMaterialExpressionBuildValue Value) const -> bool;
		ENGINE_API auto Fail(FMaterialError Error, FGuid PortId = {},
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> void;
		ENGINE_API auto GetNode(uint32 Index) const -> const FMaterialIRNode&;
	private:
		friend class FMaterialExpressionGraphBuilderImpl;
		FMaterialExpressionEmitter(FMaterialExpressionGraphBuilderImpl& InBuilder, FGuid InExpressionId)
			: Builder(InBuilder), ExpressionId(InExpressionId) {}
		auto RegisterOutput(uint8 Index, FGuid Id, FMaterialExpressionBuildValue Value) -> void;
		FMaterialExpressionGraphBuilderImpl& Builder;
		const FGuid ExpressionId;
	};

	// Owning-thread, single-use build session. Finish publishes detached results.
	// Its lifetime must not cross a graph edit or an asynchronous dispatch.
	class FMaterialExpressionGraphBuilder
	{
	public:
		ENGINE_API explicit FMaterialExpressionGraphBuilder(std::span<DMaterialExpression* const> Expressions,
			FMaterialExpressionBuildEnvironment Environment = {});
		ENGINE_API ~FMaterialExpressionGraphBuilder();
		FMaterialExpressionGraphBuilder(const FMaterialExpressionGraphBuilder&) = delete;
		auto operator=(const FMaterialExpressionGraphBuilder&) -> FMaterialExpressionGraphBuilder& = delete;
		ENGINE_API auto Finish(std::span<const FMaterialExpressionInput> Roots) -> FMaterialExpressionBuildResult;
		ENGINE_API auto FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs) -> FMaterialExpressionBuildResult;
		// Local validation permits unavailable function bodies; opaque values never reach snapshots.
		ENGINE_API static auto ValidateSurface(std::span<DMaterialExpression* const> Expressions,
			const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
		ENGINE_API static auto ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
	private:
		std::unique_ptr<FMaterialExpressionGraphBuilderImpl> Impl;
	};

	[[nodiscard]] ENGINE_API auto BuildMaterialExpressionGraph(
		std::span<DMaterialExpression* const> Expressions,
		std::span<const FMaterialExpressionInput> Roots,
		FMaterialExpressionBuildEnvironment Environment = {}) -> FMaterialExpressionBuildResult;
}
