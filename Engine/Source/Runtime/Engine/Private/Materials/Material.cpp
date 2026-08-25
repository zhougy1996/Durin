#include "Materials/Material.h"

#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Asset.h"
#include "DObject/Property.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, ParameterDefinitions(MakeCanonicalMaterialParameterDefinitions())
		, Program(MakeCanonicalMaterialProgram())
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose))
		{
			if (!IsMaterialCompileServiceAcceptingRequests())
				RequestProgramCompile(Program, StaticProperties);
			PublishMaterialRenderProxyState();
		}
	}

	auto DMaterial::RequestProgramCompile(
		const FMaterialProgram& CandidateProgram,
		const FMaterialStaticProperties& CandidateProperties,
		bool bForceRecompile) -> bool
	{
		FModuleManager::Get().LoadModule("RenderCore");
		FMaterialCompilerEnvironment Environment;
		std::string EnvironmentError;
		if (!BuildDefaultMaterialCompilerEnvironment(
			Environment, EnvironmentError))
		{
			MaterialCompileStatus.RequestGeneration =
				MaterialCompileStatus.RequestGeneration
					== std::numeric_limits<uint64>::max()
					? 1 : MaterialCompileStatus.RequestGeneration + 1;
			MaterialCompileStatus.State = EMaterialCompileState::Failed;
			MaterialCompileStatus.ResultCategory =
				EMaterialCompileResultCategory::Dependency;
			MaterialCompileStatus.bHasLastKnownGood =
				AcceptedCompiledProgram != nullptr;
			MaterialCompileStatus.bLastKnownGoodDisplayed =
				AcceptedCompiledProgram != nullptr;
			MaterialCompileDiagnostics = {{
				.Category = EMaterialCompileResultCategory::Dependency,
				.Source = {
					.Category = EMaterialProgramDiagnosticCategory::Dependency,
					.Message = std::move(EnvironmentError)},
				.AssetPath = GetObjectPath(),
				.Generation = MaterialCompileStatus.RequestGeneration,
				.bLastKnownGoodDisplayed = AcceptedCompiledProgram != nullptr,
			}};
			return false;
		}
		FMaterialCompilerInput Input;
		Input.Program = CandidateProgram;
		Input.StaticProperties = CandidateProperties;
		Input.Environment = std::move(Environment);
		Input.Parameters.reserve(ParameterDefinitions.size());
		for (const FMaterialParameterDefinition& Definition : ParameterDefinitions)
			Input.Parameters.push_back({Definition.Id, Definition.Type});
		std::ranges::sort(Input.Parameters, {},
			&FMaterialCompilerParameterDeclaration::Id);
		return Private::FMaterialCompileServiceAccess::Submit(
			*this, std::move(Input), bForceRecompile);
	}

	auto DMaterial::AdvanceAuthoredRevision() -> void
	{
		MaterialCompileStatus.AuthoredRevision =
			MaterialCompileStatus.AuthoredRevision
				== std::numeric_limits<uint64>::max()
				? 1 : MaterialCompileStatus.AuthoredRevision + 1;
	}

	auto DMaterial::GetRenderableStaticProperties() const
		-> FMaterialStaticProperties
	{
		FMaterialStaticProperties Result = AcceptedCompiledProgram
			? AcceptedCompiledStaticProperties : StaticProperties;
		Result.bTwoSided = StaticProperties.bTwoSided;
		Result.DepthWritePolicy = StaticProperties.DepthWritePolicy;
		return Result;
	}

	auto DMaterial::SetMaterialProgram(
		FMaterialProgram InProgram,
		FMaterialProgramValidationResult& OutValidation) -> bool
	{
		OutValidation = ValidateMaterialProgram(
			InProgram, ParameterDefinitions);
		if (!OutValidation) return false;
		if (Program == InProgram) return true;
		Program = std::move(InProgram);
		AdvanceAuthoredRevision();
		RequestProgramCompile(Program, StaticProperties);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap);
		return true;
	}

	auto DMaterial::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return ParameterDefinitions;
	}

	auto DMaterial::ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Id);
		if (!Definition) return false;
		OutParameter.Definition = Definition;
		OutParameter.Value = Definition->Value;
		OutParameter.Source = const_cast<DMaterial*>(this);
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterial::SetStaticProperties(const FMaterialStaticProperties& InProperties) -> bool
	{
		std::string Error;
		if (!ValidateMaterialStaticProperties(InProperties, Error)) return false;
		if (StaticProperties == InProperties) return true;
		const bool bShaderIdentityChanged =
			StaticProperties.BlendMode != InProperties.BlendMode
			|| StaticProperties.ShadingModel != InProperties.ShadingModel
			|| StaticProperties.OpacityMaskThreshold
				!= InProperties.OpacityMaskThreshold;
		StaticProperties = InProperties;
		if (bShaderIdentityChanged)
		{
			AdvanceAuthoredRevision();
			RequestProgramCompile(Program, StaticProperties);
		}
		MarkPackageDirty();
		MarkRenderDataDirty(
			EMaterialRenderDirtyFlags::ShaderMap
				| EMaterialRenderDirtyFlags::PipelineState);
		return true;
	}

	auto DMaterial::SetScalarParameterValue(FName Name, float Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.ScalarValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetVector2ParameterValue(FName Name, const FVector2& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.Vector2Value;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetVectorParameterValue(FName Name, const FVector3& Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.VectorValue;
		if (Mutable == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		auto& Mutable = ParameterDefinitions[static_cast<size_t>(Definition - ParameterDefinitions.data())].Value.TextureValue;
		if (Mutable.Get() == Value) return true;
		Mutable = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters);
		return true;
	}

	auto DMaterial::GetScalarParameterValue(FName Name, float& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Scalar) return false;
		OutValue = Definition->Value.ScalarValue;
		return true;
	}

	auto DMaterial::GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector2) return false;
		OutValue = Definition->Value.Vector2Value;
		return true;
	}

	auto DMaterial::GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Vector) return false;
		OutValue = Definition->Value.VectorValue;
		return true;
	}

	auto DMaterial::GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinition(Name);
		if (!Definition || Definition->Type != EMaterialParameterType::Texture) return false;
		OutValue = Definition->Value.TextureValue.Get();
		return true;
	}

	auto DMaterial::BuildMaterialLocalRenderLayer() const
		-> FMaterialLocalRenderLayer
	{
		FMaterialLocalRenderLayer Result;
		Result.Parameters.reserve(ParameterDefinitions.size());
		for (const FMaterialParameterDefinition& Definition
			: ParameterDefinitions)
		{
			Result.Parameters.push_back(
				BuildMaterialLocalRenderParameter(
					Definition.Id,
					Definition.Type,
					Definition.Value));
		}
		Result.StaticProperties = GetRenderableStaticProperties();
		Result.CompiledProgram = AcceptedCompiledProgram;
		return Result;
	}

	auto DMaterial::PostLoad(std::string& OutError) -> bool
	{
		if (!Super::PostLoad(OutError)) return false;
		if (!ValidateCanonicalMaterialParameterDefinitions(
				ParameterDefinitions, OutError)
			|| !ValidateMaterialStaticProperties(
				StaticProperties, OutError))
		{
			return false;
		}
		if (Asset::GetAssetRuntimeConfiguration().RequiresCookedPayload())
			return LoadCookedProgram(OutError);
		const FMaterialProgramValidationResult ProgramValidation =
			ValidateMaterialProgram(Program, ParameterDefinitions);
		if (!ProgramValidation)
		{
			OutError = ProgramValidation.Diagnostics.empty()
				? "Material program validation failed."
				: ProgramValidation.Diagnostics.front().Message;
			return false;
		}
		RequestProgramCompile(Program, StaticProperties);
		PublishMaterialRenderProxyState();
		return true;
	}

	auto DMaterial::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		const FName Name = Event.MemberProperty->NamePrivate;
		if (Name == FName("Program") || Name == FName("StaticProperties"))
		{
			AdvanceAuthoredRevision();
			RequestProgramCompile(Program, StaticProperties);
			MarkRenderDataDirty(EMaterialRenderDirtyFlags::ShaderMap);
		}
	}

	auto DMaterial::BeginDestroy() -> void
	{
		CancelMaterialCompile(*this);
		Super::BeginDestroy();
	}
}
