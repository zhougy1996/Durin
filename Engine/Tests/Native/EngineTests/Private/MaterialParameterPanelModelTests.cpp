#include "Materials/FunctionPortTestFixture.h"
#include "Materials/TypedMaterialGraphTestFixture.h"
#include "Materials/ExplicitMaterialProgramTestFixture.h"
#include "Materials/MaterialTestSupport.h"
#include "MaterialGraphOperations.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialParameterPanelModel.h"

#include <gtest/gtest.h>

namespace
{
	using FPropertyTransactionHarness = Durin::Tests::FTestTransactorOwner;

	auto FindEntry(
		const Durin::Editor::Material::FMaterialParameterPanelModel& Model,
		const Durin::FGuid& ParameterId
	) -> const Durin::Editor::Material::FMaterialParameterPanelEntry*
	{
		const auto Entries = Model.GetEntries();
		const auto It = std::ranges::find(Entries, ParameterId, &Durin::Editor::Material::FMaterialParameterPanelEntry::ParameterId);
		return It == Entries.end() ? nullptr : &*It;
	}

	auto MakeContext(
		FPropertyTransactionHarness& Transactions,
		std::string& Error
	) -> Durin::Editor::FPropertyViewContext
	{
		return {
			.Transactor = Transactions.Get(),
			.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
		};
	}

	auto MakeExpandedBase(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		return Material;
	}
}

