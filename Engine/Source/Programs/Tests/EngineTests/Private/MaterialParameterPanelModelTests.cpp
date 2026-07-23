#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialParameterPanelModel.h"

#include <gtest/gtest.h>

namespace
{
	auto FindEntry(
		const Durin::FMaterialParameterPanelModel& Model,
		const Durin::FGuid& ParameterId
	) -> const Durin::FMaterialParameterPanelEntry*
	{
		const auto Entries = Model.GetEntries();
		const auto It = std::ranges::find(Entries, ParameterId, &Durin::FMaterialParameterPanelEntry::ParameterId);
		return It == Entries.end() ? nullptr : &*It;
	}

	auto MakeContext(
		Durin::FEditorTransactionManager& Transactions,
		std::string& Error
	) -> Durin::FReflectedPropertyViewContext
	{
		return {
			.Transactions = &Transactions,
			.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
		};
	}
}

TEST(FMaterialParameterPanelModelTests, BuildsControlsAndResolvedSourceFromRuntimeSchema)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "PanelSchemaBase");
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelSchemaParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelSchemaChild");
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));
	ASSERT_TRUE(Parent->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.6f));

	const Durin::FMaterialParameterPanelModel Model(Child);
	ASSERT_EQ(Model.GetEntries().size(), 5u);
	const auto* BaseColor = FindEntry(Model, Durin::MaterialParameters::BaseColorId);
	const auto* Texture = FindEntry(Model, Durin::MaterialParameters::BaseColorTextureId);
	const auto* Opacity = FindEntry(Model, Durin::MaterialParameters::OpacityId);
	const auto* Shininess = FindEntry(Model, Durin::MaterialParameters::ShininessId);
	ASSERT_NE(BaseColor, nullptr);
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_NE(Shininess, nullptr);
	EXPECT_EQ(BaseColor->Control, Durin::EMaterialParameterControlKind::Color);
	EXPECT_EQ(Texture->Control, Durin::EMaterialParameterControlKind::AssetPicker);
	EXPECT_EQ(Opacity->Control, Durin::EMaterialParameterControlKind::RangedScalar);
	EXPECT_EQ(Shininess->Control, Durin::EMaterialParameterControlKind::RangedScalar);
	EXPECT_EQ(Opacity->Source, Parent);
	EXPECT_FLOAT_EQ(Opacity->Value.ScalarValue, 0.6f);
	EXPECT_FALSE(Opacity->bHasLocalOverride);
	EXPECT_TRUE(Opacity->bCanOverride);

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, GuidRootEditsSurviveIndexChangesAndCoalesceOrCancel)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "PanelEditBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelEditInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4)));

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::FMaterialParameterPanelModel InitialModel(Instance);
	const auto* InitialOpacity = FindEntry(InitialModel, Durin::MaterialParameters::OpacityId);
	ASSERT_NE(InitialOpacity, nullptr);
	ASSERT_TRUE(InitialModel.SetOverrideEnabled(PropertyView, Context, *InitialOpacity, true));
	ASSERT_TRUE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::OpacityId));

	Durin::FMaterialParameterPanelModel EditModel(Instance);
	const auto* Opacity = FindEntry(EditModel, Durin::MaterialParameters::OpacityId);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_TRUE(Opacity->bHasLocalOverride);
	// Removing an earlier entry changes Opacity's array index. The retained panel row
	// remains valid because every scratch mutation resolves the entry by GUID.
	ASSERT_TRUE(Instance->ClearParameterOverride(Durin::MaterialParameters::BaseColorId));

	auto FirstValue = Opacity->Value;
	FirstValue.ScalarValue = 0.4f;
	ASSERT_TRUE(EditModel.SubmitValueEdit(PropertyView, Context, *Opacity, FirstValue, true));
	auto FinalValue = Opacity->Value;
	FinalValue.ScalarValue = 0.25f;
	ASSERT_TRUE(EditModel.SubmitValueEdit(PropertyView, Context, *Opacity, FinalValue, true));
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, false));
	float Value = 0.0f;
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);

	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 1.0f);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);

	Durin::FMaterialParameterPanelModel CancelModel(Instance);
	const auto* CancelOpacity = FindEntry(CancelModel, Durin::MaterialParameters::OpacityId);
	ASSERT_NE(CancelOpacity, nullptr);
	auto CancelValue = CancelOpacity->Value;
	CancelValue.ScalarValue = 0.8f;
	ASSERT_TRUE(CancelModel.SubmitValueEdit(PropertyView, Context, *CancelOpacity, CancelValue, true));
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, true));
	ASSERT_TRUE(Instance->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Value));
	EXPECT_FLOAT_EQ(Value, 0.25f);
	EXPECT_TRUE(Error.empty());

	Transactions.Clear();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, ResetAndOrphanRemovalAreTransactional)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "PanelOrphanBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelOrphanInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.3f));

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::FMaterialParameterPanelModel OverrideModel(Instance);
	const auto* Opacity = FindEntry(OverrideModel, Durin::MaterialParameters::OpacityId);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_TRUE(OverrideModel.SetOverrideEnabled(PropertyView, Context, *Opacity, false));
	EXPECT_FALSE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::OpacityId));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::OpacityId));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FALSE(Instance->HasLocalParameterOverride(Durin::MaterialParameters::OpacityId));

	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.3f));
	ASSERT_TRUE(Instance->SetParent(nullptr));
	Durin::FMaterialParameterPanelModel OrphanModel(Instance);
	ASSERT_EQ(OrphanModel.GetEntries().size(), 1u);
	const auto& Orphan = OrphanModel.GetEntries().front();
	EXPECT_TRUE(Orphan.bOrphan);
	EXPECT_EQ(Orphan.ParameterId, Durin::MaterialParameters::OpacityId);
	ASSERT_TRUE(OrphanModel.RemoveOrphan(PropertyView, Context, Orphan));
	EXPECT_TRUE(Instance->GetParameterOverrides().empty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::OpacityId));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_TRUE(Instance->GetParameterOverrides().empty());
	EXPECT_TRUE(Error.empty());

	Transactions.Clear();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, BaseAndTexturePickerValuesUseSharedUndoHistory)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "PanelValueBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PanelValueInstance");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "PanelValueTexture");
	ASSERT_TRUE(Instance->SetParent(Base));

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::FMaterialParameterPanelModel BaseModel(Base);
	const auto* BaseOpacity = FindEntry(BaseModel, Durin::MaterialParameters::OpacityId);
	ASSERT_NE(BaseOpacity, nullptr);
	auto ScalarValue = BaseOpacity->Value;
	ScalarValue.ScalarValue = 0.7f;
	ASSERT_TRUE(BaseModel.SubmitValueEdit(PropertyView, Context, *BaseOpacity, ScalarValue, false));
	float Opacity = 0.0f;
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.7f);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.7f);

	Durin::FMaterialParameterPanelModel InheritedModel(Instance);
	const auto* InheritedTexture = FindEntry(InheritedModel, Durin::MaterialParameters::BaseColorTextureId);
	ASSERT_NE(InheritedTexture, nullptr);
	ASSERT_TRUE(InheritedModel.SetOverrideEnabled(PropertyView, Context, *InheritedTexture, true));
	Durin::FMaterialParameterPanelModel TextureModel(Instance);
	const auto* TextureEntry = FindEntry(TextureModel, Durin::MaterialParameters::BaseColorTextureId);
	ASSERT_NE(TextureEntry, nullptr);
	auto TextureValue = TextureEntry->Value;
	TextureValue.TextureValue = Texture;
	ASSERT_TRUE(TextureModel.SubmitValueEdit(PropertyView, Context, *TextureEntry, TextureValue, false));
	Durin::DTexture2D* ResolvedTexture = nullptr;
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, Texture);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, nullptr);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Instance->GetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), ResolvedTexture));
	EXPECT_EQ(ResolvedTexture, Texture);
	EXPECT_TRUE(Error.empty());

	Transactions.Clear();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage();
}

