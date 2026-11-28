#include "NativePropertyControlTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "PropertyEditor/DetailLayoutBuilder.h"
#include "PropertyEditor/PropertyView.h"
#include "NativePackageUnloadTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "PropertyEditor/PropertyValueDraft.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"
#include "Math/Color.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Widgets/MonaImGuiPropertyTable.h"
#include "ThirdParty/ImGui/imgui_internal.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeAssetTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using FPropertyViewTestTransactorOwner = Durin::Tests::FTestEditorTransactorOwner;

	using FSoftObjectViewValue = Durin::TSoftObjectPtr<Durin::DObject>;
	using FSoftObjectViewArray = std::vector<FSoftObjectViewValue>;
	using FSoftObjectViewMap = std::unordered_map<std::string, FSoftObjectViewValue>;
	using FWeakObjectViewValue = Durin::TWeakObjectPtr<Durin::DObject>;

	template<typename T>
	auto InitializePropertyViewValue(void* Memory) -> void
	{
		std::construct_at(static_cast<T*>(Memory));
	}

	template<typename T>
	auto DestroyPropertyViewValue(void* Memory) -> void
	{
		std::destroy_at(static_cast<T*>(Memory));
	}

	template<typename T>
	auto CopyConstructPropertyViewValue(void* Destination, const void* Source) -> void
	{
		std::construct_at(static_cast<T*>(Destination), *static_cast<const T*>(Source));
	}

	template<typename T>
	auto CopyAssignPropertyViewValue(void* Destination, const void* Source) -> void
	{
		*static_cast<T*>(Destination) = *static_cast<const T*>(Source);
	}

	template<typename T>
	auto SetPropertyViewValueLifecycle(Durin::FProperty& Property) -> void
	{
		Property.SetValueLifecycle(
			sizeof(T), alignof(T),
			&InitializePropertyViewValue<T>, &DestroyPropertyViewValue<T>,
			&CopyConstructPropertyViewValue<T>, &CopyAssignPropertyViewValue<T>);
	}

	auto GetMutableSoftObjectViewValue(void* Value) -> Durin::FSoftObjectPtr*
	{
		return Value ? &static_cast<FSoftObjectViewValue*>(Value)->GetBase() : nullptr;
	}

	auto GetConstSoftObjectViewValue(const void* Value) -> const Durin::FSoftObjectPtr*
	{
		return Value ? &static_cast<const FSoftObjectViewValue*>(Value)->GetBase() : nullptr;
	}

	auto GetMutableWeakObjectViewValue(void* Value) -> Durin::FWeakObjectPtr*
	{
		return Value ? &static_cast<FWeakObjectViewValue*>(Value)->GetBase() : nullptr;
	}

	auto GetConstWeakObjectViewValue(const void* Value) -> const Durin::FWeakObjectPtr*
	{
		return Value ? &static_cast<const FWeakObjectViewValue*>(Value)->GetBase() : nullptr;
	}

	class DPropertyViewHostTestObject final : public Durin::DObject
	{
	public:
		explicit DPropertyViewHostTestObject(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{}

		explicit DPropertyViewHostTestObject(Durin::DClass* Class, Durin::FName Name)
			: DObject(Class, nullptr, std::move(Name))
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer)
		{
			new (Initializer.GetObj()) DPropertyViewHostTestObject(Initializer);
		}

		static auto StaticClass() -> Durin::DClass*;

		bool bEditable = true;
		auto CanEditChange(const Durin::FProperty* Property) const -> bool override
		{ return bEditable && DObject::CanEditChange(Property); }

		bool bLockOnProposal = false;
		auto PreEditChangeProperty(Durin::FPropertyEditProposal& Proposal)
			-> std::expected<void, Durin::FObjectValidationError> override
		{
			if (bLockOnProposal) bEditable = false;
			return DObject::PreEditChangeProperty(Proposal);
		}

		void PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) override
		{
			ChangeTypes.push_back(Event.ChangeType);
		}

		void PostTransacted(const Durin::FTransactionObjectEvent&) override { ++TransactionEvents; }

		int32 TransactionEvents = 0;
		std::vector<Durin::EPropertyChangeType::Type> ChangeTypes;
		int32 Value = 5;
		std::string Text;
		Durin::FVector3f FloatVector{0.0f};
		FSoftObjectViewValue SoftValues[2];
		FSoftObjectViewArray SoftArray;
		FSoftObjectViewMap SoftMap;
	};

	struct FPropertyViewHostTestReflection
	{
		FPropertyViewHostTestReflection()
		{
			Class = new Durin::DClass(
				Durin::EC_StaticConstructor,
				Durin::FName("DPropertyViewHostTestObject"),
				sizeof(DPropertyViewHostTestObject),
				alignof(DPropertyViewHostTestObject),
				Durin::EObjectFlags::Transient,
				Durin::EClassFlags::Native,
				Durin::EClassCastFlags::DClass,
				(Durin::DClass::ClassConstructorType)
					Durin::InternalConstructor<DPropertyViewHostTestObject>
			);
			Class->SetSuperStructBase(Durin::DObject::StaticClass());
			Class->SetTypeNames("DPropertyViewHostTestObject", "", "");
			DPropertyViewHostTestObject OffsetProbe(Class, Durin::FName("OffsetProbe"));
			const auto Offset = static_cast<uint16>(
				reinterpret_cast<const uint8*>(&OffsetProbe.Value)
				- reinterpret_cast<const uint8*>(&OffsetProbe)
			);
			Property = new Durin::FNumericProperty(
				Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 1, Offset, sizeof(int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
			);
			Property->SetValueLifecycle(sizeof(int32), alignof(int32),
				[](void* Memory) { std::construct_at(static_cast<int32*>(Memory)); },
				[](void* Memory) { std::destroy_at(static_cast<int32*>(Memory)); });
			const Durin::FPropertyMetadataParams Metadata{
				.DisplayName = "Bounded Value",
				.ToolTip = "A value with property-edit bounds.",
				.Category = "Numbers",
				.Step = Durin::FPropertyMetadataNumber::FromSigned(1),
				.ClampMin = Durin::FPropertyMetadataNumber::FromSigned(0),
				.ClampMax = Durin::FPropertyMetadataNumber::FromSigned(10),
				.UIMin = Durin::FPropertyMetadataNumber::FromSigned(2),
				.UIMax = Durin::FPropertyMetadataNumber::FromSigned(8),
			};
			Property->SetTypedMetadata(&Metadata);

			const auto SoftValuesOffset = static_cast<uint16>(
				reinterpret_cast<const uint8*>(&OffsetProbe.SoftValues)
				- reinterpret_cast<const uint8*>(&OffsetProbe));
			SoftProperty = new Durin::FSoftObjectProperty(
				Durin::FFieldVariant(Class), Durin::FName("SoftValues"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 2, SoftValuesOffset, sizeof(FSoftObjectViewValue),
				Durin::DObject::StaticClass(), &GetMutableSoftObjectViewValue, &GetConstSoftObjectViewValue);
			SetPropertyViewValueLifecycle<FSoftObjectViewValue>(*SoftProperty);

			ArrayInner = new Durin::FSoftObjectProperty(
				Durin::FFieldVariant(), Durin::FName("SoftArray_Inner"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::None, 1, 0, sizeof(FSoftObjectViewValue),
				Durin::DObject::StaticClass(), &GetMutableSoftObjectViewValue, &GetConstSoftObjectViewValue);
			SetPropertyViewValueLifecycle<FSoftObjectViewValue>(*ArrayInner);
			const auto SoftArrayOffset = static_cast<uint16>(
				reinterpret_cast<const uint8*>(&OffsetProbe.SoftArray)
				- reinterpret_cast<const uint8*>(&OffsetProbe));
			ArrayProperty = new Durin::FArrayProperty(
				Durin::FFieldVariant(Class), Durin::FName("SoftArray"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 1, SoftArrayOffset, sizeof(FSoftObjectViewArray),
				Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr,
				Durin::ResolveArrayOps<FSoftObjectViewArray>());
			ArrayProperty->SetInner(ArrayInner);
			SetPropertyViewValueLifecycle<FSoftObjectViewArray>(*ArrayProperty);

			MapKey = new Durin::FStringProperty(
				Durin::FFieldVariant(), Durin::FName("SoftMap_Key"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
				Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr);
			SetPropertyViewValueLifecycle<std::string>(*MapKey);
			MapValue = new Durin::FSoftObjectProperty(
				Durin::FFieldVariant(), Durin::FName("SoftMap_Value"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::None, 1, 0, sizeof(FSoftObjectViewValue),
				Durin::DObject::StaticClass(), &GetMutableSoftObjectViewValue, &GetConstSoftObjectViewValue);
			SetPropertyViewValueLifecycle<FSoftObjectViewValue>(*MapValue);
			const auto SoftMapOffset = static_cast<uint16>(
				reinterpret_cast<const uint8*>(&OffsetProbe.SoftMap)
				- reinterpret_cast<const uint8*>(&OffsetProbe));
			MapProperty = new Durin::FMapProperty(
				Durin::FFieldVariant(Class), Durin::FName("SoftMap"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 1, SoftMapOffset, sizeof(FSoftObjectViewMap),
				Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr,
				Durin::ResolveMapOps<FSoftObjectViewMap>());
			MapProperty->SetKeyProp(MapKey);
			MapProperty->SetValueProp(MapValue);
			SetPropertyViewValueLifecycle<FSoftObjectViewMap>(*MapProperty);

			TextProperty = new Durin::FStringProperty(
				Durin::FFieldVariant(Class), Durin::FName("Text"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 1, static_cast<uint16>(reinterpret_cast<const uint8*>(&OffsetProbe.Text)
					- reinterpret_cast<const uint8*>(&OffsetProbe)), sizeof(std::string),
				Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr);
			SetPropertyViewValueLifecycle<std::string>(*TextProperty);
			Property->SetNext(TextProperty);
			TextProperty->SetNext(SoftProperty);
			SoftProperty->SetNext(ArrayProperty);
			ArrayProperty->SetNext(MapProperty);
			Class->SetChildProperties(Property);
			Class->Register(Durin::DClass::StaticClass, "", "DPropertyViewHostTestObject");
			Durin::DObjectForceRegistration(Class);
		}

		Durin::DClass* Class = nullptr;
		Durin::FStringProperty* TextProperty = nullptr;
		Durin::FNumericProperty* Property = nullptr;
		Durin::FSoftObjectProperty* SoftProperty = nullptr;
		Durin::FArrayProperty* ArrayProperty = nullptr;
		Durin::FSoftObjectProperty* ArrayInner = nullptr;
		Durin::FMapProperty* MapProperty = nullptr;
		Durin::FStringProperty* MapKey = nullptr;
		Durin::FSoftObjectProperty* MapValue = nullptr;
	};

	auto GetPropertyViewHostTestReflection() -> FPropertyViewHostTestReflection&
	{
		static FPropertyViewHostTestReflection Reflection;
		return Reflection;
	}

	auto DPropertyViewHostTestObject::StaticClass() -> Durin::DClass*
	{
		return GetPropertyViewHostTestReflection().Class;
	}

		auto BeginPropertyViewHostPreview(
		Durin::FPropertyView& View,
		const Durin::FPropertyViewContext& Context,
		DPropertyViewHostTestObject& Object
	) -> bool
	{
		FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
		if (!View.HandleOwnerContext(Context, &Object)) return false;
		return View.SubmitPropertyValueEdit(
			Context,
			Durin::FPropertyEditTarget::ForMember(&Object, Reflection.Property),
			[](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
				*static_cast<int32*>(Property->GetValuePtr(Container, ArrayIndex)) = 8;
			},
			true
		);
	}

	auto EnsureSoftObjectPropertyViewMount() -> void
	{
		static const bool bInitialized = []() {
			InitializeDObjectSystem();
			const std::filesystem::path Root =
				Durin::Testing::CreateTestFixtureDirectory("SoftObjectPropertyView");
			Durin::Testing::RegisterMountPointForTests(
				"/SoftObjectPropertyView/", Root.generic_string() + "/");
			return true;
		}();
		(void)bInitialized;
	}

	auto MakeSoftObjectPropertyViewPath(std::string_view Name) -> Durin::FSoftObjectPath
	{
		EnsureSoftObjectPropertyViewMount();
		Durin::FSoftObjectPath Path;
		EXPECT_TRUE(Durin::FSoftObjectPath::TryCreate(
			std::format("/SoftObjectPropertyView/{}.{}", Name, Name), Path));
		return Path;
	}
}

TEST(FReflectedPropertyViewTests, HidesConventionalBoolPrefixFromDisplayName)
{
	using Durin::DurinCodeGen::EPropertyGenFlags;

	EXPECT_EQ(Durin::MakePropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool), "Simulate Physics");
	EXPECT_EQ(Durin::MakePropertyDisplayName("bUseHDR", EPropertyGenFlags::Bool), "Use HDR");
	EXPECT_EQ(Durin::MakePropertyDisplayName("border", EPropertyGenFlags::Bool), "border");
	EXPECT_EQ(Durin::MakePropertyDisplayName("b", EPropertyGenFlags::Bool), "b");
	EXPECT_EQ(Durin::MakePropertyDisplayName("GroundHeight", EPropertyGenFlags::Float), "Ground Height");
	EXPECT_EQ(Durin::MakePropertyDisplayName("URLValue", EPropertyGenFlags::String), "URL Value");
	EXPECT_EQ(Durin::MakePropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::String), "b Simulate Physics");
	EXPECT_EQ(Durin::MakePropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool, "Simulate Physics"), "Simulate Physics");
}

TEST(FReflectedPropertyViewTests, ArrayIndexEnumFiltersGenericArraysAndFallsBackForUnknownEnums)
{
	using namespace Durin;
	InitializeDObjectSystem();
	DClass TestClass(EC_StaticConstructor, FName("FEnumIndexedArrayViewTestObject"),
		sizeof(DObject), alignof(DObject), EObjectFlags::Transient,
		EClassFlags::Native, EClassCastFlags::DClass, nullptr);
	FNumericProperty Property(FFieldVariant(&TestClass), FName("Values"), EObjectFlags::Transient,
		EPropertyFlags::Edit, 8, 0, sizeof(float), DurinCodeGen::EPropertyGenFlags::Float, nullptr);
	TestClass.SetChildProperties(&Property);
	DObject Object(&TestClass, nullptr, FName("Object"));
	const auto CountRows = [&](std::string_view Search = {}) {
		FDetailLayoutBuilder Layout(Search);
		Layout.AddDefaultProperties(&Object);
		return Layout.GetVisibleRowCount();
	};
	Property.SetMetaData(FName("ArrayIndexEnum"), "Durin::ECollisionResponse");
	EXPECT_EQ(CountRows(), 3u);
	EXPECT_EQ(CountRows("Overlap"), 1u);
	EXPECT_EQ(MakePropertyLabel(Property, 1), "Overlap");
	Property.SetMetaData(FName("ArrayIndexEnum"), "Missing::Enum");
	EXPECT_EQ(CountRows(), 8u);
	EXPECT_EQ(CountRows("Values[1]"), 1u);
	EXPECT_EQ(MakePropertyLabel(Property, 1), "Values[1]");
}

TEST(FReflectedPropertyViewTests, CollisionResponsesCollapseAndRenderNamedChannels)
{
	InitializeDObjectSystem();
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "StructPropertyView");
	ASSERT_NE(Component, nullptr);
	Durin::FProperty* BodyInstance = Component->GetClass()->FindPropertyByName("BodyInstance");
	ASSERT_NE(BodyInstance, nullptr);
	auto* BodyStruct = static_cast<Durin::FStructProperty*>(BodyInstance)->GetStruct();
	auto* Responses = BodyStruct->FindPropertyByName("Responses");
	ASSERT_NE(Responses, nullptr);
	auto* ResponseStruct = static_cast<Durin::FStructProperty*>(Responses)->GetStruct();
	auto* Channels = ResponseStruct->FindPropertyByName("Responses");
	ASSERT_NE(Channels, nullptr);
	EXPECT_EQ(Channels->GetArrayDim(), 32u);
	EXPECT_EQ(Responses->GetMetaData(Durin::FName("DefaultCollapsed")), "true");
	EXPECT_EQ(Channels->GetMetaData(Durin::FName("ArrayIndexEnum")), "Durin::ECollisionChannel");
	const char* Labels[] = {"World Static", "World Dynamic", "Pawn", "Visibility", "Camera"};
	for (uint32 Index = 0; Index < 5; ++Index)
		EXPECT_EQ(Durin::MakePropertyLabel(*Channels, Index), Labels[Index]);
	const auto* ResponseEnum = static_cast<Durin::FEnumProperty*>(Channels)->GetEnum();
	ASSERT_NE(ResponseEnum, nullptr);
	EXPECT_EQ(ResponseEnum->GetValues().size(), 3u);

	ImGuiContext* ImContext = ImGui::CreateContext();
	ASSERT_NE(ImContext, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {800.0f, 600.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	auto DrawFrame = [&](bool bExpandResponses) {
		ImGui::NewFrame();
		ImGui::SetNextWindowSize({600.0f, 400.0f}, ImGuiCond_Always);
		ImGui::Begin("Struct Property View Test");
		const bool bTableOpen = Durin::MonaImGui::PropertyEdit::BeginTable("StructPropertyRows");
		if (bTableOpen)
		{
			if (bExpandResponses)
			{
				ImGui::PushID(BodyInstance);
				ImGui::PushID(0);
				ImGui::PushID("##Struct");
				ImGui::PushID(Responses);
				ImGui::PushID(0);
				ImGui::GetStateStorage()->SetInt(ImGui::GetID("##Struct"), 1);
				for (int Index = 0; Index < 5; ++Index) ImGui::PopID();
			}
			Durin::FPropertyView PropertyView;
			EXPECT_FALSE(PropertyView.EditProperty({}, Component, BodyInstance));
			EXPECT_EQ(ImGui::TableGetRowIndex(), bExpandResponses ? 9 : 4);
			Durin::MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::End();
		ImGui::Render();
		EXPECT_TRUE(bTableOpen);
	};
	DrawFrame(false);
	DrawFrame(true);
	ImGui::DestroyContext(ImContext);

	Durin::MarkObjectHierarchyAsGarbage(Component);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FReflectedPropertyViewTests, ObjectReplacementCommitsDrag)
{
	Durin::Testing::InitializeAssetSystemForTests();
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	(void)Reflection;
	auto* FirstObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, Durin::FName("First"));
	auto* SecondObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, Durin::FName("Second"));
	Durin::TStrongObjectPtr<DPropertyViewHostTestObject> FirstStrong(FirstObject);
	Durin::TStrongObjectPtr<DPropertyViewHostTestObject> SecondStrong(SecondObject);
	auto& First = *FirstObject;
	auto& Second = *SecondObject;
	Durin::FPropertyView View;
	uint32 ErrorReports = 0;
	const Durin::FPropertyViewContext Context{
		.bShouldTransact = false,
		.ReportPropertyError = [&](std::string) { ++ErrorReports; },
	};
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, Context, First));
	EXPECT_TRUE(View.IsEditingObject(&First));
	EXPECT_EQ(First.Value, 8);

	EXPECT_TRUE(View.HandleOwnerContext(Context, &Second));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(First.Value, 8);
	EXPECT_EQ(ErrorReports, 0u);
}

TEST(FReflectedPropertyViewTests, ReadOnlyTransitionCommitsDrag)
{
	Durin::Testing::InitializeAssetSystemForTests();
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	(void)Reflection;
	auto* ObjectPtr = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, Durin::FName("ReadOnly"));
	Durin::TStrongObjectPtr<DPropertyViewHostTestObject> ObjectStrong(ObjectPtr);
	auto& Object = *ObjectPtr;
	Durin::FPropertyView View;
	uint32 ErrorReports = 0;
	const Durin::FPropertyViewContext EditableContext{
		.bShouldTransact = false,
		.ReportPropertyError = [&](std::string) { ++ErrorReports; },
	};
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, EditableContext, Object));
	EXPECT_EQ(Object.Value, 8);

	Durin::FPropertyViewContext ReadOnlyContext = EditableContext;
	ReadOnlyContext.bReadOnly = true;
	EXPECT_TRUE(View.HandleOwnerContext(ReadOnlyContext, &Object));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Object.Value, 8);
	EXPECT_EQ(ErrorReports, 0u);
}

