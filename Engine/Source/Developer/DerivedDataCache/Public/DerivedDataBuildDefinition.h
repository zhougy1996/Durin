#pragma once

#include "DerivedDataCache/DerivedDataCache.h"

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

	// Validated owned values. Capture by value before submitting asynchronous work.
	class FBuildDefinition
	{
	public:
		DERIVEDDATACACHE_API static auto TryCreate(FBuildFunctionDescriptor Function,
			std::vector<FBuildConstant> Constants, std::vector<FBuildInputReference> Inputs)
			-> std::expected<FBuildDefinition, FBuildDefinitionError>;

		auto GetFunction() const -> const FBuildFunctionDescriptor& { return Function; }
		auto GetConstants() const -> std::span<const FBuildConstant> { return Constants; }
		auto GetInputs() const -> std::span<const FBuildInputReference> { return Inputs; }
		auto GetKey() const -> const FCacheKey& { return Key; }
		auto GetCanonicalBytes() const -> FByteView { return CanonicalBytes; }

		// Order-independent exact match; sorted bindings take the allocation-free linear path.
		DERIVEDDATACACHE_API auto MatchesInputs(std::span<const FBuildInputReference> Bindings) const -> bool;

	private:
		FBuildDefinition() = default;
		FBuildFunctionDescriptor Function;
		std::vector<FBuildConstant> Constants;
		std::vector<FBuildInputReference> Inputs;
		FByteBuffer CanonicalBytes;
		FCacheKey Key;
	};
}