TEST(FMaterialParameterPanelModelTests, BuildsControlsAndResolvedSourceFromRuntimeSchema)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelSchemaBase");
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelSchemaParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelSchemaChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));
	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.6f));

	const Durin::Editor::Material::FMaterialParameterPanelModel Model(Child);
	ASSERT_EQ(Model.GetEntries().size(), 48u);
	const auto* BaseColor = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value);
	const auto* Texture = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture);
	const auto* Opacity = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	const auto* Roughness = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Roughness).Value);
	const auto* UVChannel = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVChannel);
	const auto* UVScale = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVScale);
	ASSERT_NE(BaseColor, nullptr);
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_NE(Roughness, nullptr);
	ASSERT_NE(UVChannel, nullptr);
	ASSERT_NE(UVScale, nullptr);
	EXPECT_EQ(BaseColor->Control, Durin::Editor::Material::EMaterialParameterControlKind::Color);
	EXPECT_EQ(Texture->Control, Durin::Editor::Material::EMaterialParameterControlKind::AssetPicker);
	EXPECT_EQ(Opacity->Control, Durin::Editor::Material::EMaterialParameterControlKind::RangedScalar);
	EXPECT_EQ(Roughness->Control, Durin::Editor::Material::EMaterialParameterControlKind::RangedScalar);
	EXPECT_EQ(UVChannel->Control, Durin::Editor::Material::EMaterialParameterControlKind::IntegerScalar);
	EXPECT_EQ(UVChannel->Definition->GroupName.ToString(), "Surface/Base");
	EXPECT_EQ(UVScale->Control, Durin::Editor::Material::EMaterialParameterControlKind::Vector);
	EXPECT_EQ(UVScale->Definition->Type, Durin::EMaterialParameterType::Vector4);
	EXPECT_EQ(Opacity->Source, Parent);
	EXPECT_FLOAT_EQ(Opacity->Value.GetScalar(), 0.6f);
	EXPECT_FALSE(Opacity->bHasLocalOverride);
	EXPECT_TRUE(Opacity->bCanOverride);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, IntegerPresentationCanonicalizesSubmittedValues)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelIntegerMaterial");
	auto* Material = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelIntegerInstance");
	ASSERT_TRUE(Material->SetParent(Base));
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::FName("BaseColorUVChannel"), 0));
	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	const Durin::Editor::Material::FMaterialParameterPanelModel Model(Material);
	const auto* UVChannel = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).UVChannel);
	ASSERT_NE(UVChannel, nullptr);

	auto Value = UVChannel->Value;
	Value.GetScalar() = 2.6f;
	ASSERT_TRUE(Model.SubmitValueEdit(PropertyView, Context, *UVChannel, Value, false));
	float StoredValue = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), StoredValue));
	EXPECT_FLOAT_EQ(StoredValue, 3.0f);

	Value.GetScalar() = -10.0f;
	ASSERT_TRUE(Model.SubmitValueEdit(PropertyView, Context, *UVChannel, Value, false));
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::FName("BaseColorUVChannel"), StoredValue));
	EXPECT_FLOAT_EQ(StoredValue, 0.0f);
	EXPECT_TRUE(Error.empty());

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Material);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, EnablingOverrideCopiesTheParameterType)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelTypedOverrideBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelTypedOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));

	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	const Durin::Editor::Material::FMaterialParameterPanelModel Model(Instance);
	const auto* BaseColor = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value);
	ASSERT_NE(BaseColor, nullptr);
	ASSERT_EQ(BaseColor->Definition->Type, Durin::EMaterialParameterType::Vector4);
	ASSERT_TRUE(Model.SetOverrideEnabled(PropertyView, Context, *BaseColor, true));

	ASSERT_EQ(Instance->GetLocalParameterValueCount(), 1u);
	Durin::FMaterialParameterValue Override;
	ASSERT_TRUE(Instance->GetLocalParameterValue(Durin::MaterialParameters::GetBuiltinParameterIds(
		Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value, Override));
	EXPECT_EQ(Override.GetType(), Durin::EMaterialParameterType::Vector4);
	EXPECT_TRUE(Error.empty());

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, GuidRootEditsSurviveIndexChangesAndCoalesceOrCancel)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelEditBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelEditInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4)));

	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::Editor::Material::FMaterialParameterPanelModel InitialModel(Instance);
	const auto* InitialOpacity = FindEntry(InitialModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(InitialOpacity, nullptr);
	ASSERT_TRUE(InitialModel.SetOverrideEnabled(PropertyView, Context, *InitialOpacity, true));
	ASSERT_TRUE(Instance->HasLocalParameterValue(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value));

	Durin::Editor::Material::FMaterialParameterPanelModel EditModel(Instance);
	const auto* Opacity = FindEntry(EditModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_TRUE(Opacity->bHasLocalOverride);
	// Removing an earlier entry changes Opacity's array index. The retained panel row
	// remains valid because every scratch mutation resolves the entry by GUID.
	ASSERT_TRUE(Instance->ClearParameterValue(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));

	auto FirstValue = Opacity->Value;
	FirstValue.GetScalar() = 0.4f;
	ASSERT_TRUE(EditModel.SubmitValueEdit(PropertyView, Context, *Opacity, FirstValue, true));
	auto FinalValue = Opacity->Value;
	FinalValue.GetScalar() = 0.25f;
	ASSERT_TRUE(EditModel.SubmitValueEdit(PropertyView, Context, *Opacity, FinalValue, true));
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, false));
	float Value = 0.0f;
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);

	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 1.0f);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);

	Durin::Editor::Material::FMaterialParameterPanelModel CancelModel(Instance);
	const auto* CancelOpacity = FindEntry(CancelModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(CancelOpacity, nullptr);
	auto CancelValue = CancelOpacity->Value;
	CancelValue.GetScalar() = 0.8f;
	ASSERT_TRUE(CancelModel.SubmitValueEdit(PropertyView, Context, *CancelOpacity, CancelValue, true));
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, true));
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);
	EXPECT_TRUE(Error.empty());

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, ResetAndOrphanRemovalAreTransactional)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelOrphanBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelOrphanInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.3f));

	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::Editor::Material::FMaterialParameterPanelModel OverrideModel(Instance);
	const auto* Opacity = FindEntry(OverrideModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_TRUE(OverrideModel.SetOverrideEnabled(PropertyView, Context, *Opacity, false));
	EXPECT_FALSE(Instance->HasLocalParameterValue(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Instance->HasLocalParameterValue(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_FALSE(Instance->HasLocalParameterValue(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value));

	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.3f));
	ASSERT_TRUE(Instance->SetParent(nullptr));
	Durin::Editor::Material::FMaterialParameterPanelModel OrphanModel(Instance);
	ASSERT_EQ(OrphanModel.GetEntries().size(), 1u);
	const auto& Orphan = OrphanModel.GetEntries().front();
	EXPECT_TRUE(Orphan.bOrphan);
	EXPECT_EQ(Orphan.ParameterId, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_TRUE(OrphanModel.RemoveOrphan(PropertyView, Context, Orphan));
	EXPECT_TRUE(Instance->GetLocalParameterValueCount() == 0);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Instance->IsParameterValueOrphan(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Instance->GetLocalParameterValueCount() == 0);
	EXPECT_TRUE(Error.empty());

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, BaseAndTexturePickerValuesUseSharedUndoHistory)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelValueBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelValueInstance");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "PanelValueTexture");
	ASSERT_TRUE(Instance->SetParent(Base));

	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::Editor::Material::FMaterialParameterPanelModel BaseModel(Base);
	const auto* BaseOpacity = FindEntry(BaseModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	ASSERT_NE(BaseOpacity, nullptr);
	auto ScalarValue = BaseOpacity->Value;
	ScalarValue.GetScalar() = 0.7f;
	ASSERT_TRUE(Durin::Editor::Material::FMaterialGraphOperations::SetParameterValue(*Base, BaseOpacity->ParameterId, ScalarValue, Transactions.Get()));
	float Opacity = 0.0f;
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.7f);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.7f);

	Durin::Editor::Material::FMaterialParameterPanelModel InheritedModel(Instance);
	const auto* InheritedTexture = FindEntry(InheritedModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture);
	ASSERT_NE(InheritedTexture, nullptr);
	ASSERT_TRUE(InheritedModel.SetOverrideEnabled(PropertyView, Context, *InheritedTexture, true));
	Durin::Editor::Material::FMaterialParameterPanelModel TextureModel(Instance);
	const auto* TextureEntry = FindEntry(TextureModel, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Texture);
	ASSERT_NE(TextureEntry, nullptr);
	auto TextureValue = TextureEntry->Value;
	TextureValue.GetTexture().Texture = Texture;
	TextureValue.GetTexture().SamplerState.AddressU = Durin::EMaterialSamplerAddressMode::ClampToEdge;
	TextureValue.GetTexture().TextureFallback = Durin::EMaterialTextureFallback::Black;
	ASSERT_TRUE(TextureModel.SubmitValueEdit(PropertyView, Context, *TextureEntry, TextureValue, false));
	Durin::DTexture2D* ResolvedTexture = nullptr;
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, Texture);
	Durin::FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Instance->ResolveParameterValue(TextureEntry->ParameterId, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState, TextureValue.GetTexture().SamplerState);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::Black);
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, nullptr);
	ASSERT_TRUE(Instance->ResolveParameterValue(TextureEntry->ParameterId, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState.AddressU, Durin::EMaterialSamplerAddressMode::Repeat);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::White);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, Texture);
	ASSERT_TRUE(Instance->ResolveParameterValue(TextureEntry->ParameterId, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState, TextureValue.GetTexture().SamplerState);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::Black);
	ASSERT_TRUE(Instance->SetTextureParameterValue(Durin::MaterialParameters::BaseColorTextureName(), nullptr));
	ASSERT_TRUE(Instance->ResolveParameterValue(TextureEntry->ParameterId, Resolved));
	EXPECT_EQ(Resolved.Value.GetTexture().SamplerState, TextureValue.GetTexture().SamplerState);
	EXPECT_EQ(Resolved.Value.GetTexture().TextureFallback, Durin::EMaterialTextureFallback::Black);
	TextureValue.GetTexture().SamplerState.AddressU = static_cast<Durin::EMaterialSamplerAddressMode>(255);
	EXPECT_FALSE(Instance->SetParameterValue(TextureEntry->ParameterId, TextureValue));
	EXPECT_FALSE(TextureModel.SubmitValueEdit(PropertyView, Context, *TextureEntry, TextureValue, false));
	EXPECT_TRUE(Error.empty());

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, GraphDefaultSessionsRemainParameterScoped)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelIdentityBase");
	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::Editor::Material::FMaterialParameterPanelModel Model(Base);
	const auto* Opacity = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	const auto* BaseColor = FindEntry(Model, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_NE(BaseColor, nullptr);

	auto OpacityValue = Opacity->Value;
	OpacityValue.GetScalar() = 0.55f;
	Durin::Editor::Material::FMaterialGraphParameterEditSession OpacitySession, ColorSession;
	ASSERT_TRUE(OpacitySession.Begin(*Base, Opacity->ParameterId, Transactions.Get()));
	ASSERT_TRUE(OpacitySession.Apply(OpacityValue));
	ASSERT_TRUE(OpacitySession.Commit());
	auto ColorValue = BaseColor->Value;
	ColorValue.GetVector4() = Durin::FVector4(0.1, 0.2, 0.3, 0);
	ASSERT_TRUE(ColorSession.Begin(*Base, BaseColor->ParameterId, Transactions.Get()));
	ASSERT_TRUE(ColorSession.Apply(ColorValue));
	// Switching logical GUIDs commits the first continuous edit. Cancelling the
	// second must not restore the entire collection to the first edit's origin.
	ASSERT_TRUE(ColorSession.Cancel());

	float ResolvedOpacity = 0.0f;
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), ResolvedOpacity));
	EXPECT_FLOAT_EQ(ResolvedOpacity, 0.55f);
	Durin::FVector3 ResolvedColor;
	ASSERT_TRUE(Base->GetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), ResolvedColor));
	EXPECT_EQ(ResolvedColor, Durin::FVector3(BaseColor->Value.GetVector4()));
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), ResolvedOpacity));
	EXPECT_FLOAT_EQ(ResolvedOpacity, 1.0f);
	EXPECT_TRUE(Error.empty());

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}