TEST(FReflectedPropertyViewTests, ReadOnlyCommitEndsRecordingAndAllowsImmediateHandoff)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> First(NewObject<DPropertyViewHostTestObject>(nullptr, "Outgoing"));
	TStrongObjectPtr<DPropertyViewHostTestObject> Next(NewObject<DPropertyViewHostTestObject>(nullptr, "Incoming"));
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	std::vector<std::string> Reports;
	const FPropertyViewContext Context{.ReportPropertyError = [&](std::string Message) { Reports.push_back(std::move(Message)); }};
	ASSERT_TRUE(BeginPropertyViewHostPreview(View, Context, *First));
	First->bEditable = false;
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, FPropertyEditTarget::ForMember(Next.Get(), Reflection.Property),
		[](FProperty* Field, void* Data, uint32 Index) { *Field->ContainerPtrToValuePtr<int32>(Data, Index) = 9; }, false));
	EXPECT_EQ(Reports, (std::vector<std::string>{"The property is no longer editable."}));
	EXPECT_EQ(First->Value, 8);
	EXPECT_EQ(Next->Value, 9);
	EXPECT_FALSE(View.IsEditing());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Next->Value, 5);
	EXPECT_EQ(First->Value, 8);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(First->Value, 5);
}

TEST(FReflectedPropertyViewTests, ReleaseEscapeAndDisappearanceCommitOneDrag)
{
	using namespace Durin;
	using namespace MonaImGui::PropertyEdit;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "InteractionCancel"));
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	int Errors = 0;
	const FPropertyViewContext Context{.ReportPropertyError = [&](std::string) { ++Errors; }};
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property);
	bool bLastSucceeded = false;
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.ConfigInputTrickleEventQueue = false;
	IO.DisplaySize = {600, 400}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	const auto Frame = [&](int Value, bool bActive = true, bool bVisible = true) {
		ImGui::NewFrame();
		ImGui::Begin("Continuous interaction");
		if (bVisible)
		{
			const auto Interaction = FPropertyInteractionState::FromWidget(ImGui::GetID("Value"), {bActive, !bActive}, false, Value >= 0);
			std::function<void(FProperty*, void*, uint32)> Assign;
			if (Value >= 0) Assign = [Value](FProperty* Property, void* Container, uint32 Index) {
				*Property->ContainerPtrToValuePtr<int32>(Container, Index) = Value;
			};
			bLastSucceeded = View.UpdatePropertyValueEdit(Context, Target, Interaction, Assign);
		}
		View.EndInteractionFrame(Context);
		ImGui::End(); ImGui::Render();
	};
	Frame(-1);
	EXPECT_TRUE(bLastSucceeded);
	EXPECT_FALSE(View.IsEditing());
	Frame(7); Frame(8);
	EXPECT_TRUE(bLastSucceeded);
	EXPECT_TRUE(View.IsEditing());
	Frame(8, false);
	EXPECT_EQ(Object->ChangeTypes, (std::vector{EPropertyChangeType::Interactive, EPropertyChangeType::Interactive, EPropertyChangeType::ValueSet}));
	EXPECT_FALSE(View.IsEditing());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	Transactions.Get()->Reset();
	Frame(8);
	IO.AddKeyEvent(ImGuiKey_Escape, true);
	Frame(9, false);
	EXPECT_EQ(Object->Value, 8);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(View.IsEditing());
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	IO.AddKeyEvent(ImGuiKey_Escape, false);
	Frame(8);
	Frame(-1, false, false);
	EXPECT_EQ(Object->Value, 8);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Errors, 0);
}

