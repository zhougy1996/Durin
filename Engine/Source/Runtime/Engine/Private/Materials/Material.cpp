#include "MaterialParameterMutation.h"
#include "MaterialExpressionOwnership.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/ObjectCacheContext.h"
#include "Materials/Material.h"
#include "Materials/MaterialCustomVersion.h"
#include "Logging/LogMacros.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialRenderTypes.h"
#include "Asset/Asset.h"
#include "DObject/Property.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	namespace
	{
		auto AdvanceRevision(uint64& Revision) -> void
		{
			Revision = Revision == std::numeric_limits<uint64>::max()
				? 1 : Revision + 1;
		}

	}

	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		if (!IsTemplateConstructionPurpose(ObjectInitializer.Purpose))
		{
#if DURIN_WITH_EDITORONLY_DATA
			ValidateExpressionGraph(&ObservedExpressionCode);
#endif
			if (!IsMaterialCompilationAcceptingRequests())
				RequestProgramCompile();
			PublishMaterialRenderProxyState();
		}
	}

	auto DMaterial::AdvanceAuthoredRevision(FObjectCacheContext* Context) -> void
	{
		AdvanceRevision(CompilationState.AuthoredRevision);
		InvalidateMaterialCompilation(false, false, Context);
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterial::SetEditCompileMode(EMaterialEditCompileMode Mode) -> void
	{
		if (EditCompileMode == Mode) return;
		EditCompileMode = Mode;
		FObjectCacheContext Context;
		for (auto* Owner : Context.GetMaterialsAffectedByMaterial(this))
			if (IsValid(Owner))
			{
				if (Owner->GetMaterialCompileStatus().HasUnsubmittedEdits())
					Private::FMaterialCompilationLifecycle::ScheduleEdit(*Owner, &Context);
			}
	}

	auto DMaterial::CompileEdits() -> bool
	{
		FObjectCacheContext Context;
		bool bAccepted = Private::FMaterialCompilationLifecycle::RequestCurrent(*this, false, &Context);
		for (auto* Owner : Context.GetMaterialsAffectedByMaterial(this))
			if (IsValid(Owner) && Owner != this && !Owner->IsDynamicInstance())
				bAccepted = Private::FMaterialCompilationLifecycle::RequestCurrent(*Owner, false, &Context) && bAccepted;
		return bAccepted;
	}



	auto DMaterial::GetMaterialGraphPresentation() const -> FMaterialGraphPresentation
	{
		return Private::CollectExpressionLayout(ExpressionCollection);
	}

	auto DMaterial::SetMaterialGraphPresentation(
		FMaterialGraphPresentation InPresentation) -> EMaterialGraphPresentationResult
	{
		if (!Private::ApplyExpressionLayout(ExpressionCollection, InPresentation))
			return EMaterialGraphPresentationResult::NoChange;
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
		return EMaterialGraphPresentationResult::Changed;
	}

	auto DMaterial::ApplyMaterialGraphNodePositions(
		std::span<const FMaterialGraphNodePresentation> Positions,
		uint64 ExpectedAuthoredRevision) -> EMaterialGraphPresentationResult
	{
		if (CompilationState.AuthoredRevision != ExpectedAuthoredRevision
			|| Positions.size() > MaterialProgramMaxNodeCount)
			return EMaterialGraphPresentationResult::Rejected;
		std::unordered_set<FGuid> RequestedNodes;
		std::vector<DMaterialExpression*> Targets;
		Targets.reserve(Positions.size());
		for (const auto& Position : Positions)
		{
			auto* Expression = FindMaterialExpression(ExpressionCollection.Expressions, Position.NodeId);
			if (!Position.NodeId.IsValid() || !Expression || !RequestedNodes.insert(Position.NodeId).second
				|| Position.X < -MaterialGraphPresentationCoordinateLimit || Position.X > MaterialGraphPresentationCoordinateLimit
				|| Position.Y < -MaterialGraphPresentationCoordinateLimit || Position.Y > MaterialGraphPresentationCoordinateLimit)
				return EMaterialGraphPresentationResult::Rejected;
			Targets.push_back(Expression);
		}

		bool bChanged = false;
		for (size_t Index = 0; Index < Positions.size(); ++Index)
		{
			const auto& Position = Positions[Index];
			auto& Layout = Targets[Index]->EditorLayout;
			if (Layout.bAuthored && Layout.X == Position.X && Layout.Y == Position.Y) continue;
			if (!Layout.bAuthored) Layout.DisplayName = Position.DisplayName;
			Layout.bAuthored = true;
			Layout.X = Position.X;
			Layout.Y = Position.Y;
			bChanged = true;
		}
		if (!bChanged) return EMaterialGraphPresentationResult::NoChange;
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
		return EMaterialGraphPresentationResult::Changed;
	}
#endif

	auto DMaterial::GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition>
	{
		return ParameterSchema;
	}

	auto DMaterial::ResolveParameterValueByInfo(const FMaterialParameterInfo& Info, FResolvedMaterialParameter& OutParameter) const -> bool
	{
		const FMaterialParameterDefinition* Definition = FindParameterDefinitionByInfo(Info);
		if (!Definition) return false;
		OutParameter.Definition = Definition;
		OutParameter.Value = Definition->Value;
		OutParameter.Source = const_cast<DMaterial*>(this);
		OutParameter.bHasLocalOverride = false;
		return true;
	}

	auto DMaterial::SetMaterialConfiguration(const FMaterialConfiguration& Configuration) -> FMaterialProgramValidationResult
	{
		auto Properties = Configuration.Properties;
		if (const auto Valid = ValidateMaterialConfiguration(Configuration.Domain, Properties); !Valid)
			return {.Diagnostics = {{.Message = Valid.error()}}};
		Properties.GeometryUsage &= uint8(~4u);
		const FMaterialConfiguration Previous{Domain, StaticProperties};
		if (Domain == Configuration.Domain && StaticProperties == Properties) return {.bSucceeded = true};
		Domain = Configuration.Domain;
		StaticProperties = Properties;
		PublishMaterialEdit(Previous);
		return {.bSucceeded = true};
	}

	auto DMaterial::PublishMaterialEdit(const FMaterialConfiguration& Previous, bool bGraphChanged, bool bGraphCodeChanged) -> void
	{
		const bool bDomainChanged = Previous.Domain != Domain;
		const bool bConfigurationChanged = bDomainChanged || Previous.Properties != StaticProperties;
		const bool bShaderChanged = bGraphCodeChanged || bDomainChanged
			|| CanonicalizeMaterialShaderProperties(Previous.Properties) != CanonicalizeMaterialShaderProperties(StaticProperties);
#if DURIN_WITH_EDITORONLY_DATA
		PublishedConfiguration = {Domain, StaticProperties};
#endif
		FObjectCacheContext Context;
		if (bShaderChanged)
		{
			AdvanceRevision(CompilationState.AuthoredRevision);
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this, &Context);
		}
		if (bConfigurationChanged || bGraphCodeChanged)
			InvalidateMaterialCompilation(false, !bDomainChanged && !bGraphCodeChanged, &Context);
		auto Dirty = EMaterialRenderDirtyFlags::None;
		if (bConfigurationChanged)
		{
			MarkPackageDirty();
			Dirty |= EMaterialRenderDirtyFlags::ShaderMap | EMaterialRenderDirtyFlags::PipelineState;
		}
		if (bGraphChanged)
			Dirty |= bGraphCodeChanged ? EMaterialRenderDirtyFlags::ShaderMap : EMaterialRenderDirtyFlags::DynamicParameters;
		if (Dirty != EMaterialRenderDirtyFlags::None)
			MarkRenderDataDirty(Dirty, bDomainChanged || bGraphChanged, &Context);
		Context.EndDiscovery();
#if DURIN_WITH_EDITORONLY_DATA
		GraphChanges.Publish(*this);
#endif
	}

	auto DMaterial::SetStaticProperties(const FMaterialStaticProperties& Properties) -> std::expected<void, std::string>
	{
		const auto Result = SetMaterialConfiguration({Domain, Properties});
		if (!Result) return std::unexpected(Result.Diagnostics.front().Message);
		return {};
	}

	auto DMaterial::SetScalarParameterValue(FName Name, float Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Scalar);
		if (!Validated) return Validated;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), FMaterialParameterValue::MakeScalar(Value));
	}

	auto DMaterial::SetVector2ParameterValue(FName Name, const FVector2& Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Vector4);
		if (!Validated) return Validated;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), FMaterialParameterValue::MakeVector4(FVector4(Value, 0, 0)));
	}

	auto DMaterial::SetVectorParameterValue(FName Name, const FVector3& Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Vector4);
		if (!Validated) return Validated;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), FMaterialParameterValue::MakeVector4(FVector4(Value, 0)));
	}

	auto DMaterial::SetTextureParameterValue(FName Name, DTexture* Value) -> std::expected<void, std::string>
	{
		const auto* Definition = FindParameterDefinition(Name);
		const auto Validated = ValidateNamedMaterialParameter(Definition, EMaterialParameterType::Texture);
		if (!Validated) return Validated;
		auto Candidate = Definition->Value;
		Candidate.GetTexture().Texture = Value;
		return SetParameterValueByInfo(Definition->GetParameterInfo(), Candidate);
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterial::SetParameterValueEditorOnly(const FName& ParameterName, const FMaterialParameterMetadata& Meta) -> bool
	{
		const auto* Definition = FindParameterDefinition(ParameterName);
		if (!Definition || !Definition->Value.HasSameType(Meta.Value)) return false;
		auto Candidate = *Definition;
		Candidate.Value = Meta.Value;
		if (!ValidateMaterialParameterDefinitions(std::span(&Candidate, 1))) return false;
		const auto Declarations = FMaterialExpressionEditing::GetParameterDeclarations(*this, *Definition);
		if (!Declarations) return false;
		if (Definition->Value == Meta.Value) return true;

		// Capture all participants before the first expression can send a change event.
		if (GUndo)
		{
			for (auto* Declaration : *Declarations)
			{
				Declaration->SetFlags(EObjectFlags::Transactional);
				if (!Declaration->Modify(false)) return false;
			}
		}
		FScopedMaterialGraphChange Batch(*this);
		for (auto* Declaration : *Declarations)
		{
			const bool bMatched = Declaration->SetParameterValue(Candidate.Name, Meta,
				EMaterialExpressionSetParameterValueFlags::SendPostEditChangeProperty);
			require(bMatched);
		}
		MarkPackageDirty();
		return true;
	}
#endif

	auto DMaterial::SetParameterValueByInfo(const FMaterialParameterInfo& Info, const FMaterialParameterValue& Value) -> std::expected<void, std::string>
	{
		auto Entry = std::ranges::find(ParameterSchema, Info, &FMaterialParameterDefinition::GetParameterInfo);
		if (Entry == ParameterSchema.end() || !Info.IsGlobal())
			return std::unexpected("Material parameter does not exist: " + Info.Name.ToString());
		const bool bCooked = GetAssetRuntimeConfiguration().RequiresCookedPayload();
		if (!Entry->Value.HasSameType(Value)) return std::unexpected("Material parameter value has an incompatible texture dimension or type.");
		auto Definition = *Entry;
		Definition.Value = Value;
		const auto Validation = ValidateMaterialParameterDefinitions(std::span(&Definition, 1), !bCooked);
		if (!Validation) return std::unexpected(std::format("{} [parameter {}]", GetMaterialParameterErrorText(Validation.Error), Validation.ParameterId.ToString()));
		if (*Entry == Definition) return {};
		if (!bCooked)
		{
#if DURIN_WITH_EDITORONLY_DATA
			const auto Owners = FMaterialExpressionEditing::GetParameterDeclarations(*this, *Entry);
			if (!Owners) return std::unexpected(Owners.error());
			const FMaterialParameterMetadata Meta(Value);
			// Publish the shared edit once, after the schema and every declaration agree.
			for (auto* Parameter : *Owners)
			{
				const bool bMatched = Parameter->SetParameterValue(Definition.Name, Meta);
				require(bMatched);
			}
#else
			return std::unexpected(std::format("{} [parameter {}]", "The parameter has no authored expression owner.", Info.Name.ToString()));
#endif
		}

		*Entry = std::move(Definition);
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::DynamicParameters, true);
#if DURIN_WITH_EDITORONLY_DATA
		GraphChanges.Publish(*this);
#endif
		return {};
	}

	auto DMaterial::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialGraphVersion::Serialize(Ar) || !FMaterialOutputVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
		if (!Ar.IsError() && !IsTemplateObject() && Ar.IsSaving() && Ar.GetPurpose() == EArchivePurpose::AuthoredPackage)
		{
			const auto Validation = ValidateLoadedObjectGraph({});
			if (!Validation)
			{
				if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar)) ObjectArchive->FailValidation(Validation.error());
				else Ar.Fail(EArchiveFailureCode::InvalidData, ToString(Validation.error()));
			}
		}
	}

	auto DMaterial::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		for (auto& Definition : ParameterSchema) Definition.Value.AddReferencedObjects(Collector);
	}

	auto DMaterial::SerializeCooked(FArchive& Ar) -> void
	{
#if DURIN_WITH_EDITORONLY_DATA
		if (Ar.IsSaving() && !GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& !DeriveExpressionParameterSchema(ExpressionCollection, ParameterSchema))
		{
			Ar.Fail(EArchiveFailureCode::InvalidData, "Cannot Cook an invalid material parameter schema.");
			return;
		}
#else
		if (Ar.IsSaving() && !GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Cooking authored materials requires editor-only data support.");
			return;
		}
#endif
		Super::SerializeCooked(Ar);
		if (Ar.IsError()) return;
		std::vector<FMaterialParameterDefinition> Loaded;
		for (const auto Type : {EMaterialParameterType::Scalar, EMaterialParameterType::Vector2,
			EMaterialParameterType::Vector, EMaterialParameterType::Vector4, EMaterialParameterType::Texture})
		{
			std::vector<FMaterialParameterDefinition> Values;
			if (Ar.IsSaving())
				for (const auto& Definition : ParameterSchema)
					if (Definition.Type == Type) Values.push_back(Definition);
			std::ranges::sort(Values, {}, [](const auto& Definition) { return StringUtils::FoldAscii(Definition.Name.ToString()); });
			const FName RecordName(std::format("Durin::CookedMaterialParameter{}", static_cast<uint32>(Type)));
			auto Field = EnterArchiveField(Ar, {FName("Durin::DMaterial"),
				FName(std::format("TypedParameters{}", static_cast<uint32>(Type))),
				FArchiveLogicalTypeDescriptor::Array(FArchiveLogicalTypeDescriptor::Struct(RecordName))});
			uint64 Index = 0;
			SerializeBoundedSequence(Ar, Values, MaterialProgramMaxNodeCount,
				[&](FArchive& Inner, FMaterialParameterDefinition& Definition) {
					auto Element = EnterArchiveArrayElement(Inner, Index++);
					Inner.GetStructBaseline();
					auto Member = [&](const char* Name, auto& Value, FArchiveLogicalTypeDescriptor LogicalType) {
						auto Scope = EnterArchiveField(Inner, {RecordName, FName(Name), std::move(LogicalType)});
						Inner << Value;
					};
					Member("Name", Definition.Name, FArchiveLogicalTypeDescriptor::Name());
					{
						auto Scope = EnterArchiveField(Inner, {RecordName, FName("DisplayName"), FArchiveLogicalTypeDescriptor::String()});
						SerializeBoundedString(Inner, Definition.DisplayName, MaterialMaxParameterTextBytes);
					}
					Member("GroupName", Definition.GroupName, FArchiveLogicalTypeDescriptor::Name());
					Member("SortOrder", Definition.SortOrder, FArchiveLogicalTypeDescriptor::Scalar(true, 32));
					Member("Presentation", Definition.Presentation, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialParameterPresentation"), false, 8));
					Definition.Type = Type;
					if (Inner.IsLoading())
					{
						switch (Type)
						{
						case EMaterialParameterType::Scalar: Definition.Value = FMaterialParameterValue::MakeScalar(0); break;
						case EMaterialParameterType::Vector2: Definition.Value = FMaterialParameterValue::MakeVector2(FVector2(0)); break;
						case EMaterialParameterType::Vector: Definition.Value = FMaterialParameterValue::MakeVector(FVector3(0)); break;
						case EMaterialParameterType::Vector4: Definition.Value = FMaterialParameterValue::MakeVector4(FVector4(0)); break;
						case EMaterialParameterType::Texture: Definition.Value = FMaterialParameterValue::MakeTexture(nullptr); break;
						}
					}
					switch (Type)
					{
					case EMaterialParameterType::Scalar:
						Member("Value", Definition.Value.GetScalar(), FArchiveLogicalTypeDescriptor::Scalar(true, 32, true));
						Member("HasRange", Definition.bHasRange, FArchiveLogicalTypeDescriptor::Scalar(false, 8));
						Member("Minimum", Definition.MinimumValue, FArchiveLogicalTypeDescriptor::Scalar(true, 32, true));
						Member("Maximum", Definition.MaximumValue, FArchiveLogicalTypeDescriptor::Scalar(true, 32, true));
						break;
					case EMaterialParameterType::Vector2:
						Member("X", Definition.Value.GetVector2().x, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Y", Definition.Value.GetVector2().y, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						break;
					case EMaterialParameterType::Vector:
						Member("X", Definition.Value.GetVector().x, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Y", Definition.Value.GetVector().y, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Z", Definition.Value.GetVector().z, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						break;
					case EMaterialParameterType::Vector4:
						Member("X", Definition.Value.GetVector4().x, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Y", Definition.Value.GetVector4().y, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("Z", Definition.Value.GetVector4().z, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						Member("W", Definition.Value.GetVector4().w, FArchiveLogicalTypeDescriptor::Scalar(true, 64, true));
						break;
					case EMaterialParameterType::Texture:
					{
						auto& Texture = Definition.Value.GetTexture();
						{
							auto Scope = EnterArchiveField(Inner, {RecordName, FName("Texture"), FArchiveLogicalTypeDescriptor::Object(FName("Durin::DTexture"))});
							DObject* Object = Texture.Texture.Get();
							SerializeArchiveObjectReference(Inner, Object);
							if (Object && !Object->IsA(DTexture::StaticClass())) Inner.Fail(EArchiveFailureCode::InvalidData, "Cooked texture parameter has a non-texture reference.");
							Texture.Texture = Cast<DTexture>(Object);
						}
						Member("MinFilter", Texture.SamplerState.MinFilter, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerMinFilter"), false, 8));
						Member("MagFilter", Texture.SamplerState.MagFilter, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerMagFilter"), false, 8));
						Member("AddressU", Texture.SamplerState.AddressU, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerAddressMode"), false, 8));
						Member("AddressV", Texture.SamplerState.AddressV, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerAddressMode"), false, 8));
						Member("AddressW", Texture.SamplerState.AddressW, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialSamplerAddressMode"), false, 8));
						Member("Dimension", Texture.Dimension, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialTextureDimension"), false, 8));
						Member("Fallback", Texture.TextureFallback, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::EMaterialTextureFallback"), false, 8));
						Member("Usage", Definition.TextureUsage, FArchiveLogicalTypeDescriptor::Enum(FName("Durin::ETextureUsage"), false, 8));
						break;
					}
					}
				});
			if (Ar.IsError()) return;
			if (Ar.IsLoading()) Loaded.insert(Loaded.end(), Values.begin(), Values.end());
		}
		if (Ar.IsLoading()) ParameterSchema = std::move(Loaded);
		if (!Ar.IsError() && !ValidateMaterialParameterDefinitions(ParameterSchema, false))
			Ar.Fail(EArchiveFailureCode::InvalidData, "Invalid generated cooked material parameter schema.");
	}

	auto DMaterial::PostLoad() -> void
	{
		Super::PostLoad();
		StaticProperties.GeometryUsage &= uint8(~4u);
#if DURIN_WITH_EDITORONLY_DATA
		PublishedConfiguration = {Domain, StaticProperties};
#endif
		if (const auto Validation = ValidateMaterialStaticProperties(StaticProperties); !Validation)
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Validation.error());
			return;
		}
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			Private::FMaterialCompilationLifecycle::MarkCanceled(*this);
			CompilationState.SelectedResource->RequestedInput = {};
			if (CookedProgramData.GetMetadata().LogicalSize == 0)
			{
				CompilationState.SelectedResource->CookDiagnostic = "Material cooked program is unavailable.";
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), CompilationState.SelectedResource->CookDiagnostic);
				return;
			}
			CompilationState.SelectedResource->CompiledProgram.reset();
			CompilationState.SelectedResource->Diagnostics.clear();
			CompilationState.SelectedResource->CookDiagnostic = {};
			CacheResourceShadersForRendering();
			return;
		}
#if DURIN_WITH_EDITORONLY_DATA
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Call->UpdateFromFunctionResource(false);
		const auto SchemaValidation = DeriveExpressionParameterSchema(ExpressionCollection, ParameterSchema);
		if (!SchemaValidation)
		{
			DURIN_ERROR("PostLoad '{}': unsupported material graph; rebuild this material.", GetObjectPath());
			NotifyParameterChanges();
			return;
		}
		const FMaterialProgramValidationResult ProgramValidation =
			ValidateExpressionGraph(&ObservedExpressionCode);
		if (!ProgramValidation)
		{
			const auto Message = ProgramValidation.Diagnostics.empty()
				? "Material program validation failed."
				: ProgramValidation.Diagnostics.front().Message;
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Message);
			NotifyParameterChanges();
			return;
		}
		AdvanceRevision(MaterialProgramRevision);
		RequestProgramCompile();
		PublishMaterialRenderProxyState();
		NotifyParameterChanges();
		GraphChanges.Publish(*this);
#else
		DURIN_ERROR("PostLoad '{}': authored materials require editor-only data support.", GetObjectPath());
#endif
	}

	auto DMaterial::PostEditChangeProperty(
		const FPropertyChangedEvent& Event) -> void
	{
#if DURIN_WITH_EDITORONLY_DATA
		const auto Name = Event.MemberProperty ? Event.MemberProperty->NamePrivate : NAME_None;
		const bool bGraphChanged = Name == FName("ExpressionCollection");
		const bool bConfigurationChanged = Name == FName("Domain") || Name == FName("StaticProperties");
		if (!bGraphChanged && !bConfigurationChanged)
		{
			Super::PostEditChangeProperty(Event);
			if (Event.MemberProperty) GraphChanges.Publish(*this);
			return;
		}
		// Object replay restores every participant before PostTransacted publishes the edit.
		if (Event.Origin == EPropertyChangeOrigin::Undo || Event.Origin == EPropertyChangeOrigin::Redo) return;
		DObject::PostEditChangeProperty(Event);
		const auto Previous = PublishedConfiguration;
		bool bGraphCodeChanged = false;
		if (bGraphChanged)
		{
			std::vector<FMaterialParameterDefinition> Schema;
			if (DeriveExpressionParameterSchema(ExpressionCollection, Schema))
			{
				FXxHash128 Code;
				// Invalidate failed drafts too, so a previous preview cannot hide the error.
				const bool bCompilable = static_cast<bool>(ValidateExpressionGraph(&Code));
				ParameterSchema = std::move(Schema);
				bGraphCodeChanged = !bCompilable || Code != ObservedExpressionCode;
				ObservedExpressionCode = Code;
				if (bGraphCodeChanged) AdvanceRevision(MaterialProgramRevision);
			}
			else if (Previous.Domain == Domain && Previous.Properties == StaticProperties) return;
		}
		PublishMaterialEdit(Previous, bGraphChanged, bGraphCodeChanged);
#else
		Super::PostEditChangeProperty(Event);
#endif
	}

	auto DMaterial::BeginDestroy() -> void
	{
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*this);
		Super::BeginDestroy();
	}
}
