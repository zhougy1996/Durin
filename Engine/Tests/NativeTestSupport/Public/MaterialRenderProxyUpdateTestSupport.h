#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialShaderMap.h"
namespace Durin::Testing
{
	struct FMaterialTestParameter
	{
		FMaterialFieldKey Id;
		FMaterialParameterValue Value;
		auto GetType() const -> EMaterialParameterType { return Value.GetType(); }
		auto operator==(const FMaterialTestParameter&) const -> bool = default;
	};
	inline auto MakeMaterialTestParameters(const FMaterialInstanceParameterSet& Parameters, const FMaterialProgram* Program) -> std::vector<FMaterialTestParameter>
	{
		std::vector<FMaterialTestParameter> Result;
		if (Program)
			Parameters.VisitParameters([&](uint32 Index, const FMaterialParameterValue& Value) {
				Result.push_back({Program->GetShaderMap().Layout.Fields.at(Index).ParameterId, Value});
			});
		std::ranges::sort(Result, {}, &FMaterialTestParameter::Id);
		return Result;
	}
	inline auto MakeMaterialParameterSet(std::span<const FMaterialTestParameter> Parameters, const FMaterialProgram* Program) -> FMaterialInstanceParameterSet
	{
		FMaterialInstanceParameterSet Result;
		if (!Program) return Result;
		const auto& Map = Program->GetShaderMap();
		for (const auto& Parameter : Parameters)
		{
			const auto Field = std::ranges::find(Map.Layout.Fields, Parameter.Id, &FMaterialRenderField::ParameterId);
			const auto Active = std::ranges::find(Map.ActiveParameters, Parameter.Id, &FMaterialCompilerParameterDeclaration::Id);
			if (Field != Map.Layout.Fields.end() && Active != Map.ActiveParameters.end() && Active->Type == Parameter.GetType())
				Result.SetParameter(static_cast<uint32>(Field - Map.Layout.Fields.begin()), Parameter.Value);
		}
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
	{ return {Resource.CompiledProgram ? MakeMaterialTestParameters(Resource.CompiledProgram->DefaultParameters, Resource.CompiledProgram.get()) : std::vector<FMaterialTestParameter>{}, Resource.AcceptedProperties, Resource.CompiledProgram, !Resource.Compilation.expired()}; }
	inline auto CaptureMaterialTestState(const DMaterialInterface& Material) -> FMaterialTestState
	{
		if (!Material.GetMaterialResource()) return {};
		return {MakeMaterialTestParameters(Material.GetMaterialRenderParameters(), Material.GetAcceptedCompiledProgram().get()), Material.GetMaterialResourceProperties(), Material.GetAcceptedCompiledProgram(),
			Material.GetMaterialCompileStatus().State == EMaterialCompileState::Running};
	}
	inline auto CaptureMaterialTestState(const DMaterialInterface& Material, ERHIFeatureLevel Level, EMaterialQualityLevel Quality = GetMaterialQualityLevel()) -> FMaterialTestState
	{
		if (!Material.GetMaterialResource(Level, Quality)) return {};
		return {MakeMaterialTestParameters(Material.GetMaterialRenderParameters(Level, Quality), Material.GetAcceptedCompiledProgram(Level, Quality).get()), Material.GetMaterialResourceProperties(Level, Quality), Material.GetAcceptedCompiledProgram(Level, Quality),
			Material.GetMaterialCompileStatus(Level, Quality).State == EMaterialCompileState::Running};
	}

	inline auto QueueMaterialTestState(FMaterialRenderProxy& Proxy, Durin::Testing::FMaterialTestState Values) -> void
	{
		Proxy.SetProgram_GameThread(Values.CompiledProgram);
		Proxy.SetProperties_GameThread(std::move(Values.StaticProperties));
		Proxy.InitParameters_GameThread(MakeMaterialParameterSet(Values.Parameters, Values.CompiledProgram.get()));
		Proxy.SetCompiling_GameThread(Values.bIsCompiling);
	}
	inline auto ApplyMaterialTestState(FMaterialRenderProxy& Proxy, Durin::Testing::FMaterialTestState Values) -> void
	{
		Proxy.SetProgram_RenderThread(Values.CompiledProgram);
		Proxy.SetProperties_RenderThread(std::move(Values.StaticProperties));
		Proxy.InitParameters_RenderThread(MakeMaterialParameterSet(Values.Parameters, Values.CompiledProgram.get()));
		Proxy.SetCompiling_RenderThread(Values.bIsCompiling);
	}
}