TEST(FReflectedPropertyViewTests, SwitchCommitsOutgoingControlBeforeIncomingCancellation)
{
	using namespace Durin;
	using namespace MonaImGui::PropertyEdit;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "WidgetSwitch"));
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
	auto& IO = ImGui::GetIO();
	IO.DisplaySize = {600, 400}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault(); IO.Fonts->Build();
	ImGui::NewFrame();
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	const FPropertyViewContext Context{};
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property);
	const auto Write = [&](ImGuiID Id, int Value) {
		return View.UpdatePropertyValueEdit(Context, Target, {.Id = Id},
			[Value](FProperty* Property, void* Container, uint32 Index) {
				*Property->ContainerPtrToValuePtr<int32>(Container, Index) = Value;
			});
	};
	ASSERT_TRUE(Write(1, 7));
	ASSERT_TRUE(Write(2, 9));
	ASSERT_TRUE(View.FinishActiveEdit(&Context, true));
	EXPECT_EQ(Object->Value, 9);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Object->Value, 7);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions->CanUndo());
	ImGui::EndFrame();
}

TEST(FReflectedPropertyViewTests, PreFinalizeListenerCanCommitNestedPropertyEdit)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "NestedFinish"));
	FPropertyViewTestTransactorOwner Transactions;
	ASSERT_EQ(Transactions->Begin("test", "Outer"), 0);
	const auto Listener = Transactions->OnTransactionStateChanged().AddLambda([&](const FTransactionContext&, ETransactionStateEventType State) {
		if (State != ETransactionStateEventType::PreTransactionFinalized) return;
		PropertyEditor::FPropertyControl Edit;
		ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property)), true));
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, 7));
		ASSERT_TRUE(Edit.Finish(false));
	});
	EXPECT_EQ(Transactions->End(), 1);
	Transactions->OnTransactionStateChanged().Remove(Listener);
	EXPECT_EQ(Object->Value, 7);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions->CanUndo());
}

TEST(FReflectedPropertyViewTests, FinishingPropertyEditFromTransactionCanceledIsIllegal)
{
	using namespace Durin;
	const auto FinishDuringCancel = [](bool bCommit, bool bChanged) {
		Testing::InitializeAssetSystemForTests();
		auto& Reflection = GetPropertyViewHostTestReflection();
		TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "ReentrantFinish"));
		FPropertyViewTestTransactorOwner Transactions;
		PropertyEditor::FPropertyControl Edit;
		ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property)), true));
		if (bChanged) ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, 7));
		Transactions->OnTransactionStateChanged().AddLambda([&](const FTransactionContext&, ETransactionStateEventType State) {
			if (State != ETransactionStateEventType::TransactionCanceled) return;
			if (bCommit) Edit.Finish(false);
			else if (bChanged) Edit.Finish(true);
			else Edit.Finish(false);
		});
		Transactions->Cancel();
	};
	EXPECT_DEATH(FinishDuringCancel(true, true), "cannot finish a property gesture during a protected transaction operation");
	EXPECT_DEATH(FinishDuringCancel(false, true), "cannot finish a property gesture during a protected transaction operation");
	EXPECT_DEATH(FinishDuringCancel(false, false), "cannot finish a property gesture during a protected transaction operation");
}

