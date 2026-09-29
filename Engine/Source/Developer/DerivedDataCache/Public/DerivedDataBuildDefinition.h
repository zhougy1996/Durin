#pragma once

#include "DerivedDataCache/DerivedDataCacheTypes.h"

#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Durin::DerivedData
{
	struct FBuildFunctionDescriptor
	{
		std::string Name;
		uint32 Version = 0;
		uint32 ConstantsSchema = 0;
		std::string OutputType;
		uint32 OutputSchema = 0;
		FCacheBucket Bucket;

		auto operator==(const FBuildFunctionDescriptor&) const -> bool = default;
	};

	// Names and type tags are part of the canonical encoding; integers are uint64.
	using FBuildConstantValue = std::variant<bool, uint64, float, std::string, FXxHash128>;
	struct FBuildConstant
	{
		std::string Name;
		FBuildConstantValue Value;
	};

	// Semantic identity, not a physical path or compressed-storage checksum.
	struct FBuildInputReference
	{
		std::string Name;
		FXxHash128 Identity;
		std::string IdentityScheme;
		uint32 IdentityVersion = 0;
		std::string Representation;
		uint32 RepresentationVersion = 0;

		auto operator==(const FBuildInputReference&) const -> bool = default;
	};

	enum class EBuildDefinitionError : uint8
	{
		InvalidFunction, InvalidName, DuplicateName, InvalidConstant, InvalidInput, LimitExceeded
	};
	struct FBuildDefinitionError
	{
		EBuildDefinitionError Code;
		std::string Name;
	};

	// Opaque name of captured source state owned by the resolver, never a live object.
	struct FBuildSourceReference
	{
		std::string Name;
		std::string Source;
		auto operator==(const FBuildSourceReference&) const -> bool = default;
	};

	// Request description. Function versions and cache keys belong to admitted actions.
	class FBuildDefinition
	{
	public:
		auto GetFunctionName() const -> std::string_view { return FunctionName; }
		auto GetConstants() const -> std::span<const FBuildConstant> { return Constants; }
		auto GetSources() const -> std::span<const FBuildSourceReference> { return Sources; }
	private:
		friend class FBuildDefinitionBuilder;
		DERIVEDDATACACHE_API static auto TryCreate(std::string FunctionName,
			std::vector<FBuildConstant> Constants, std::vector<FBuildSourceReference> Sources)
			-> std::expected<FBuildDefinition, FBuildDefinitionError>;
		FBuildDefinition() = default;
		std::string FunctionName;
		std::vector<FBuildConstant> Constants;
		std::vector<FBuildSourceReference> Sources;
	};

	class FBuildDefinitionBuilder
	{
	public:
		explicit FBuildDefinitionBuilder(std::string FunctionName) : FunctionName(std::move(FunctionName)) {}
		FBuildDefinitionBuilder(FBuildDefinitionBuilder&&) noexcept = default;
		auto operator=(FBuildDefinitionBuilder&&) noexcept -> FBuildDefinitionBuilder& = default;
		FBuildDefinitionBuilder(const FBuildDefinitionBuilder&) = delete;
		DERIVEDDATACACHE_API auto AddConstant(std::string Name, bool Value) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddConstant(std::string Name, uint64 Value) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddConstant(std::string Name, float Value) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddConstant(std::string Name, std::string Value) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddConstant(std::string Name, FXxHash128 Value) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddInput(std::string Name, std::string Source) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto Build() && -> std::expected<FBuildDefinition, FBuildDefinitionError>;
	private:
		auto Add(std::string Name, FBuildConstantValue Value) -> FBuildDefinitionBuilder&;
		std::string FunctionName;
		std::vector<FBuildConstant> Constants;
		std::vector<FBuildSourceReference> Sources;
		std::optional<FBuildDefinitionError> Error;
	};

	// Frozen registered descriptor and resolved semantic identities, canonical schema 2.
	class FBuildAction
	{
	public:
		auto GetFunction() const -> const FBuildFunctionDescriptor& { return Function; }
		auto GetConstants() const -> std::span<const FBuildConstant> { return Constants; }
		auto GetInputs() const -> std::span<const FBuildInputReference> { return Inputs; }
		auto GetKey() const -> const FCacheKey& { return Key; }
		auto GetCanonicalBytes() const -> FByteView { return CanonicalBytes; }
	private:
		friend class FBuildActionBuilder;
		DERIVEDDATACACHE_API static auto TryCreate(const FBuildDefinition& Definition,
			FBuildFunctionDescriptor Function, std::vector<FBuildInputReference> Inputs)
			-> std::expected<FBuildAction, FBuildDefinitionError>;
		FBuildAction() = default;
		FBuildFunctionDescriptor Function;
		std::vector<FBuildConstant> Constants;
		std::vector<FBuildInputReference> Inputs;
		FByteBuffer CanonicalBytes;
		FCacheKey Key;
	};

	class FBuildActionBuilder
	{
	public:
		FBuildActionBuilder(FBuildDefinition Definition, FBuildFunctionDescriptor Function)
			: Definition(std::move(Definition)), Function(std::move(Function)) {}
		DERIVEDDATACACHE_API auto AddInput(FBuildInputReference Input) -> FBuildActionBuilder&;
		DERIVEDDATACACHE_API auto Build() && -> std::expected<FBuildAction, FBuildDefinitionError>;
	private:
		FBuildDefinition Definition;
		FBuildFunctionDescriptor Function;
		std::vector<FBuildInputReference> Inputs;
		std::optional<FBuildDefinitionError> Error;
	};
}
