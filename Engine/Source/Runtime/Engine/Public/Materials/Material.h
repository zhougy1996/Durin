#pragma once

#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramTypes.h"
#include "Texture/Texture2D.h"

#include "Material.gen.h"

namespace Durin
{
	// Owns canonical base-material definitions and their editable default values.
	DCLASS()
	class DMaterial : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterial(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		auto GetMaterialProgram() const -> const FMaterialProgram* override
		{
			return &Program;
		}
		auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult> override
		{
			return AcceptedCompiledProgram;
		}
		auto GetMaterialCompileDiagnostics() const
			-> std::span<const FMaterialCompileDiagnostic>
		{
			return MaterialCompileDiagnostics;
		}
		auto GetMaterialCompileStatus() const -> const FMaterialCompileStatus&
		{
			return MaterialCompileStatus;
		}
		auto GetMaterialCookDiagnostic() const -> std::string_view
		{
			return MaterialCookDiagnostic;
		}
		ENGINE_API auto SetMaterialProgram(
			FMaterialProgram InProgram,
			FMaterialProgramValidationResult& OutValidation) -> bool;
		ENGINE_API auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		auto GetStaticProperties() const -> const FMaterialStaticProperties& override { return StaticProperties; }
		ENGINE_API auto GetRenderableStaticProperties() const
			-> FMaterialStaticProperties override;
		ENGINE_API auto SetStaticProperties(const FMaterialStaticProperties& InProperties) -> bool;

		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> bool;
		ENGINE_API auto SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> bool;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool;
		ENGINE_API auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		ENGINE_API auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool override;
		ENGINE_API auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		ENGINE_API auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
		ENGINE_API auto PostEditChangeProperty(
			const FPropertyChangedEvent& Event) -> void override;
		ENGINE_API auto BeginDestroy() -> void override;

	protected:
		ENGINE_API auto BuildMaterialLocalRenderLayer() const
			-> FMaterialLocalRenderLayer override;

	private:
		ENGINE_API auto RequestProgramCompile(
			const FMaterialProgram& CandidateProgram,
			const FMaterialStaticProperties& CandidateProperties,
			bool bForceRecompile = false) -> bool;
		auto AdvanceAuthoredRevision() -> void;
		// These values are inherited by instances and will form shader and pipeline keys.
		DPROPERTY(Edit)
		FMaterialStaticProperties StaticProperties;

		// Definition identity and metadata are canonical; only the nested Value fields are editable.
		DPROPERTY()
		std::vector<FMaterialParameterDefinition> ParameterDefinitions;

		// Missing legacy fields retain the canonical constructor value; malformed
		// present program data is rejected by PostLoad.
		DPROPERTY(EditorOnly)
		FMaterialProgram Program;

		// Runtime-only descriptor replaced transactionally in cooked package bytes.
		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedProgramPayload;

		std::shared_ptr<const FMaterialCompilerResult> AcceptedCompiledProgram;
		FMaterialStaticProperties AcceptedCompiledStaticProperties;
		FMaterialCompileStatus MaterialCompileStatus;
		std::vector<FMaterialCompileDiagnostic> MaterialCompileDiagnostics;
		std::string MaterialCookDiagnostic;

		auto LoadCookedProgram(std::string& OutError) -> bool;

		friend struct Private::FMaterialCompileServiceAccess;
	};
}