TEST(FReflectedPropertyViewTests, SoftObjectStateInspectionDoesNotLoadUntilRequested)
{
	EnsureSoftObjectPropertyViewMount();
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject Object(Reflection.Class, Durin::FName("SoftState"));

	auto State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Null);
	EXPECT_EQ(Durin::GetSoftObjectStateLabel(State.State), "Null");

	const Durin::FSoftObjectPath MissingPath = MakeSoftObjectPropertyViewPath("Missing");
	Object.SoftValues[0] = MissingPath;
	State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Missing);
	const auto MissingState = State;
	EXPECT_EQ(MissingState.PropertyName, Reflection.SoftProperty->NamePrivate.ToString());
	EXPECT_EQ(MissingState.ArrayIndex, 0u);
	ASSERT_FALSE(State.AssetDiagnostic.empty());
	EXPECT_EQ(State.Error, Durin::ESoftObjectViewError::Asset);
	EXPECT_FALSE(Object.SoftValues[0].IsValid());
	Durin::DObject* LoadedObject = nullptr;
	const auto Missing = Durin::LoadSoftObjectProperty(Reflection.SoftProperty, &Object, 0, LoadedObject);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Error, Durin::EPropertySoftLoadError::Asset);
	ASSERT_FALSE(Missing.AssetDiagnostic.empty());
	EXPECT_FALSE(Missing.AssetDiagnostic.empty());
	EXPECT_EQ(Missing.Path, MissingPath);
	EXPECT_EQ(LoadedObject, nullptr);

	EXPECT_EQ(Object.SoftValues[0].ToSoftObjectPath(), MissingPath);

	const Durin::FSoftObjectPath AssetSoftPath = MakeSoftObjectPropertyViewPath("Loadable");
	const Durin::FPackagePath AssetPath = AssetSoftPath.GetPackagePath();
	Durin::DObject* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AssetPath, Asset));
	ASSERT_NE(Asset, nullptr);
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	Object.SoftValues[0] = AssetSoftPath;
	State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Loaded);
	EXPECT_EQ(MissingState.Path, MissingPath);
	ASSERT_FALSE(MissingState.AssetDiagnostic.empty());
	EXPECT_EQ(MissingState.Error, Durin::ESoftObjectViewError::Asset);
	EXPECT_EQ(State.LoadedObject, Asset);

	Durin::FSoftObjectProperty MismatchedProperty(
		Durin::FFieldVariant(), Durin::FName("Mismatched"), Durin::EObjectFlags::Transient,
		Durin::EPropertyFlags::Edit, 2, Reflection.SoftProperty->GetOffset(),
		sizeof(FSoftObjectViewValue), Durin::DPackage::StaticClass(),
		&GetMutableSoftObjectViewValue, &GetConstSoftObjectViewValue);
	State = Durin::InspectSoftObject(&MismatchedProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::TypeMismatch);
	ASSERT_FALSE(State.AssetDiagnostic.empty());
	EXPECT_EQ(State.Error, Durin::ESoftObjectViewError::Asset);

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	EXPECT_FALSE(Object.SoftValues[0].IsValid());
	State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Unloaded);
	EXPECT_EQ(State.LoadedObject, nullptr);
	EXPECT_FALSE(Object.SoftValues[0].IsValid());

	ASSERT_TRUE(Durin::LoadSoftObjectProperty(
		Reflection.SoftProperty, &Object, 0, LoadedObject));
	ASSERT_NE(LoadedObject, nullptr);
	EXPECT_EQ(Missing.Path, MissingPath);
	EXPECT_TRUE(Object.SoftValues[0].IsValid());
	State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Loaded);

	const Durin::FSoftObjectPath AliasSoftPath = MakeSoftObjectPropertyViewPath("AliasXXX");
	const Durin::FPackagePath AliasPath = AliasSoftPath.GetPackagePath();
	Durin::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Testing::CreateAssetRedirectorForTests(AliasPath, AssetPath, Redirector));
	ASSERT_TRUE(Durin::SavePackage(Redirector->GetPackage()));
	Durin::TStrongObjectPtr<Durin::DObject> TargetOwner(LoadedObject);
	ASSERT_TRUE(Durin::UnloadPackage(AliasPath));
	Object.SoftValues[0] = AliasSoftPath;
	State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Redirected);
	EXPECT_EQ(State.Path, AliasSoftPath);
	EXPECT_EQ(State.ResolvedPath, AssetSoftPath);
	EXPECT_EQ(State.LoadedObject, LoadedObject);
	EXPECT_EQ(State.Error, Durin::ESoftObjectViewError::None);
	EXPECT_TRUE(State.AssetDiagnostic.empty());
	EXPECT_EQ(Durin::GetSoftObjectStateLabel(State.State), "Redirected");

	TargetOwner.Reset();
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	State = Durin::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectViewState::Redirected);
	EXPECT_EQ(State.LoadedObject, nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	ASSERT_TRUE(Durin::LoadSoftObjectProperty(
		Reflection.SoftProperty, &Object, 0, LoadedObject));
	EXPECT_EQ(LoadedObject->GetPackage()->GetPackagePath(), AssetPath.ToString());
	EXPECT_EQ(Object.SoftValues[0].ToSoftObjectPath(), AliasSoftPath);
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, AssetPath}));
}

TEST(FReflectedPropertyViewTests, WeakObjectInspectionDistinguishesNullLiveAndExpired)
{
	Durin::Testing::InitializeAssetSystemForTests();
	Durin::FWeakObjectProperty Property(
		Durin::FFieldVariant(), Durin::FName("Weak"), Durin::EObjectFlags::Transient,
		Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::Transient, 1, 0,
		sizeof(FWeakObjectViewValue), Durin::DObject::StaticClass(),
		&GetMutableWeakObjectViewValue, &GetConstWeakObjectViewValue);
	FWeakObjectViewValue Value;
	auto State = Durin::InspectWeakObject(&Property, &Value);
	EXPECT_EQ(State.State, Durin::EWeakObjectViewState::Null);

	Durin::DObject* Target = Durin::NewObject<Durin::DObject>(nullptr, "WeakViewTarget");
	Value = Target;
	State = Durin::InspectWeakObject(&Property, &Value);
	EXPECT_EQ(State.State, Durin::EWeakObjectViewState::Live);
	EXPECT_EQ(State.Object, Target);
	EXPECT_EQ(Durin::GetWeakObjectStateLabel(State.State), "Live");

	Durin::MarkAsGarbage(Target);
	State = Durin::InspectWeakObject(&Property, &Value);
	EXPECT_EQ(State.State, Durin::EWeakObjectViewState::Expired);
	EXPECT_EQ(State.Object, nullptr);
}

TEST(FReflectedPropertyViewTests, SoftObjectPathEditsUndoRedoFixedArrayArrayAndMapValues)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	auto* ManagedObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, "SoftTransactions");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(ManagedObject);
	auto& Object = *ManagedObject;
	const Durin::FSoftObjectPath First = MakeSoftObjectPropertyViewPath("First");
	const Durin::FSoftObjectPath Second = MakeSoftObjectPropertyViewPath("Second");
	const Durin::FSoftObjectPath Third = MakeSoftObjectPropertyViewPath("Third");
	const Durin::FSoftObjectPath Fourth = MakeSoftObjectPropertyViewPath("Fourth");
	Object.SoftValues[1] = First;
	Object.SoftArray.emplace_back(First);
	Object.SoftMap.emplace("Alpha", FSoftObjectViewValue(First));

	FPropertyViewTestTransactorOwner Transactions;
	Durin::FPropertyView View;
	const Durin::FPropertyViewContext Context{
		};
	auto AssignPath = [](Durin::FSoftObjectPath Path) {
		return [Path = std::move(Path)](
			Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
			auto* Reference = static_cast<Durin::FSoftObjectProperty*>(Property)
				->GetSoftObjectPtr(Container, ArrayIndex);
			ASSERT_NE(Reference, nullptr);
			*Reference = Path;
		};
	};

	ASSERT_TRUE(View.SubmitPropertyValueEdit(
		Context,
		Durin::FPropertyEditTarget::ForMember(&Object, Reflection.SoftProperty, 1),
		AssignPath(Second), false));
	EXPECT_EQ(Object.SoftValues[1].ToSoftObjectPath(), Second);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object.SoftValues[1].ToSoftObjectPath(), First);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object.SoftValues[1].ToSoftObjectPath(), Second);
	Transactions.Get()->Reset();

	const Durin::FPropertyEditTarget ArrayTarget =
		Durin::FPropertyEditTarget::ForMember(&Object, Reflection.ArrayProperty)
			.ForArrayElement(Reflection.ArrayInner, 0);
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, ArrayTarget, AssignPath(Third), false));
	EXPECT_EQ(Object.SoftArray[0].ToSoftObjectPath(), Third);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object.SoftArray[0].ToSoftObjectPath(), First);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object.SoftArray[0].ToSoftObjectPath(), Third);
	Transactions.Get()->Reset();

	const std::string Alpha = "Alpha";
	Durin::FPropertyValueSnapshot KeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Reflection.MapKey, &Alpha, 0, KeySnapshot));
	const Durin::FPropertyEditTarget MapTarget =
		Durin::FPropertyEditTarget::ForMember(&Object, Reflection.MapProperty)
			.ForMapEntry(Reflection.MapValue, KeySnapshot, KeySnapshot.GetBytes());
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, MapTarget, AssignPath(Fourth), false));
	EXPECT_EQ(Object.SoftMap.at("Alpha").ToSoftObjectPath(), Fourth);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object.SoftMap.at("Alpha").ToSoftObjectPath(), First);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object.SoftMap.at("Alpha").ToSoftObjectPath(), Fourth);
}

