#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialInterface.h"
namespace Durin::Testing
{
	struct FMaterialTestParameter
	{
		FGuid Id;
		FMaterialParameterValue Value;
		auto GetType() const -> EMaterialParameterType { return Value.GetType(); }
		auto operator==(const FMaterialTestParameter&) const -> bool = default;
	};
	inline auto MakeMaterialTestParameters(const FMaterialInstanceParameterSet& Parameters) -> std::vector<FMaterialTestParameter>
	{
		std::vector<FMaterialTestParameter> Result;
		Parameters.VisitParameters([&](const FGuid& Id, const FMaterialParameterValue& Value) { Result.push_back({Id, Value}); });
		std::ranges::sort(Result, {}, &FMaterialTestParameter::Id);
		return Result;
	}
	inline auto MakeMaterialParameterSet(std::span<const FMaterialTestParameter> Parameters) -> FMaterialInstanceParameterSet
	{
		FMaterialInstanceParameterSet Result;
		for (const auto& Parameter : Parameters) Result.SetParameter(Parameter.Id, Parameter.Value);
		return Result;
	}
	// Test-only setup/assertion value. Runtime consumers query the resource directly.
	struct FMaterialTestState
	{
		std::vector<FMaterialTestParameter> Parameters;
		std::optional<FMaterialStaticProperties> StaticProperties;
		std::shared_ptr<const FMaterialProgram> CompiledProgram;
		bool bIsCompiling = false;
		auto HasValidGameThreadShaderMap() const -> bool { return CompiledProgram && CompiledProgram->HasShaderMap(); }
		auto GetStaticProperties() const -> FMaterialStaticProperties { return StaticProperties.value_or(FMaterialStaticProperties{}); }
		auto GetBlendMode() const -> EMaterialBlendMode { return GetStaticProperties().BlendMode; }
		auto IsTwoSided() const -> bool { return GetStaticProperties().bTwoSided; }
		auto operator==(const FMaterialTestState&) const -> bool = default;
	};
	inline auto CaptureResourceTestState(const FMaterialResource& Resource) -> FMaterialTestState
	{ return {Resource.CompiledProgram ? MakeMaterialTestParameters(Resource.CompiledProgram->DefaultParameters) : std::vector<FMaterialTestParameter>{}, Resource.AcceptedProperties, Resource.CompiledProgram, !Resource.Compilation.expired()}; }
	inline auto CaptureMaterialTestState(const DMaterialInterface& Material) -> FMaterialTestState
	{
		if (!Material.GetMaterialResource()) return {};
		return {MakeMaterialTestParameters(Material.GetMaterialRenderParameters()), Material.GetMaterialResourceProperties(), Material.GetAcceptedCompiledProgram(),
			Material.GetMaterialCompileStatus().State == EMaterialCompileState::Running};
	}
	inline auto CaptureMaterialTestState(const DMaterialInterface& Material, ERHIFeatureLevel Level, EMaterialQualityLevel Quality = GetMaterialQualityLevel()) -> FMaterialTestState
	{
		if (!Material.GetMaterialResource(Level, Quality)) return {};
		return {MakeMaterialTestParameters(Material.GetMaterialRenderParameters(Level, Quality)), Material.GetMaterialResourceProperties(Level, Quality), Material.GetAcceptedCompiledProgram(Level, Quality),
			Material.GetMaterialCompileStatus(Level, Quality).State == EMaterialCompileState::Running};
	}

	inline auto QueueMaterialTestState(FMaterialRenderProxy& Proxy, Durin::Testing::FMaterialTestState Values) -> void
	{
		Proxy.SetProgram_GameThread(std::move(Values.CompiledProgram));
		Proxy.SetProperties_GameThread(std::move(Values.StaticProperties));
		Proxy.InitParameters_GameThread(MakeMaterialParameterSet(Values.Parameters));
		Proxy.SetCompiling_GameThread(Values.bIsCompiling);
	}
	inline auto ApplyMaterialTestState(FMaterialRenderProxy& Proxy, Durin::Testing::FMaterialTestState Values) -> void
	{
		Proxy.SetProgram_RenderThread(std::move(Values.CompiledProgram));
		Proxy.SetProperties_RenderThread(std::move(Values.StaticProperties));
		Proxy.InitParameters_RenderThread(MakeMaterialParameterSet(Values.Parameters));
		Proxy.SetCompiling_RenderThread(Values.bIsCompiling);
	}
}