TEST(FMaterialParameterPanelModelTests, RefreshReusesDependenciesAndInvalidatesForGraphAndParentChanges)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelCacheBase");
	auto* OtherBase = MakeExpandedBase("PanelCacheOtherBase");
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelCacheParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelCacheChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));
	const auto Id = Durin::MaterialParameters::GetBuiltinParameterIds(
		Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value;
	Durin::Editor::Material::FMaterialParameterPanelModel Model(Child);
	EXPECT_FALSE(Model.Refresh());
	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.4f));
	EXPECT_FALSE(Model.Refresh());
	ASSERT_NE(FindEntry(Model, Id), nullptr);
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.4f);
	EXPECT_EQ(FindEntry(Model, Id)->Source, Parent);
	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.2f));
	EXPECT_FALSE(Model.Refresh());
	EXPECT_TRUE(FindEntry(Model, Id)->bHasLocalOverride);
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.2f);

	ASSERT_TRUE(Base->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(Model.Refresh());
	ASSERT_EQ(Model.GetEntries().size(), 1u);
	EXPECT_TRUE(Model.GetEntries().front().bOrphan);
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Base));
	EXPECT_TRUE(Model.Refresh());
	EXPECT_FALSE(FindEntry(Model, Id)->bOrphan);
	ASSERT_TRUE(Parent->SetParent(OtherBase));
	EXPECT_TRUE(Model.Refresh());
	EXPECT_FALSE(Model.Refresh());
	ASSERT_TRUE(Parent->SetParent(nullptr));
	EXPECT_TRUE(Model.Refresh());
	ASSERT_EQ(Model.GetEntries().size(), 1u);
	EXPECT_TRUE(Model.GetEntries().front().bOrphan);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(OtherBase);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, ReflectedDefaultEditsRefreshValuesWithoutInvalidatingDependencies)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedBase("PanelDefaultCacheBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelDefaultCacheInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const auto Id = Durin::MaterialParameters::GetBuiltinParameterIds(
		Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value;
	Durin::Editor::Material::FMaterialParameterPanelModel BaseModel(Base);
	Durin::Editor::Material::FMaterialParameterPanelModel Model(Instance);
	FPropertyTransactionHarness Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	ASSERT_NE(FindEntry(BaseModel, Id), nullptr);
	const auto SchemaRevision = Base->GetParameterDefinitionSchemaRevision();
	const auto Target = MakeMaterialValueTarget(Base, Id, "ScalarValue");
	ASSERT_TRUE(Target.has_value());
	ASSERT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* Property, void* Container, uint32 Index) {
			*Property->ContainerPtrToValuePtr<float>(Container, Index) = 0.35f;
		}, true));
	EXPECT_GT(Base->GetParameterDefinitionSchemaRevision(), SchemaRevision);
	EXPECT_FALSE(Model.Refresh());
	EXPECT_FALSE(BaseModel.Refresh());
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 0.35f);
	EXPECT_FLOAT_EQ(FindEntry(BaseModel, Id)->Definition->Value.GetScalar(), 0.35f);
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, false));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Model.Refresh());
	EXPECT_FLOAT_EQ(FindEntry(Model, Id)->Value.GetScalar(), 1.0f);
	EXPECT_TRUE(Error.empty());
	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, RootPanelIncludesUnreachableCustomDefaults)
{
	InitializeDObjectSystem();
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "UnreachableDefaults");
	Durin::FMaterialParameterDefinition Definition;
	Definition.Id = Durin::FGuid::NewGuid();
	Definition.Name = "UnusedTint";
	Definition.Type = Durin::EMaterialParameterType::Vector4;
	Definition.Value = Durin::FMaterialParameterValue::MakeVector4({1, 2, 3, 4});
	Durin::Testing::FTestMaterialExpressionGraph Graph;
	const std::array Definitions{Definition};
	Graph.Add(Durin::EMaterialProgramOpcode::Parameter, Durin::EMaterialProgramValueType::Float4, {}, Definition.Id, {}, Definitions);
	ASSERT_TRUE(Graph.Apply(*Material));
	const Durin::Editor::Material::FMaterialParameterPanelModel Model(Material);
	ASSERT_EQ(Model.GetEntries().size(), 1u);
	const auto& Entry = Model.GetEntries().front();
	EXPECT_EQ(Entry.ParameterId, Definition.Id);
	EXPECT_EQ(Entry.Control, Durin::Editor::Material::EMaterialParameterControlKind::Vector);
	EXPECT_EQ(Entry.Value.GetVector4(), Durin::FVector4(1, 2, 3, 4));
	EXPECT_FALSE(Entry.bOrphan);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, TypedResourceOutputsSkipUnusedUVDependencies)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
	Base->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto UV = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	UV->Metadata.Id = FGuid::NewGuid(); UV->Metadata.Name = "OnlySampleUV";
	auto Sample = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Sample->Metadata.Id = FGuid::NewGuid(); Sample->Metadata.Name = "SharedTexture";
	auto Mask = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>();
	Mask->Input = {UV->Id}; Mask->Components = {0, 1};
	Sample->UV = {Mask->Id};
	auto ResourceConsumer = Testing::MakeGraphExpression<DMaterialExpressionTextureSample2D>();
	ResourceConsumer->Texture = {Sample->Id, 7};
	const std::array<DMaterialExpression*, 4> Expressions{UV.Get(), Mask.Get(), Sample.Get(), ResourceConsumer.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {ResourceConsumer->Id, 1};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	Editor::Material::FMaterialParameterPanelModel Model(Instance.Get());
	EXPECT_NE(FindEntry(Model, Sample->Metadata.Id), nullptr);
	EXPECT_EQ(FindEntry(Model, UV->Metadata.Id), nullptr);
	Outputs.Normal = {Sample->Id, 1};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Model.Refresh());
	EXPECT_NE(FindEntry(Model, UV->Metadata.Id), nullptr);
	Outputs.Normal = {};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Model.Refresh());
	EXPECT_EQ(FindEntry(Model, UV->Metadata.Id), nullptr);
	EXPECT_NE(FindEntry(Model, Sample->Metadata.Id), nullptr);
}