TEST(FReflectedPropertyViewTests, MapKeyInteractionRetainsIdentityAndCommitsHiddenOrEscapedEdits)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	for (int EndMode = 0; EndMode < 4; ++EndMode)
	{
		TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "MapInteraction"));
		Object->SoftMap["Alpha"] = nullptr;
		if (EndMode == 3) Object->SoftMap["Reserved"] = nullptr;
		FPropertyViewTestTransactorOwner Transactions;
		FPropertyView View;
		const FPropertyViewContext Context{.ReportPropertyError = [&](std::string Error) {
			if (EndMode == 3) EXPECT_EQ(Error, "Map keys must be unique.");
			else ADD_FAILURE() << Error;
		}};
		const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
		auto& IO = ImGui::GetIO();
		IO.ConfigInputTrickleEventQueue = false;
		IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		IO.DisplaySize = {900, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		const auto Frame = [&](bool bVisible = true) {
			ImGui::NewFrame();
			ImGui::SetNextWindowSize({800, 500});
			ImGui::Begin("Map interaction");
			if (bVisible && MonaImGui::PropertyEdit::BeginTable("MapRows"))
			{
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
				View.EditProperty(Context, Object.Get(), Reflection.MapProperty);
				MonaImGui::PropertyEdit::EndTable();
			}
			View.EndInteractionFrame(Context);
			ImGui::End(); ImGui::Render();
		};
		const auto FocusKey = [&] {
			Frame();
			auto* Window = ImGui::FindWindowByName("Map interaction");
			ImGui::FocusWindow(Window);
			ImGui::NavInitWindow(Window, true);
			Frame();
			for (int Attempt = 0; Attempt < 24 && (GImGui->ActiveId == 0 || !ImGui::GetInputTextState(GImGui->ActiveId)
				|| std::string_view(GImGui->InputTextState.GetText()) != "Alpha"); ++Attempt)
			{
				IO.AddKeyEvent(ImGuiKey_Tab, true); Frame();
				IO.AddKeyEvent(ImGuiKey_Tab, false); Frame();
			}
			ASSERT_NE(GImGui->ActiveId, 0u);
			ASSERT_NE(ImGui::GetInputTextState(GImGui->ActiveId), nullptr);
			EXPECT_STREQ(GImGui->InputTextState.TextA.Data, "Alpha");
		};
		const auto Rename = [&](const char* Name) {
			IO.AddKeyEvent(ImGuiMod_Ctrl, true); IO.AddKeyEvent(ImGuiKey_A, true); Frame();
			IO.AddKeyEvent(ImGuiKey_A, false); IO.AddKeyEvent(ImGuiMod_Ctrl, false);
			IO.AddInputCharactersUTF8(Name); Frame(); Frame();
			EXPECT_TRUE(Object->SoftMap.contains(Name)) << "Expected " << Name << ", actual "
				<< Object->SoftMap.begin()->first << ", active widget " << GImGui->ActiveId;
		};
		Frame(); Frame(); FocusKey();
		Rename("Beta"); Rename("Gamma");
		EXPECT_TRUE(View.IsEditing());
		if (EndMode == 0)
		{
			IO.AddKeyEvent(ImGuiKey_Enter, true); Frame();
			IO.AddKeyEvent(ImGuiKey_Enter, false); Frame();
			EXPECT_FALSE(View.IsEditing());
			ASSERT_TRUE(Transactions.Get()->Undo());
		}
		else if (EndMode == 1)
		{
			IO.AddKeyEvent(ImGuiKey_Escape, true); Frame();
			IO.AddKeyEvent(ImGuiKey_Escape, false); Frame();
		}
		else if (EndMode == 2) Frame(false);
		else if (EndMode == 3)
		{
			Rename("Reserved");
			EXPECT_TRUE(Object->SoftMap.contains("Gamma"));
			EXPECT_TRUE(View.IsEditing());
			IO.AddKeyEvent(ImGuiKey_Escape, true); Frame();
			IO.AddKeyEvent(ImGuiKey_Escape, false); Frame();
			EXPECT_TRUE(Object->SoftMap.contains("Reserved"));
		}
		if (EndMode != 0)
		{
			EXPECT_TRUE(Object->SoftMap.contains("Gamma"));
			ASSERT_TRUE(Transactions.Get()->Undo());
		}
		EXPECT_TRUE(Object->SoftMap.contains("Alpha"));
		EXPECT_FALSE(View.IsEditing());
		EXPECT_FALSE(Transactions.Get()->CanUndo());
	}
}

TEST(FReflectedPropertyViewTests, IntegerGestureReturningToOriginalPublishesOneFinalNotification)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "IntegerGesture"));
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyControl Control;
	ASSERT_TRUE(Control.BeginGesture(MakePropertyHandle(*Object, Reflection.Property->NamePrivate), true));
	EXPECT_TRUE(Object->ChangeTypes.empty());
	ASSERT_TRUE(Testing::ApplyControlInput(Control, 8));
	ASSERT_TRUE(Testing::ApplyControlInput(Control, 5));
	ASSERT_TRUE(Control.Finish(false));
	EXPECT_EQ(Object->ChangeTypes, (std::vector<EPropertyChangeType::Type>{
		EPropertyChangeType::Interactive, EPropertyChangeType::Interactive, EPropertyChangeType::ValueSet}));
	EXPECT_EQ(Object->TransactionEvents, 0);
	EXPECT_EQ(Transactions->GetQueueLength(), 0u);
	EXPECT_FALSE(Transactions->CanUndo());
}

TEST(FReflectedPropertyViewTests, NumberAndTextHandoffHasTheSameUndoBoundariesInBothDrawOrders)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	for (bool bTextFirst : {false, true})
	{
		SCOPED_TRACE(bTextFirst);
		TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "NumberAndText"));
		FPropertyViewTestTransactorOwner Transactions;
		FPropertyView View;
		const FPropertyViewContext Context{.ReportPropertyError = [](std::string Error) { ADD_FAILURE() << Error; }};
		const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
		auto& IO = ImGui::GetIO();
		IO.ConfigInputTrickleEventQueue = false;
		IO.DisplaySize = {800, 600}; IO.DeltaTime = 1.f / 60; IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault(); IO.Fonts->Build();
		ImVec2 NumberPos, TextPos;
		const auto Draw = [&](bool bHandoff = false) {
			ImGui::NewFrame();
			// Release the numeric text input before either row can consume the new field's character.
			if (bHandoff) ImGui::ClearActiveID();
			ImGui::SetNextWindowSize({600, 300});
			ImGui::Begin("Control handoff");
			if (MonaImGui::PropertyEdit::BeginTable("Rows"))
			{
				const auto Row = [&](FProperty* Property, ImVec2& Position) {
					View.EditProperty(Context, Object.Get(), Property);
					const auto Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
					Position = {(Min.x + Max.x) * .5f, (Min.y + Max.y) * .5f};
				};
				if (bTextFirst) Row(Reflection.TextProperty, TextPos);
				Row(Reflection.Property, NumberPos);
				if (!bTextFirst) Row(Reflection.TextProperty, TextPos);
				MonaImGui::PropertyEdit::EndTable();
			}
			if (bHandoff)
			{
				EXPECT_TRUE(View.IsEditingTarget(FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property)));
				EXPECT_TRUE(Object->Text.empty());
			}
			EXPECT_TRUE(View.EndInteractionFrame(Context));
			ImGui::End(); ImGui::Render();
		};
		Draw(); Draw();
		IO.AddMousePosEvent(NumberPos.x, NumberPos.y);
		IO.AddKeyEvent(ImGuiMod_Ctrl, true);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true); Draw();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		IO.AddKeyEvent(ImGuiMod_Ctrl, false); Draw();
		IO.AddInputCharactersUTF8("7"); Draw();
		ASSERT_EQ(Object->Value, 7);
		IO.AddMousePosEvent(TextPos.x, TextPos.y);
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		IO.AddInputCharactersUTF8("x"); Draw(true);
		ASSERT_EQ(Object->Text, "x");
		EXPECT_TRUE(View.IsEditingTarget(FPropertyEditTarget::ForMember(Object.Get(), Reflection.TextProperty)));
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false); Draw();
		IO.AddKeyEvent(ImGuiKey_Enter, true); Draw();
		IO.AddKeyEvent(ImGuiKey_Enter, false); Draw();
		ASSERT_FALSE(View.IsEditing());
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_TRUE(Object->Text.empty());
		EXPECT_EQ(Object->Value, 7);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Object->Value, 5);
		EXPECT_FALSE(Transactions->CanUndo());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Object->Value, 7);
		EXPECT_TRUE(Object->Text.empty());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_EQ(Object->Text, "x");
	}
}

