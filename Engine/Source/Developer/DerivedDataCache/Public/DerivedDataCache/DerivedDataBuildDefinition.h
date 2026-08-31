#pragma once

#include "DerivedDataCache/DerivedDataBuildTypes.h"

namespace Durin::DerivedData
{
	class FBuildDefinition
	{
	public:
		auto GetFunction() const -> const FBuildFunctionName& { return Function; }
		auto GetKey() const -> const FBuildKey& { return Key; }
		// The definition carries its output contract independently of the local
		// registry so it can be serialized for future remote execution.
		auto GetExpectedValueName() const -> std::string_view { return ExpectedValueName; }
		DERIVEDDATACACHE_API auto GetInput(std::string_view Name) const -> const FBuildValue*;
		DERIVEDDATACACHE_API auto GetTargetFact(std::string_view Name) const -> std::optional<std::string_view>;
		auto HasLocalInputs() const -> bool { return !Inputs.empty(); }
	private:
		FBuildFunctionName Function;
		FBuildKey Key;
		std::string ExpectedValueName;
		std::vector<FBuildValue> Inputs;
		std::vector<std::pair<std::string, std::string>> TargetFacts;
		friend class FBuildDefinitionBuilder;
	};

	class FBuildDefinitionBuilder
	{
	public:
		DERIVEDDATACACHE_API FBuildDefinitionBuilder(
			FBuildFunctionName Function, std::string ExpectedValueName);
		DERIVEDDATACACHE_API auto SetKey(
			FBuildKey Key, std::span<const std::byte> CanonicalKeyInput = {})
			-> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddInput(FBuildValue Value) -> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto AddTargetFact(std::string Name, std::string Value)
			-> FBuildDefinitionBuilder&;
		DERIVEDDATACACHE_API auto Build(
			FBuildDefinition& OutDefinition, std::string* OutError = nullptr) const -> bool;
	private:
		FBuildDefinition Definition;
		FByteArray KeyInput;
		std::string Error;
	};

	// Parses the exact unsigned-decimal syntax used by numeric Build target facts.
	DERIVEDDATACACHE_API auto ParseBuildTargetFactUInt32(
		std::string_view Text, uint32& OutValue) -> bool;
}