TEST(FMaterialParameterPanelModelTests, ReachabilitySharesFunctionOutputAnalysisAndTracksTransitiveEdits)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(NewObject<DMaterial>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialInstance> Sibling(NewObject<DMaterialInstance>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, NAME_None));
	TStrongObjectPtr<DMaterialFunction> Wrapper(NewObject<DMaterialFunction>(nullptr, NAME_None));
	Base->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto A = FGuid::NewGuid(), B = FGuid::NewGuid();
	const auto X = FGuid::NewGuid(), Y = FGuid::NewGuid();
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {{.Id = A, .Type = EMaterialProgramValueType::Float, .Name = "A", .bRequired = true},
		{.Id = B, .Type = EMaterialProgramValueType::Float, .Name = "B", .bRequired = true}};
	Signature.Outputs = {{.Id = X, .Type = EMaterialProgramValueType::Float, .Name = "X"},
		{.Id = Y, .Type = EMaterialProgramValueType::Float, .Name = "Y"}};
	auto InputA = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(); InputA->Port.Id = A;
	auto InputB = Testing::MakeGraphExpression<DMaterialExpressionFunctionInput>(); InputB->Port.Id = B;
	auto OutputX = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>(); OutputX->Port.Id = X;
	auto OutputY = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>(); OutputY->Port.Id = Y;
	OutputX->Source = {InputA->Id}; OutputY->Source = {InputB->Id};
	const std::array<DMaterialExpression*, 4> Body{InputA.Get(), InputB.Get(), OutputX.Get(), OutputY.Get()};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	auto Nested = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>();
	Nested->Function = Function.Get();
	Nested->Inputs = {{A, EMaterialProgramValueType::Float, {InputA->Id}},
		{B, EMaterialProgramValueType::Float, {InputB->Id}}};
	Nested->Outputs = {{X, EMaterialProgramValueType::Float}, {Y, EMaterialProgramValueType::Float}};
	OutputX->Source = {Nested->Id, 0, X}; OutputY->Source = {Nested->Id, 0, Y};
	const std::array<DMaterialExpression*, 5> WrapperBody{InputA.Get(), InputB.Get(), OutputX.Get(), OutputY.Get(), Nested.Get()};
	ASSERT_TRUE(Wrapper->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, WrapperBody)));
	auto ParameterA = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	ParameterA->Metadata.Id = FGuid::NewGuid(); ParameterA->Metadata.Name = "ParameterA";
	auto ParameterB = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	ParameterB->Metadata.Id = FGuid::NewGuid(); ParameterB->Metadata.Name = "ParameterB";
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>();
	Call->Function = Wrapper.Get();
	Call->Inputs = {{A, EMaterialProgramValueType::Float, {ParameterA->Id}},
		{B, EMaterialProgramValueType::Float, {ParameterB->Id}}};
	Call->Outputs = Nested->Outputs;
	const std::array<DMaterialExpression*, 3> Expressions{ParameterA.Get(), ParameterB.Get(), Call.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Roughness = {Call->Id, 0, X};
	ASSERT_TRUE(Base->SetMaterialExpressions(Expressions, Outputs));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	ASSERT_TRUE(Sibling->SetParent(Base.Get()));
	const auto Initial = Instance->GetParameterReachability();
	ASSERT_TRUE(Initial->Validation);
	EXPECT_EQ(Initial, Base->GetParameterReachability());
	EXPECT_EQ(Initial, Sibling->GetParameterReachability());
	EXPECT_TRUE(Initial->ParameterIds.contains(ParameterA->Metadata.Id));
	EXPECT_FALSE(Initial->ParameterIds.contains(ParameterB->Metadata.Id));
	Editor::Material::FMaterialParameterPanelModel Model(Instance.Get());
	EXPECT_NE(FindEntry(Model, ParameterA->Metadata.Id), nullptr);
	EXPECT_EQ(FindEntry(Model, ParameterB->Metadata.Id), nullptr);
	ASSERT_TRUE(Instance->SetParameterValue(ParameterA->Metadata.Id, FMaterialParameterValue::MakeScalar(0.4f)));
	EXPECT_FALSE(Instance->SetParameterValue(ParameterB->Metadata.Id, FMaterialParameterValue::MakeScalar(0.4f)));
	EXPECT_EQ(Initial, Instance->GetParameterReachability());
	EXPECT_FALSE(Model.Refresh());

	// Only the nested function changes; neither the root nor wrapper revision advances.
	const auto RootRevision = Base->GetMaterialProgramRevision();
	const auto WrapperRevision = Wrapper->GetFunctionRevision();
	OutputX->Source = {InputB->Id}; OutputY->Source = {InputA->Id};
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	EXPECT_EQ(RootRevision, Base->GetMaterialProgramRevision());
	EXPECT_EQ(WrapperRevision, Wrapper->GetFunctionRevision());
	const auto Changed = Instance->GetParameterReachability();
	ASSERT_TRUE(Changed->Validation);
	EXPECT_NE(Initial, Changed);
	EXPECT_FALSE(Changed->ParameterIds.contains(ParameterA->Metadata.Id));
	EXPECT_TRUE(Changed->ParameterIds.contains(ParameterB->Metadata.Id));
	EXPECT_TRUE(Initial->ParameterIds.contains(ParameterA->Metadata.Id));
	EXPECT_TRUE(Instance->IsParameterValueOrphan(ParameterA->Metadata.Id));
	EXPECT_TRUE(Model.Refresh());
	ASSERT_NE(FindEntry(Model, ParameterA->Metadata.Id), nullptr);
	EXPECT_TRUE(FindEntry(Model, ParameterA->Metadata.Id)->bOrphan);
	EXPECT_NE(FindEntry(Model, ParameterB->Metadata.Id), nullptr);

	// A stale call signature fails analysis, then recovers after the dependency is repaired.
	auto BrokenSignature = Signature;
	BrokenSignature.Outputs[0].Id = FGuid::NewGuid();
	OutputX->Port.Id = BrokenSignature.Outputs[0].Id;
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(BrokenSignature, Body)));
	const auto Failed = Instance->GetParameterReachability();
	EXPECT_FALSE(Failed->Validation);
	EXPECT_TRUE(Failed->ParameterIds.empty());
	EXPECT_FALSE(Failed->Validation.Diagnostics.empty());
	OutputX->Port.Id = X;
	ASSERT_TRUE(Function->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	const auto Recovered = Instance->GetParameterReachability();
	ASSERT_TRUE(Recovered->Validation);
	EXPECT_TRUE(Recovered->ParameterIds.contains(ParameterB->Metadata.Id));
	EXPECT_EQ(Recovered, Sibling->GetParameterReachability());
}