TEST(FReflectedPropertyViewTests, ColorPickerHandoffToIntegerHasTheSameHistoryInBothDrawOrders)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	using EEnd = MonaImGui::PropertyEdit::EPropertyInteractionEnd;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	const FLinearColor Preview{0.25f, 0.5f, 0.75f};
	const FLinearColor Committed{0.75f, 0.25f, 0.5f};
	for (bool bDrawIncomingFirst : {false, true})
	{
		SCOPED_TRACE(bDrawIncomingFirst);
		TStrongObjectPtr<DDirectionalLightComponent> Light(NewObject<DDirectionalLightComponent>(
			nullptr, bDrawIncomingFirst ? "LightIncomingFirst" : "LightOutgoingFirst"));
		TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(
			nullptr, bDrawIncomingFirst ? "NumberIncomingFirst" : "NumberOutgoingFirst"));
		FPropertyViewTestTransactorOwner Transactions;
		const auto* ColorProperty = Light->GetClass()->FindPropertyByName("Color");
		ASSERT_NE(ColorProperty, nullptr);
		const auto ColorHandle = MakePropertyHandle(FPropertyEditTarget::ForMember(Light.Get(), ColorProperty));
		const auto NumberHandle = MakePropertyHandle(*Object, Reflection.Property->NamePrivate);
		FLinearColor Original;
		ASSERT_EQ(ColorHandle->GetValue(Original), Durin::PropertyEditor::FPropertyAccess::Success);
		using FValue = std::variant<int32, FLinearColor>;
		TPropertyControlFrame<ImGuiID, std::shared_ptr<IPropertyHandle>, FValue> Frame;
		FPropertyControl Control;
		const auto EndFrame = [&] {
			return Frame.EndFrame(false,
				[&](ImGuiID, const auto& Handle) { return Control.BeginGesture(Handle, true, Handle == ColorHandle); },
				[&](const FValue& Value) { return std::visit([&](const auto& V) { return Testing::ApplyControlInput(Control, V); }, Value); },
				[&](bool bRestore) { return Control.Finish(bRestore); });
		};

		Frame.Observe(1, ColorHandle, EEnd::None, Preview);
		ASSERT_TRUE(EndFrame());
		ASSERT_TRUE(Frame.IsActive());

		const auto DrawPicker = [&] { Frame.Observe(1, ColorHandle, EEnd::Commit, Committed); };
		const auto DrawNumber = [&] { Frame.Observe(2, NumberHandle, EEnd::Commit, 9); };
		if (bDrawIncomingFirst) { DrawNumber(); DrawPicker(); }
		else { DrawPicker(); DrawNumber(); }
		ASSERT_TRUE(EndFrame());
		EXPECT_FALSE(Frame.IsActive());

		FLinearColor Color;
		ASSERT_EQ(ColorHandle->GetValue(Color), Durin::PropertyEditor::FPropertyAccess::Success);
		EXPECT_EQ(Color, Committed);
		EXPECT_EQ(Object->Value, 9);
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_EQ(Object->Value, 5);
		ASSERT_EQ(ColorHandle->GetValue(Color), Durin::PropertyEditor::FPropertyAccess::Success);
		EXPECT_EQ(Color, Committed);
		ASSERT_TRUE(Transactions->Undo());
		ASSERT_EQ(ColorHandle->GetValue(Color), Durin::PropertyEditor::FPropertyAccess::Success);
		EXPECT_EQ(Color, Original);
		EXPECT_FALSE(Transactions->CanUndo());
		ASSERT_TRUE(Transactions->Redo());
		ASSERT_TRUE(Transactions->Redo());
		ASSERT_EQ(ColorHandle->GetValue(Color), Durin::PropertyEditor::FPropertyAccess::Success);
		EXPECT_EQ(Color, Committed);
		EXPECT_EQ(Object->Value, 9);
		EXPECT_FALSE(Transactions->CanRedo());
	}
}

TEST(FReflectedPropertyViewTests, ReflectedIntegerHandleReadsAndWritesInOneTransaction)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	Testing::InitializeAssetSystemForTests();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "IntegerHandle"));
	FPropertyViewTestTransactorOwner Transactions;
	FDetailLayoutBuilder Layout;
	Layout.ApplyRegisteredCustomizations(Object.Get());
	EXPECT_FALSE(Layout.GetProperty("UnknownMember")->IsEditable());
	const auto Reflected = Layout.GetProperty(GetPropertyViewHostTestReflection().Property->NamePrivate);
	IPropertyHandle& Handle = *Reflected;
	int32 Value = 0;
	ASSERT_EQ(Handle.GetValue(Value), Durin::PropertyEditor::FPropertyAccess::Success);
	EXPECT_EQ(Value, 5);
	EXPECT_TRUE(Handle.IsEditable());
	EXPECT_EQ(Handle.SetValue(8), Durin::PropertyEditor::FPropertyAccess::Success);
	ASSERT_EQ(Handle.GetValue(Value), Durin::PropertyEditor::FPropertyAccess::Success);
	EXPECT_EQ(Value, 8);
	EXPECT_EQ(Handle.SetValue(8), Durin::PropertyEditor::FPropertyAccess::Success);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions->CanUndo());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Object->Value, 8);
	Transactions->Reset();
	Object->ChangeTypes.clear();
	ASSERT_EQ(Handle.SetValue(1, EPropertyValueSetFlags::NotTransactable), Durin::PropertyEditor::FPropertyAccess::Success);
	EXPECT_FALSE(Transactions->CanUndo());
	{
		FScopedTransaction Gesture("Slider");
		ASSERT_EQ(Handle.SetValue(2, EPropertyValueSetFlags::InteractiveChange | EPropertyValueSetFlags::NotTransactable), Durin::PropertyEditor::FPropertyAccess::Success);
		EXPECT_TRUE(Gesture.IsOutstanding());
		EXPECT_EQ(Object->Value, 2);
		ASSERT_EQ(Handle.SetValue(2), Durin::PropertyEditor::FPropertyAccess::Success);
		ASSERT_EQ(Handle.SetValue(2), FPropertyAccess::Success);
		float IncompatibleValue = 0;
		EXPECT_EQ(Handle.GetValue(IncompatibleValue), FPropertyAccess::Fail);
		EXPECT_EQ(Object->ChangeTypes, (std::vector{EPropertyChangeType::ValueSet, EPropertyChangeType::Interactive,
			EPropertyChangeType::ValueSet}));
	}
	ASSERT_EQ(Transactions->GetQueueLength(), 1u);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Object->Value, 1);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Object->Value, 2);
}

