#pragma once

#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialProgramCompiler.h"

#include <memory>
#include <functional>

namespace Durin
{
	namespace MIR
	{
		inline constexpr uint32 InvalidIndex = 0xffffffffu;
		struct FTextureDefault
		{
			FMaterialSamplerState Sampler;
			EMaterialTextureFallback Fallback = EMaterialTextureFallback::White;
		};

		// Function texture defaults are values, not fake resource nodes or IR indices.
		struct FValue
		{
			std::variant<uint32, FTextureDefault> Value = InvalidIndex;
			FValue() = default;
			FValue(uint32 Index) : Value(Index) {}
			FValue(FTextureDefault Texture) : Value(std::move(Texture)) {}
			auto GetIndex() const -> const uint32* { return std::get_if<uint32>(&Value); }
			auto GetTexture() const -> const FTextureDefault* { return std::get_if<FTextureDefault>(&Value); }
		};

		// Supplied by the owning function collection. The provider may not load assets.
		struct FFunctionBody
		{
			FMaterialFunctionSignature Signature;
			std::vector<DMaterialExpression*> Expressions;
			std::string AssetPath;
			uint64 Revision = 0;
		};
		struct FBuildEnvironment
		{
			std::function<std::optional<FFunctionBody>(const DMaterialFunctionInterface&)> FindFunction;
		};
		struct FFunctionDependency
		{
			std::string AssetPath;
			uint64 Revision = 0;
		};
	}

	enum class EMaterialFunctionValidationMode : uint8 { Compilation, Editing };

	// Editing permits unfinished wiring; both modes enforce dependency bounds and cycles.
	// Publishes stamps only on success.
	ENGINE_API auto ValidateMaterialFunctionDependencies(std::span<DMaterialFunctionInterface* const> Roots,
		std::vector<FMaterialFunctionOwnerStamp>& OutOwners,
		EMaterialFunctionValidationMode Mode = EMaterialFunctionValidationMode::Compilation) -> FMaterialProgramValidationResult;
	ENGINE_API auto ValidateMaterialFunctionCallSignature(const DMaterialExpressionFunctionCall& Call,
		const FMaterialFunctionSignature& Signature,
		EMaterialFunctionValidationMode Mode = EMaterialFunctionValidationMode::Compilation) -> FMaterialProgramValidationResult;

	namespace MIR
	{
		// Detached result. No expression, texture, or callee object is retained.
		struct FBuildResult
		{
			// Completed outcome; diagnostics and payload emptiness do not define success.
			bool bSucceeded = false;
			FModule IR;
			std::vector<uint32> Roots;
			std::vector<FMaterialCompilerParameterDeclaration> Parameters;
			std::vector<FSource> Sources;
			std::vector<FFunctionDependency> Dependencies;
			std::vector<FMaterialProgramDiagnostic> Diagnostics;
			explicit operator bool() const { return bSucceeded; }
		};

		class FGraphBuilderImpl;

		// A call-local emission interface. The builder owns traversal and invocation state.
		// Register every output during Build; never retain the emitter beyond that call.
		class FEmitter
		{
		public:
			FEmitter(const FEmitter&) = delete;
			auto operator=(const FEmitter&) -> FEmitter& = delete;
			ENGINE_API auto Output(uint8 Index, FValue Value) -> void;
			ENGINE_API auto Output(FGuid Id, FValue Value) -> void;
			ENGINE_API auto Resolve(const FMaterialExpressionInput& Input) -> FValue;
			ENGINE_API auto ResolveIndex(const FMaterialExpressionInput& Input) -> uint32;
			ENGINE_API auto FunctionInput(FGuid PortId) -> FValue;
			ENGINE_API auto FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source) -> FValue;
			ENGINE_API auto FunctionCall(const DMaterialExpressionFunctionCall& Call) -> void;
			ENGINE_API auto Emit(FNode Node) -> uint32;
			ENGINE_API auto Literal(std::span<const float> Components) -> uint32;
			ENGINE_API auto Parameter(FGuid Id, EMaterialParameterType Type) -> uint32;
			ENGINE_API auto Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
				std::span<const FMaterialExpressionInput* const> Inputs,
				std::span<const std::vector<float>* const> Defaults, std::span<const uint8> Swizzle = {}) -> uint32;
			ENGINE_API auto Coordinates() -> uint32;
			ENGINE_API auto IsNormalTexture(FValue Value) const -> bool;
			ENGINE_API auto Fail(FMaterialError Error, FGuid PortId = {},
				EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Graph) -> void;
			ENGINE_API auto GetNode(uint32 Index) const -> const FNode&;
		private:
			friend class FGraphBuilderImpl;
			FEmitter(FGraphBuilderImpl& InBuilder, FGuid InExpressionId)
				: Builder(InBuilder), ExpressionId(InExpressionId) {}
			auto RegisterOutput(uint8 Index, FGuid Id, FValue Value) -> void;
			FGraphBuilderImpl& Builder;
			const FGuid ExpressionId;
		};

		// Owning-thread, single-use build session. Finish publishes detached results.
		// Its lifetime must not cross a graph edit or an asynchronous dispatch.
		class FGraphBuilder
		{
		public:
			ENGINE_API explicit FGraphBuilder(std::span<DMaterialExpression* const> Expressions,
				FBuildEnvironment Environment = {});
			ENGINE_API ~FGraphBuilder();
			FGraphBuilder(const FGraphBuilder&) = delete;
			auto operator=(const FGraphBuilder&) -> FGraphBuilder& = delete;
			ENGINE_API auto Finish(std::span<const FMaterialExpressionInput> Roots) -> FBuildResult;
			ENGINE_API auto FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs) -> FBuildResult;
			// Local validation permits unavailable function bodies; opaque values never reach snapshots.
			ENGINE_API static auto ValidateSurface(std::span<DMaterialExpression* const> Expressions,
				const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint = nullptr) -> FMaterialProgramValidationResult;
			ENGINE_API static auto ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
		private:
			std::unique_ptr<FGraphBuilderImpl> Impl;
		};

		[[nodiscard]] ENGINE_API auto BuildGraph(
			std::span<DMaterialExpression* const> Expressions,
			std::span<const FMaterialExpressionInput> Roots,
			FBuildEnvironment Environment = {}) -> FBuildResult;
	}
}
