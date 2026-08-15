#pragma once

#include "AssetBuild/BuildTypes.h"

namespace Durin::Asset::Build
{
	class FBuildDefinition
	{
	public:
		auto GetFunction() const -> const FBuildFunctionIdentity& { return Function; }
		auto GetKey() const -> const FBuildKey& { return Key; }
		// The definition carries its output contract independently of the local
		// registry so it can be serialized for future remote execution.
		auto GetExpectedValueName() const -> std::string_view { return ExpectedValueName; }
		ASSETBUILDCORE_API auto GetInput(std::string_view Name) const -> const FBuildValue*;
		ASSETBUILDCORE_API auto GetTargetFact(std::string_view Name) const -> std::optional<std::string_view>;
		auto HasLocalInputs() const -> bool { return !Inputs.empty(); }
	private:
		FBuildFunctionIdentity Function;
		FBuildKey Key;
		std::string ExpectedValueName;
		std::vector<FBuildValue> Inputs;
		std::vector<std::pair<std::string, std::string>> TargetFacts;
		friend class FBuildDefinitionBuilder;
	};

	class FBuildDefinitionBuilder
	{
	public:
		ASSETBUILDCORE_API FBuildDefinitionBuilder(
			FBuildFunctionIdentity Function, std::string ExpectedValueName);
		ASSETBUILDCORE_API auto SetKey(
			FBuildKey Key, std::span<const uint8> CanonicalKeyInput = {})
			-> FBuildDefinitionBuilder&;
		ASSETBUILDCORE_API auto AddInput(FBuildValue Value) -> FBuildDefinitionBuilder&;
		ASSETBUILDCORE_API auto AddTargetFact(std::string Name, std::string Value)
			-> FBuildDefinitionBuilder&;
		ASSETBUILDCORE_API auto Build(
			FBuildDefinition& OutDefinition, std::string* OutError = nullptr) const -> bool;
	private:
		FBuildDefinition Definition;
		std::vector<uint8> KeyInput;
		std::string Error;
	};

	// Parses the exact unsigned-decimal syntax used by numeric Build target facts.
	ASSETBUILDCORE_API auto ParseBuildTargetFactUInt32(
		std::string_view Text, uint32& OutValue) -> bool;
}