TEST(FReflectedPropertyViewTests, NumericWidgetClampsDraggingAndManualInputWithoutErrors)
{
	Durin::Testing::InitializeAssetSystemForTests();
	using namespace Durin;
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	const FPropertyMetadata Original = Reflection.Property->GetTypedMetadata();
	const FPropertyMetadataParams OriginalParams{
		.DisplayName = Original.DisplayName.c_str(), .ToolTip = Original.ToolTip.c_str(),
		.Category = Original.Category.c_str(), .Units = Original.Units, .Step = Original.Step,
		.Precision = Original.Precision, .ClampMin = Original.ClampMin, .ClampMax = Original.ClampMax,
		.UIMin = Original.UIMin, .UIMax = Original.UIMax,
	};
	struct FCase
	{
		FPropertyMetadataNumber Minimum;
		FPropertyMetadataNumber Maximum;
		bool bUIRange;
		int32 ExpectedMinimum;
		int32 ExpectedMaximum;
	};
	for (const FCase& Case : {
		FCase{FPropertyMetadataNumber::FromSigned(0), FPropertyMetadataNumber::FromSigned(10), false, 0, 10},
		FCase{FPropertyMetadataNumber::FromSigned(0), {}, false, 0, 105},
		FCase{{}, FPropertyMetadataNumber::FromSigned(10), false, -95, 10},
		FCase{FPropertyMetadataNumber::FromSigned(0), FPropertyMetadataNumber::FromSigned(0), false, 0, 0},
		FCase{FPropertyMetadataNumber::FromSigned(0), FPropertyMetadataNumber::FromSigned(10), true, 2, 8},
	})
	{
		FPropertyMetadataParams Metadata = OriginalParams;
		Metadata.ClampMin = Case.Minimum;
		Metadata.ClampMax = Case.Maximum;
		Metadata.UIMin = Case.bUIRange ? Original.UIMin : FPropertyMetadataNumber{};
		Metadata.UIMax = Case.bUIRange ? Original.UIMax : FPropertyMetadataNumber{};
		Reflection.Property->SetTypedMetadata(&Metadata);
		auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "NumericWidgetBounds");
		TStrongObjectPtr<DObject> ObjectRoot(Object);
		FPropertyView View;
		uint32 ErrorReports = 0;
		const FPropertyViewContext Context{
		.bShouldTransact = false,
			.ReportPropertyError = [&](std::string) { ++ErrorReports; },
		};
		ImGuiContext* ImContext = ImGui::CreateContext();
		ImGuiIO& IO = ImGui::GetIO();
		IO.ConfigInputTrickleEventQueue = false;
		IO.ConfigMacOSXBehaviors = false;
		IO.DisplaySize = {800.0f, 600.0f};
		IO.DeltaTime = 1.0f / 60.0f;
		IO.IniFilename = nullptr;
		IO.Fonts->AddFontDefault();
		IO.Fonts->Build();
		ImVec2 WidgetPosition;
		const auto DrawFrame = [&] {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({600.0f, 300.0f});
			ImGui::Begin("Numeric Widget Bounds", nullptr, ImGuiWindowFlags_NoTitleBar);
			if (MonaImGui::PropertyEdit::BeginTable("NumericRows"))
			{
				View.EditProperty(Context, Object, Reflection.Property);
				const ImVec2 Minimum = ImGui::GetItemRectMin();
				const ImVec2 Maximum = ImGui::GetItemRectMax();
				WidgetPosition = {(Minimum.x + Maximum.x) * 0.5f, (Minimum.y + Maximum.y) * 0.5f};
				MonaImGui::PropertyEdit::EndTable();
			}
			EXPECT_TRUE(View.EndInteractionFrame(Context));
			ImGui::End();
			ImGui::Render();
		};
		DrawFrame();
		DrawFrame();
		for (const float Delta : {100.0f, -100.0f})
		{
			Object->Value = Case.ExpectedMinimum == Case.ExpectedMaximum ? 0 : 5;
			IO.AddMousePosEvent(WidgetPosition.x, WidgetPosition.y);
			// Separate clicks so ImGui does not interpret the next drag as double-click text input.
			for (int Frame = 0; Frame < 25; ++Frame) DrawFrame();
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			DrawFrame();
			IO.AddMousePosEvent(WidgetPosition.x + Delta, WidgetPosition.y);
			DrawFrame();
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			DrawFrame();
			EXPECT_EQ(Object->Value, Delta > 0.0f ? Case.ExpectedMaximum : Case.ExpectedMinimum);
		}
		// Ctrl-click input may exceed the softer UI range, but never the hard bounds.
		IO.AddMousePosEvent(WidgetPosition.x, WidgetPosition.y);
		IO.AddKeyEvent(ImGuiMod_Ctrl, true);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		IO.AddKeyEvent(ImGuiMod_Ctrl, false);
		DrawFrame();
		IO.AddInputCharactersUTF8("999");
		DrawFrame();
		IO.AddKeyEvent(ImGuiKey_Enter, true);
		DrawFrame();
		EXPECT_EQ(Object->Value, Case.Maximum.Kind == EPropertyMetadataNumericKind::None ? 999 : Case.Maximum.Signed);
		EXPECT_EQ(ErrorReports, 0u);
		EXPECT_TRUE(View.FinishActiveEdit(&Context, false));
		ImGui::DestroyContext(ImContext);
	}
	Reflection.Property->SetTypedMetadata(&OriginalParams);
}

TEST(FReflectedPropertyViewTests, ColorPickerGroupsMultipleDragsAndCancelsAbandonedPreview)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto* Light = NewObject<DDirectionalLightComponent>(nullptr, "ColorPickerSession");
	TStrongObjectPtr<DObject> LightRoot(Light);
	FProperty* ColorProperty = Light->GetClass()->FindPropertyByName("Color");
	ASSERT_NE(ColorProperty, nullptr);
	const auto* Color = ColorProperty->ContainerPtrToValuePtr<FLinearColor>(Light);
	const FLinearColor Original = *Color;
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	const FPropertyViewContext Context{.ReportPropertyError = [](std::string Error) { ADD_FAILURE() << Error; }};
	const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> ImContext(ImGui::CreateContext(), ImGui::DestroyContext);
	ImGuiIO& IO = ImGui::GetIO();
	IO.ConfigInputTrickleEventQueue = false;
	IO.DisplaySize = {1200.0f, 900.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	ImGui::SetColorEditOptions(ImGuiColorEditFlags_PickerHueBar);
	ImGuiID PickerId = 0;
	const auto DrawFrame = [&](bool bOpenPicker = false, bool bDrawColor = true) {
		const FLinearColor BeforeDraw = *Color;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({600.0f, 300.0f});
		ImGui::Begin("Color Picker Session", nullptr, ImGuiWindowFlags_NoTitleBar);
		if (View.HandleOwnerContext(Context, Light) && MonaImGui::PropertyEdit::BeginTable("ColorRows"))
		{
			if (bOpenPicker)
			{
				ImGui::PushID(ColorProperty);
				ImGui::PushID(0);
				ImGui::PushID("Color");
				PickerId = ImGui::GetID("##Picker");
				ImGui::OpenPopup("##Picker");
				ImGui::PopID();
				ImGui::PopID();
				ImGui::PopID();
			}
			if (bDrawColor) View.EditProperty(Context, Light, ColorProperty);
			MonaImGui::PropertyEdit::EndTable();
		}
		EXPECT_EQ(*Color, BeforeDraw);
		EXPECT_TRUE(View.EndInteractionFrame(Context));
		ImGui::End();
		ImGui::Render();
	};
	const auto EditPicker = [&](float Offset) {
		const ImGuiWindow* Picker = ImGui::FindWindowByName(std::format("##Popup_{:08x}", PickerId).c_str());
		ASSERT_NE(Picker, nullptr);
		const ImVec2 Position = Picker->DC.CursorStartPos;
		IO.AddMousePosEvent(Position.x + Offset, Position.y + Offset);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		DrawFrame();
		IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		DrawFrame();
	};
	DrawFrame();
	DrawFrame(true);
	DrawFrame();
	EXPECT_FALSE(Transactions.Get()->IsActive());
	EditPicker(10.0f);
	const FLinearColor FirstPreview = *Color;
	EXPECT_NE(FirstPreview, Original);
	EXPECT_TRUE(Transactions.Get()->IsActive());
	EditPicker(20.0f);
	const FLinearColor FinalPreview = *Color;
	EXPECT_NE(FinalPreview, FirstPreview);
	EXPECT_TRUE(Transactions.Get()->IsActive());
	IO.AddMousePosEvent(1100.0f, 850.0f);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	DrawFrame();
	IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	DrawFrame();
	EXPECT_FALSE(Transactions.Get()->IsActive());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(*Color, Original);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(*Color, FinalPreview);
	Transactions.Get()->Reset();
	IO.AddMousePosEvent(350.0f, 60.0f);
	DrawFrame();
	DrawFrame(true);
	DrawFrame();
	EditPicker(10.0f);
	EXPECT_NE(*Color, FinalPreview);
	auto* Popup = ImGui::FindWindowByName(std::format("##Popup_{:08x}", PickerId).c_str());
	ASSERT_NE(Popup, nullptr);
	ImGui::ActivateItemByID(Popup->GetID("Cancel"));
	DrawFrame();
	EXPECT_EQ(*Color, FinalPreview);
	EXPECT_FALSE(Transactions.Get()->IsActive());
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	DrawFrame();
	DrawFrame(true);
	DrawFrame();
	EditPicker(10.0f);
	EXPECT_NE(*Color, FinalPreview);
	DrawFrame(false, false);
	DrawFrame(false, false);
	EXPECT_EQ(*Color, FinalPreview);
	EXPECT_FALSE(Transactions.Get()->IsActive());
	EXPECT_FALSE(Transactions.Get()->CanUndo());
}

TEST(FReflectedPropertyViewTests, PickerCancellationRestoresColorAndPreservesOuterUndo)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	TStrongObjectPtr<DDirectionalLightComponent> Light(NewObject<DDirectionalLightComponent>(nullptr, "NestedColorPicker"));
	const auto Color = PropertyEditor::MakePropertyHandle(*Light, "Color");
	const auto Intensity = PropertyEditor::MakePropertyHandle(*Light, "Intensity");
	FLinearColor Original;
	float BeforeIntensity = 0;
	ASSERT_EQ(Color->GetValue(Original), PropertyEditor::FPropertyAccess::Success);
	ASSERT_EQ(Intensity->GetValue(BeforeIntensity), PropertyEditor::FPropertyAccess::Success);
	FPropertyViewTestTransactorOwner History;
	{
		FScopedTransaction Outer("Edit light");
		ASSERT_EQ(Intensity->SetValue(BeforeIntensity + 1.f), PropertyEditor::FPropertyAccess::Success);
		PropertyEditor::FPropertyControl Picker;
		ASSERT_TRUE(Picker.BeginGesture(Color, true, true));
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Picker, FLinearColor(.25f, .5f, .75f, 1.f)));
		ASSERT_TRUE(Picker.Finish(true));
		EXPECT_TRUE(History->IsActive());
		FLinearColor Restored;
		ASSERT_EQ(Color->GetValue(Restored), PropertyEditor::FPropertyAccess::Success);
		EXPECT_EQ(Restored, Original);
	}
	ASSERT_TRUE(History->Undo());
	float RestoredIntensity = 0;
	ASSERT_EQ(Intensity->GetValue(RestoredIntensity), PropertyEditor::FPropertyAccess::Success);
	EXPECT_EQ(RestoredIntensity, BeforeIntensity);
	EXPECT_FALSE(History->CanUndo());
}