TEST(FMaterialParameterPanelModelTests, RootSnapshotContinuousSessionsRemainParameterScoped)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "PanelIdentityBase");
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::FMaterialParameterPanelModel Model(Base);
	const auto* Opacity = FindEntry(Model, Durin::MaterialParameters::OpacityId);
	const auto* BaseColor = FindEntry(Model, Durin::MaterialParameters::BaseColorId);
	ASSERT_NE(Opacity, nullptr);
	ASSERT_NE(BaseColor, nullptr);

	auto OpacityValue = Opacity->Value;
	OpacityValue.ScalarValue = 0.55f;
	ASSERT_TRUE(Model.SubmitValueEdit(PropertyView, Context, *Opacity, OpacityValue, true));
	auto ColorValue = BaseColor->Value;
	ColorValue.VectorValue = Durin::FVector3(0.1, 0.2, 0.3);
	ASSERT_TRUE(Model.SubmitValueEdit(PropertyView, Context, *BaseColor, ColorValue, true));
	// Switching logical GUIDs commits the first continuous edit. Cancelling the
	// second must not restore the entire collection to the first edit's origin.
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, true));

	float ResolvedOpacity = 0.0f;
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), ResolvedOpacity));
	EXPECT_FLOAT_EQ(ResolvedOpacity, 0.55f);
	Durin::FVector3 ResolvedColor;
	ASSERT_TRUE(Base->GetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), ResolvedColor));
	EXPECT_EQ(ResolvedColor, BaseColor->Value.VectorValue);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Base->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), ResolvedOpacity));
	EXPECT_FLOAT_EQ(ResolvedOpacity, 1.0f);
	EXPECT_TRUE(Error.empty());

	Transactions.Clear();
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}