TEST(FReflectedPropertyViewTests, SingleValueSubmissionCreatesOneUndoRedoEntry)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "SingleValueSubmission"));
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property);

	ASSERT_TRUE(View.SubmitPropertyValueEdit({}, Target,
		[](FProperty* Property, void* Container, uint32 Index) {
			*Property->ContainerPtrToValuePtr<int32>(Container, Index) = 8;
		}, false));
	EXPECT_EQ(Object->Value, 8);
	EXPECT_EQ(Object->ChangeTypes, (std::vector{EPropertyChangeType::ValueSet}));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_FALSE(Transactions.Get()->IsActive());

	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 8);
	EXPECT_FALSE(Transactions.Get()->CanRedo());
}

TEST(FReflectedPropertyViewTests, SingleSubmissionRechecksEditabilityAfterProposal)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "LockDuringSubmission"));
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	std::string Error;
	const FPropertyViewContext Context{.ReportPropertyError = [&](std::string Message) { Error = std::move(Message); }};
	Object->bLockOnProposal = true;
	EXPECT_FALSE(View.SubmitPropertyValueEdit(Context, FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property),
		[](FProperty* Property, void* Container, uint32 Index) {
			*Property->ContainerPtrToValuePtr<int32>(Container, Index) = 8;
		}, false));
	EXPECT_EQ(Error, "The property is no longer editable.");
	EXPECT_EQ(Object->Value, 5);
	EXPECT_TRUE(Object->ChangeTypes.empty());
	EXPECT_FALSE(Transactions.Get()->CanUndo());
}

TEST(FReflectedPropertyViewTests, InvalidBoundedEditDoesNotMutateOrCreateTransaction)
{
	Durin::Testing::InitializeAssetSystemForTests();
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	auto* ManagedObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, "BoundedEdit");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(ManagedObject);
	auto& Object = *ManagedObject;
	FPropertyViewTestTransactorOwner Transactions;
	Durin::FPropertyView View;
	uint32 ErrorReports = 0;
	const Durin::FPropertyViewContext Context{
		.ReportPropertyError = [&](std::string) { ++ErrorReports; },
	};

	EXPECT_FALSE(View.SubmitPropertyValueEdit(
		Context,
		Durin::FPropertyEditTarget::ForMember(&Object, Reflection.Property),
		[](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
			*Property->ContainerPtrToValuePtr<int32>(Container, ArrayIndex) = 11;
		}, false));
	EXPECT_EQ(Object.Value, 5);
	EXPECT_EQ(ErrorReports, 1u);
	EXPECT_FALSE(Transactions.Get()->Undo());
}

TEST(FReflectedPropertyViewTests, TargetValidationOwnsRejectedPathAndBounds)
{
	using namespace Durin;
	using namespace Durin;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedPathTarget");
	TStrongObjectPtr<DObject> Root(Object);
	const auto MissingOwner = FPropertyEditTarget{}.Validate();
	ASSERT_FALSE(MissingOwner);
	EXPECT_EQ(MissingOwner.error().Code, EPropertyEditPathError::MissingOwner);
	auto Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	ASSERT_TRUE(Target.Validate());
	Target.SnapshotArrayIndex = Reflection.Property->GetArrayDim();
	const auto Bounds = Target.Validate();
	ASSERT_FALSE(Bounds);
	EXPECT_EQ(Bounds.error().Code, EPropertyEditPathError::SnapshotIndex);
	EXPECT_EQ(Bounds.error().Actual, Reflection.Property->GetArrayDim());
	EXPECT_EQ(Bounds.error().Expected, Reflection.Property->GetArrayDim());
	EXPECT_EQ(Bounds.error().Owner, FObjectKey(Object));
	EXPECT_EQ(Bounds.error().Snapshot, Reflection.Property->NamePrivate.ToString());
	Target.SnapshotArrayIndex = 0;
	Target.SnapshotContainer = nullptr;
	const auto MissingStorage = Target.Validate();
	ASSERT_FALSE(MissingStorage);
	EXPECT_EQ(MissingStorage.error().Code, EPropertyEditPathError::IncompleteTarget);
	EXPECT_FALSE(MissingStorage.error().HasSnapshotContainer);
	Target.SnapshotContainer = Object;
	Target.Path.clear();
	const auto Endpoints = Target.Validate();
	ASSERT_FALSE(Endpoints);
	EXPECT_EQ(Endpoints.error().Code, EPropertyEditPathError::Endpoints);
	Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	Target.Path.push_back({});
	Target.Path.push_back(Target.Path.front());
	const auto EmptySegment = Target.Validate();
	ASSERT_FALSE(EmptySegment);
	EXPECT_EQ(EmptySegment.error().Code, EPropertyEditPathError::EmptySegment);
	EXPECT_EQ(EmptySegment.error().PathIndex, 1u);
	Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	Target.Path.front().MapKeyData = {std::byte{0x2a}};
	const auto RejectedKey = Target.Validate();
	ASSERT_FALSE(RejectedKey);
	EXPECT_EQ(RejectedKey.error().Code, EPropertyEditPathError::UnexpectedKeyData);
	EXPECT_EQ(RejectedKey.error().PathIndex, 0u);
	EXPECT_EQ(RejectedKey.error().Actual, 1u);
	Target = {};
	ASSERT_EQ(RejectedKey.error().Path.size(), 1u);
	EXPECT_EQ(RejectedKey.error().Path.front().Property, Reflection.Property->NamePrivate.ToString());
	ASSERT_EQ(RejectedKey.error().Path.front().KeyData.size(), 1u);
	EXPECT_EQ(RejectedKey.error().Path.front().KeyData.front(), std::byte{0x2a});
	EXPECT_TRUE(Bounds.error().HasSnapshotContainer);
	EXPECT_EQ(Object->Value, 5);
}

TEST(FReflectedPropertyViewTests, DraftRetainsInitializationAndRestoreCausesAcrossRetry)
{
	using namespace Durin;
	using namespace Durin;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedDraftTarget");
	TStrongObjectPtr<DObject> Root(Object);
	auto Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	Target.SnapshotContainer = nullptr;
	FPropertyValueDraft Missing(Target);
	ASSERT_FALSE(Missing.IsValid());
	FPropertyValueSnapshotPayload Payload;
	const auto MissingCapture = Missing.Capture(Payload);
	ASSERT_FALSE(MissingCapture);
	EXPECT_EQ(MissingCapture.error().Code, EPropertyValueDraftError::MissingRoot);
	EXPECT_EQ(MissingCapture.error().Root, Reflection.Property->NamePrivate.ToString());
	EXPECT_FALSE(MissingCapture.error().HasContainer);
	Target.SnapshotContainer = Object;
	FPropertyValueDraft Draft(Target);
	ASSERT_TRUE(Draft.IsValid());
	ASSERT_TRUE(Draft.Capture(Payload));
	const auto Rejected = Draft.Restore(FPropertyValueSnapshotPayload{});
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Code, EPropertyValueDraftError::Restore);
	ASSERT_TRUE(Rejected.error().SnapshotCause.has_value());
	EXPECT_EQ(Rejected.error().SnapshotCause->Code, EPropertySnapshotError::IncompatibleType);
	EXPECT_EQ(Rejected.error().SnapshotCause->Operation, EPropertySnapshotOperation::Restore);
	EXPECT_TRUE(Draft.IsValid());
	ASSERT_TRUE(Draft.Restore(Payload));
	FPropertyValueSnapshot Snapshot;
	EXPECT_TRUE(Draft.Capture(Snapshot));
	EXPECT_EQ(Rejected.error().SnapshotCause->Code, EPropertySnapshotError::IncompatibleType);
	EXPECT_EQ(Rejected.error().Root, Reflection.Property->NamePrivate.ToString());
	EXPECT_EQ(Object->Value, 5);
}

TEST(FReflectedPropertyViewTests, SessionDiagnosticsAreOptionalAndSurviveRetry)
{
	using namespace Durin;
	Testing::InitializeAssetSystemForTests();
	Durin::PropertyEditor::FPropertyControl Session;
	EXPECT_TRUE(Session.Finish(false));

	std::string MissingError;
	EXPECT_FALSE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle({}), false, false, &MissingError));
	EXPECT_EQ(MissingError, "The edit target has no owning object.");

	auto& Reflection = GetPropertyViewHostTestReflection();
	TStrongObjectPtr<DPropertyViewHostTestObject> Object(NewObject<DPropertyViewHostTestObject>(nullptr, "TypedSessionErrors"));
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Reflection.Property);
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false));

	std::string ActiveError;
	EXPECT_FALSE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false, false, &ActiveError));
	EXPECT_EQ(ActiveError, "The property control is unavailable or already active.");

	std::string InvalidError;
	EXPECT_FALSE(Durin::Testing::ApplyControlInput(Session, FPropertyValueSnapshot{}, &InvalidError));
	EXPECT_FALSE(InvalidError.empty());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_TRUE(Session.IsActive());

	Object->Value = 6;
	FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(CapturePropertyValue(Reflection.Property, Object.Get(), 0, Proposed));
	Object->Value = 5;
	const auto Rejection = InvalidError;
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed, &InvalidError));
	EXPECT_EQ(InvalidError, Rejection);
	ASSERT_TRUE(Session.Finish(false));
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Object->Value, 6);
}
