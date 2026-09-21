#include "Editor/PropertyView.h"
#include "Editor/PropertyValueDraft.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/MathStructs.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/Transaction.h"
#include "Editor/Transactor.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "MonaImGuiPropertyTable.h"
#include "NativeTestSupport.h"
#include "NativeAssetTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	class FPropertyViewTestTransactorOwner
	{
	public:
		FPropertyViewTestTransactorOwner()
			: Transactor(Durin::NewObject<Durin::DTransBuffer>(nullptr, "PropertyViewTestTransactor"))
			, TransactorRoot(Transactor)
		{}

		auto Get() const -> Durin::DTransBuffer* { return Transactor; }

	private:
		Durin::DTransBuffer* Transactor = nullptr;
		Durin::TStrongObjectPtr<Durin::DObject> TransactorRoot;
	};

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

		auto PreEditChangeProperty(Durin::FPropertyEditProposal& Proposal) -> Durin::FObjectValidationResult override
		{
			if (Proposal.Phase == Durin::EPropertyChangePhase::Cancelled && !bAllowRestore)
			{
				return Durin::RejectPropertyEdit(*this, Proposal, Durin::EPropertyEditRejection::Rejected);
			}
			return {};
		}

		int32 Value = 5;
		Durin::FVector3f FloatVector{0.0f};
		FSoftObjectViewValue SoftValues[2];
		FSoftObjectViewArray SoftArray;
		FSoftObjectViewMap SoftMap;
		bool bAllowRestore = true;
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

			Property->Next = SoftProperty;
			SoftProperty->Next = ArrayProperty;
			ArrayProperty->Next = MapProperty;
			Class->ChildProperties = Property;
			Class->Register(Durin::DClass::StaticClass, "", "DPropertyViewHostTestObject");
			Durin::DObjectForceRegistration(Class);
		}

		Durin::DClass* Class = nullptr;
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
		Durin::Editor::FPropertyView& View,
		const Durin::Editor::FPropertyViewContext& Context,
		DPropertyViewHostTestObject& Object
	) -> bool
	{
		FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
		if (!View.HandleOwnerContext(Context, &Object)) return false;
		return View.SubmitPropertyValueEdit(
			Context,
			Durin::Editor::FPropertyEditTarget::ForMember(&Object, Reflection.Property),
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

	auto MakeSoftObjectPropertyViewPath(std::string_view Name) -> Durin::FObjectPath
	{
		EnsureSoftObjectPropertyViewMount();
		Durin::FObjectPath Path;
		EXPECT_TRUE(Durin::FObjectPath::TryCreate(
			std::format("/SoftObjectPropertyView/{}.{}", Name, Name), Path));
		return Path;
	}
}

TEST(FReflectedPropertyViewTests, HidesConventionalBoolPrefixFromDisplayName)
{
	using Durin::DurinCodeGen::EPropertyGenFlags;

	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool), "Simulate Physics");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("bUseHDR", EPropertyGenFlags::Bool), "Use HDR");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("border", EPropertyGenFlags::Bool), "border");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("b", EPropertyGenFlags::Bool), "b");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("GroundHeight", EPropertyGenFlags::Float), "Ground Height");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("URLValue", EPropertyGenFlags::String), "URL Value");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::String), "b Simulate Physics");
	EXPECT_EQ(Durin::Editor::MakePropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool, "Simulate Physics"), "Simulate Physics");
}

TEST(FReflectedPropertyViewTests, EditObjectEnumeratesEditableStaticArrayElementsBeforeSearch)
{
	using namespace Durin;
	using DurinCodeGen::EPropertyGenFlags;

	DClass TestClass(
		EC_StaticConstructor,
		FName("FReflectedPropertyViewTestObject"),
		sizeof(DObject),
		alignof(DObject),
		EObjectFlags::Transient,
		EClassFlags::Native,
		EClassCastFlags::DClass,
		nullptr
	);
	FNumericProperty EditableProperty(
		FFieldVariant(&TestClass), FName("TestValues"), EObjectFlags::Transient,
		EPropertyFlags::Edit, 3, 0, sizeof(float), EPropertyGenFlags::Float, nullptr
	);
	FNumericProperty HiddenProperty(
		FFieldVariant(&TestClass), FName("HiddenValues"), EObjectFlags::Transient,
		EPropertyFlags::None, 2, 0, sizeof(float), EPropertyGenFlags::Float, nullptr
	);
	EditableProperty.Next = &HiddenProperty;
	TestClass.ChildProperties = &EditableProperty;
	DObject Object(&TestClass, nullptr, FName("Object"));

	std::vector<uint32> FilteredIndices;
	Durin::Editor::FPropertyView View;
	const Durin::Editor::FObjectPropertyViewResult Result = View.EditObject({}, &Object, {
		.SearchText = "not present",
		.Filter = [&](const FProperty& Property, uint32 ArrayIndex) {
			EXPECT_EQ(&Property, &EditableProperty);
			FilteredIndices.push_back(ArrayIndex);
			return true;
		},
		.bCreatePropertyTable = false,
		.bShowEmptyMessage = false,
	});

	EXPECT_EQ(FilteredIndices, (std::vector<uint32>{0, 1, 2}));
	EXPECT_EQ(Result.VisiblePropertyCount, 0u);
	EXPECT_FALSE(Result.bChanged);
	EXPECT_EQ(Durin::Editor::MakePropertyLabel(EditableProperty, 2), "Test Values[2]");
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
	TestClass.ChildProperties = &Property;
	DObject Object(&TestClass, nullptr, FName("Object"));
	Editor::FPropertyView View;
	auto CollectIndices = [&]() {
		std::vector<uint32> Indices;
		View.EditObject({}, &Object, {
			.SearchText = "not present",
			.Filter = [&](const FProperty&, uint32 Index) {
				Indices.push_back(Index);
				return true;
			},
			.bCreatePropertyTable = false,
			.bShowEmptyMessage = false,
		});
		return Indices;
	};
	Property.SetMetaData(FName("ArrayIndexEnum"), "Durin::ECollisionResponse");
	EXPECT_EQ(CollectIndices(), (std::vector<uint32>{0, 1, 2}));
	EXPECT_EQ(Editor::MakePropertyLabel(Property, 1), "Overlap");
	Property.SetMetaData(FName("ArrayIndexEnum"), "Missing::Enum");
	EXPECT_EQ(CollectIndices(), (std::vector<uint32>{0, 1, 2, 3, 4, 5, 6, 7}));
	EXPECT_EQ(Editor::MakePropertyLabel(Property, 1), "Values[1]");
}

TEST(FReflectedPropertyViewTests, CategoriesCollapseAndSearchExpandsMatchingProperties)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject Object(Reflection.Class, Durin::FName("CategoryGroups"));
	ImGuiContext* ImContext = ImGui::CreateContext();
	ASSERT_NE(ImContext, nullptr);
	ImGuiIO& IO = ImGui::GetIO();
	IO.DisplaySize = {800.0f, 600.0f};
	IO.DeltaTime = 1.0f / 60.0f;
	IO.IniFilename = nullptr;
	IO.Fonts->AddFontDefault();
	IO.Fonts->Build();
	Durin::Editor::FPropertyView PropertyView;

	const auto DrawFrame = [&](std::string_view SearchText) -> int
	{
		int LastRow = -1;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0.0f, 0.0f});
		ImGui::SetNextWindowSize({600.0f, 300.0f});
		ImGui::Begin("Category Property View Test", nullptr,
			ImGuiWindowFlags_NoTitleBar);
		if (Durin::MonaImGui::PropertyEdit::BeginTable("CategoryPropertyRows"))
		{
			ImGui::GetStateStorage()->SetInt(ImGui::GetID("Numbers"), 0);
			const Durin::Editor::FObjectPropertyViewResult Result =
				PropertyView.EditObject({}, &Object, {
					.SearchText = SearchText,
					.Filter = [&Reflection](const Durin::FProperty& Property, uint32) {
						return &Property == Reflection.Property;
					},
					.bCreatePropertyTable = false,
					.bShowEmptyMessage = false,
				});
			EXPECT_EQ(Result.VisiblePropertyCount, 1u);
			LastRow = ImGui::TableGetRowIndex();
			Durin::MonaImGui::PropertyEdit::EndTable();
		}
		ImGui::End();
		ImGui::Render();
		return LastRow;
	};

	EXPECT_EQ(DrawFrame({}), 0);
	EXPECT_EQ(DrawFrame("Bounded Value"), 1);
	ImGui::DestroyContext(ImContext);
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
		EXPECT_EQ(Durin::Editor::MakePropertyLabel(*Channels, Index), Labels[Index]);
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
			Durin::Editor::FPropertyView PropertyView;
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
	Durin::CollectGarbage();
}

TEST(FReflectedPropertyViewTests, ObjectReplacementWaitsForFailedPreviewRestoration)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	(void)Reflection;
	auto* FirstObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, Durin::FName("First"));
	auto* SecondObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, Durin::FName("Second"));
	Durin::TStrongObjectPtr<DPropertyViewHostTestObject> FirstStrong(FirstObject);
	Durin::TStrongObjectPtr<DPropertyViewHostTestObject> SecondStrong(SecondObject);
	auto& First = *FirstObject;
	auto& Second = *SecondObject;
	Durin::Editor::FPropertyView View;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
	};
	First.bAllowRestore = true;
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, Context, First));
	EXPECT_TRUE(View.IsEditingObject(&First));
	EXPECT_EQ(First.Value, 8);

	First.bAllowRestore = false;
	EXPECT_FALSE(View.HandleOwnerContext(Context, &Second));
	EXPECT_TRUE(View.IsEditingObject(&First));
	EXPECT_EQ(First.Value, 8);
	EXPECT_EQ(Error, "The object rejected the reflected property proposal.");

	First.bAllowRestore = true;
	EXPECT_TRUE(View.HandleOwnerContext(Context, &Second));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(First.Value, 5);
}

TEST(FReflectedPropertyViewTests, ReadOnlyTransitionWaitsForFailedPreviewRestoration)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	(void)Reflection;
	auto* ObjectPtr = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, Durin::FName("ReadOnly"));
	Durin::TStrongObjectPtr<DPropertyViewHostTestObject> ObjectStrong(ObjectPtr);
	auto& Object = *ObjectPtr;
	Durin::Editor::FPropertyView View;
	std::string Error;
	const Durin::Editor::FPropertyViewContext EditableContext{
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
	};
	Object.bAllowRestore = true;
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, EditableContext, Object));
	EXPECT_EQ(Object.Value, 8);

	Object.bAllowRestore = false;
	Durin::Editor::FPropertyViewContext ReadOnlyContext = EditableContext;
	ReadOnlyContext.bReadOnly = true;
	EXPECT_FALSE(View.HandleOwnerContext(ReadOnlyContext, &Object));
	EXPECT_TRUE(View.IsEditingObject(&Object));
	EXPECT_EQ(Object.Value, 8);
	EXPECT_EQ(Error, "The object rejected the reflected property proposal.");

	Object.bAllowRestore = true;
	EXPECT_TRUE(View.HandleOwnerContext(ReadOnlyContext, &Object));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Object.Value, 5);
}

TEST(FReflectedPropertyViewTests, SoftObjectStateInspectionDoesNotLoadUntilRequested)
{
	EnsureSoftObjectPropertyViewMount();
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject Object(Reflection.Class, Durin::FName("SoftState"));

	auto State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Null);
	EXPECT_EQ(Durin::Editor::GetSoftObjectStateLabel(State.State), "Null");

	const Durin::FObjectPath MissingPath = MakeSoftObjectPropertyViewPath("Missing");
	Object.SoftValues[0].SetPath(MissingPath);
	State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Missing);
	const auto MissingState = State;
	EXPECT_EQ(MissingState.PropertyName, Reflection.SoftProperty->NamePrivate.ToString());
	EXPECT_EQ(MissingState.ArrayIndex, 0u);
	ASSERT_TRUE(State.AssetCause);
	EXPECT_EQ(State.Error, Durin::Editor::ESoftObjectViewError::Asset);
	EXPECT_FALSE(Object.SoftValues[0].IsLoaded());
	Durin::DObject* LoadedObject = nullptr;
	const auto Missing = Durin::Editor::LoadSoftObject(Reflection.SoftProperty, &Object, 0, LoadedObject);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Error, Durin::Editor::EPropertySoftLoadError::Asset);
	ASSERT_TRUE(Missing.AssetCause);
	EXPECT_NE(Missing.AssetCause->Error, Durin::EAssetReadError::None);
	EXPECT_EQ(Missing.Path, MissingPath);
	EXPECT_EQ(LoadedObject, nullptr);

	EXPECT_EQ(Object.SoftValues[0].GetPath(), MissingPath);

	const Durin::FObjectPath AssetSoftPath = MakeSoftObjectPropertyViewPath("Loadable");
	const Durin::FPackagePath AssetPath = AssetSoftPath.GetPackagePath();
	Durin::DObject* Asset = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AssetPath, Asset));
	ASSERT_NE(Asset, nullptr);
	ASSERT_TRUE(Durin::SavePackage(Asset->GetPackage()));
	Object.SoftValues[0].SetPath(AssetSoftPath);
	State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Loaded);
	EXPECT_EQ(MissingState.Path, MissingPath);
	ASSERT_TRUE(MissingState.AssetCause);
	EXPECT_EQ(MissingState.Error, Durin::Editor::ESoftObjectViewError::Asset);
	EXPECT_EQ(State.LoadedObject, Asset);

	Durin::FSoftObjectProperty MismatchedProperty(
		Durin::FFieldVariant(), Durin::FName("Mismatched"), Durin::EObjectFlags::Transient,
		Durin::EPropertyFlags::Edit, 2, Reflection.SoftProperty->GetOffset(),
		sizeof(FSoftObjectViewValue), Durin::DPackage::StaticClass(),
		&GetMutableSoftObjectViewValue, &GetConstSoftObjectViewValue);
	State = Durin::Editor::InspectSoftObject(&MismatchedProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::TypeMismatch);
	ASSERT_TRUE(State.AssetCause);
	EXPECT_EQ(State.Error, Durin::Editor::ESoftObjectViewError::Asset);

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	EXPECT_FALSE(Object.SoftValues[0].IsLoaded());
	State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Unloaded);
	EXPECT_EQ(State.LoadedObject, nullptr);
	EXPECT_FALSE(Object.SoftValues[0].IsLoaded());

	ASSERT_TRUE(Durin::Editor::LoadSoftObject(
		Reflection.SoftProperty, &Object, 0, LoadedObject));
	ASSERT_NE(LoadedObject, nullptr);
	EXPECT_EQ(Missing.Path, MissingPath);
	EXPECT_TRUE(Object.SoftValues[0].IsLoaded());
	State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Loaded);

	const Durin::FObjectPath AliasSoftPath = MakeSoftObjectPropertyViewPath("AliasXXX");
	const Durin::FPackagePath AliasPath = AliasSoftPath.GetPackagePath();
	Durin::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Testing::CreateAssetRedirectorForTests(AliasPath, AssetPath, Redirector));
	ASSERT_TRUE(Durin::SavePackage(Redirector->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(AliasPath));
	Object.SoftValues[0].SetPath(AliasSoftPath);
	State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Redirected);
	EXPECT_EQ(State.Path, AliasSoftPath);
	EXPECT_EQ(State.ResolvedPath, AssetSoftPath);
	EXPECT_EQ(State.LoadedObject, LoadedObject);
	EXPECT_EQ(State.Error, Durin::Editor::ESoftObjectViewError::None);
	EXPECT_FALSE(State.AssetCause);
	EXPECT_EQ(Durin::Editor::GetSoftObjectStateLabel(State.State), "Redirected");

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	State = Durin::Editor::InspectSoftObject(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::Editor::ESoftObjectViewState::Redirected);
	EXPECT_EQ(State.LoadedObject, nullptr);
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	ASSERT_TRUE(Durin::Editor::LoadSoftObject(
		Reflection.SoftProperty, &Object, 0, LoadedObject));
	EXPECT_EQ(LoadedObject->GetPackage()->GetPackagePath(), AssetPath.ToString());
	EXPECT_EQ(Object.SoftValues[0].GetPath(), AliasSoftPath);
	EXPECT_EQ(Durin::FindResidentPackage(AliasPath), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, AssetPath}));
}

TEST(FReflectedPropertyViewTests, WeakObjectInspectionDistinguishesNullLiveAndExpired)
{
	Durin::FWeakObjectProperty Property(
		Durin::FFieldVariant(), Durin::FName("Weak"), Durin::EObjectFlags::Transient,
		Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::Transient, 1, 0,
		sizeof(FWeakObjectViewValue), Durin::DObject::StaticClass(),
		&GetMutableWeakObjectViewValue, &GetConstWeakObjectViewValue);
	FWeakObjectViewValue Value;
	auto State = Durin::Editor::InspectWeakObject(&Property, &Value);
	EXPECT_EQ(State.State, Durin::Editor::EWeakObjectViewState::Null);

	Durin::DObject* Target = Durin::NewObject<Durin::DObject>(nullptr, "WeakViewTarget");
	Value = Target;
	State = Durin::Editor::InspectWeakObject(&Property, &Value);
	EXPECT_EQ(State.State, Durin::Editor::EWeakObjectViewState::Live);
	EXPECT_EQ(State.Object, Target);
	EXPECT_EQ(Durin::Editor::GetWeakObjectStateLabel(State.State), "Live");

	Durin::MarkAsGarbage(Target);
	State = Durin::Editor::InspectWeakObject(&Property, &Value);
	EXPECT_EQ(State.State, Durin::Editor::EWeakObjectViewState::Expired);
	EXPECT_EQ(State.Object, nullptr);
}

TEST(FReflectedPropertyViewTests, SoftObjectPathEditsUndoRedoFixedArrayArrayAndMapValues)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	auto* ManagedObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, "SoftTransactions");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(ManagedObject);
	auto& Object = *ManagedObject;
	const Durin::FObjectPath First = MakeSoftObjectPropertyViewPath("First");
	const Durin::FObjectPath Second = MakeSoftObjectPropertyViewPath("Second");
	const Durin::FObjectPath Third = MakeSoftObjectPropertyViewPath("Third");
	const Durin::FObjectPath Fourth = MakeSoftObjectPropertyViewPath("Fourth");
	Object.SoftValues[1].SetPath(First);
	Object.SoftArray.emplace_back(First);
	Object.SoftMap.emplace("Alpha", FSoftObjectViewValue(First));

	FPropertyViewTestTransactorOwner Transactions;
	Durin::Editor::FPropertyView View;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
	};
	auto AssignPath = [](Durin::FObjectPath Path) {
		return [Path = std::move(Path)](
			Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
			auto* Reference = static_cast<Durin::FSoftObjectProperty*>(Property)
				->GetSoftObjectPtr(Container, ArrayIndex);
			ASSERT_NE(Reference, nullptr);
			Reference->SetPath(Path);
		};
	};

	ASSERT_TRUE(View.SubmitPropertyValueEdit(
		Context,
		Durin::Editor::FPropertyEditTarget::ForMember(&Object, Reflection.SoftProperty, 1),
		AssignPath(Second), false));
	EXPECT_EQ(Object.SoftValues[1].GetPath(), Second);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object.SoftValues[1].GetPath(), First);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object.SoftValues[1].GetPath(), Second);
	ASSERT_TRUE(Transactions.Get()->Reset());

	const Durin::Editor::FPropertyEditTarget ArrayTarget =
		Durin::Editor::FPropertyEditTarget::ForMember(&Object, Reflection.ArrayProperty)
			.ForArrayElement(Reflection.ArrayInner, 0);
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, ArrayTarget, AssignPath(Third), false));
	EXPECT_EQ(Object.SoftArray[0].GetPath(), Third);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object.SoftArray[0].GetPath(), First);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object.SoftArray[0].GetPath(), Third);
	ASSERT_TRUE(Transactions.Get()->Reset());

	const std::string Alpha = "Alpha";
	Durin::FPropertyValueSnapshot KeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Reflection.MapKey, &Alpha, 0, KeySnapshot));
	const Durin::Editor::FPropertyEditTarget MapTarget =
		Durin::Editor::FPropertyEditTarget::ForMember(&Object, Reflection.MapProperty)
			.ForMapEntry(Reflection.MapValue, KeySnapshot, KeySnapshot.GetBytes());
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, MapTarget, AssignPath(Fourth), false));
	EXPECT_EQ(Object.SoftMap.at("Alpha").GetPath(), Fourth);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object.SoftMap.at("Alpha").GetPath(), First);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object.SoftMap.at("Alpha").GetPath(), Fourth);
}

TEST(FReflectedPropertyViewTests, InvalidBoundedEditDoesNotMutateOrCreateTransaction)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	auto* ManagedObject = Durin::NewObject<DPropertyViewHostTestObject>(nullptr, "BoundedEdit");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(ManagedObject);
	auto& Object = *ManagedObject;
	FPropertyViewTestTransactorOwner Transactions;
	Durin::Editor::FPropertyView View;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
	};

	EXPECT_FALSE(View.SubmitPropertyValueEdit(
		Context,
		Durin::Editor::FPropertyEditTarget::ForMember(&Object, Reflection.Property),
		[](Durin::FProperty* Property, void* Container, uint32 ArrayIndex) {
			*Property->ContainerPtrToValuePtr<int32>(Container, ArrayIndex) = 11;
		}, false));
	EXPECT_EQ(Object.Value, 5);
	EXPECT_NE(Error.find("ClampMax"), std::string::npos);
	EXPECT_FALSE(Transactions.Get()->Undo());
}

TEST(FReflectedPropertyViewTests, TypedExtensionRejectionPreservesLiveStateAndAllowsRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedExtensionEdit");
	TStrongObjectPtr<DObject> Root(Object);
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyView View;
	std::string Message;
	const FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
		.ReportError = [&](std::string Text) { Message = std::move(Text); }};
	bool Reject = true;
	uint32 PostCount = 0;
	uint32 CancelCount = 0;
	FObjectValidationError Retained;
	struct FExtensionScope
	{
		FPropertyEditExtensionHandle Handle = 0;
		~FExtensionScope() { UnregisterPropertyEditExtension(Handle); }
	} Extension;
	Extension.Handle = RegisterPropertyEditExtension({
		.PreEdit = [&](DObject& Target, FPropertyEditProposal& Proposal) -> FObjectValidationResult {
			if (&Target != Object || !Reject) return {};
			auto Result = RejectPropertyEdit(Target, Proposal, EPropertyEditRejection::IncompleteDraft);
			Retained = Result.Error;
			return Result;
		},
		.PostEdit = [&](DObject& Target, const FPropertyChangedEvent& Event) {
			if (&Target != Object) return;
			if (Event.Phase == EPropertyChangePhase::Cancelled) ++CancelCount;
			else ++PostCount;
		}});
	ASSERT_NE(Extension.Handle, 0u);
	auto Edit = [&] {
		return View.SubmitPropertyValueEdit(Context,
			FPropertyEditTarget::ForMember(Object, Reflection.Property),
			[](FProperty* Property, void* Container, uint32 Index) {
				*Property->ContainerPtrToValuePtr<int32>(Container, Index) = 6;
			}, false);
	};
	EXPECT_FALSE(Edit());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_EQ(PostCount, 0u);
	EXPECT_EQ(CancelCount, 1u);
	EXPECT_FALSE(Transactions.Get()->Undo());
	EXPECT_EQ(Retained.Code, EObjectValidationError::PropertyRejected);
	EXPECT_EQ(Retained.PropertyReason, EPropertyEditRejection::IncompleteDraft);
	EXPECT_EQ(Retained.PropertyName, Reflection.Property->NamePrivate.ToString());
	EXPECT_EQ(Retained.ObjectPath, Object->GetObjectPath());
	EXPECT_FALSE(Message.empty());
	Reject = false;
	EXPECT_TRUE(Edit());
	EXPECT_EQ(Object->Value, 6);
	EXPECT_GT(PostCount, 0u);
	EXPECT_EQ(Retained.PropertyReason, EPropertyEditRejection::IncompleteDraft);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
}

TEST(FReflectedPropertyViewTests, TypedDeferredCompletionRejectsAndIgnoresLatePublication)
{
	using namespace Durin;
	using namespace Durin::Editor;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedDeferredEdit");
	TStrongObjectPtr<DObject> Root(Object);
	Object->Value = 6;
	FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(CapturePropertyValue(Reflection.Property, Object, 0, Proposed));
	Object->Value = 5;
	FPropertyViewTestTransactorOwner Transactions;
	FPropertyEditDeferredCompletion Completion;
	uint32 CancelCount = 0;
	struct FExtensionScope
	{
		FPropertyEditExtensionHandle Handle = 0;
		~FExtensionScope() { UnregisterPropertyEditExtension(Handle); }
	} Extension;
	Extension.Handle = RegisterPropertyEditExtension({
		.PreEdit = [&](DObject& Target, FPropertyEditProposal& Proposal) -> FObjectValidationResult {
			if (&Target != Object || Proposal.Origin != EPropertyChangeOrigin::Edit
				|| Proposal.Phase != EPropertyChangePhase::Interactive) return {};
			EXPECT_TRUE(Proposal.Defer([&](FPropertyEditDeferredCompletion Done) {
				Completion = std::move(Done);
				return FPropertyEditDeferredCancel([&] { ++CancelCount; });
			}));
			return {};
		}});
	ASSERT_NE(Extension.Handle, 0u);
	FPropertyEditSession Session;
	auto Start = [&] {
		EXPECT_TRUE(Session.Begin(FPropertyEditTarget::ForMember(Object, Reflection.Property), "Deferred Test", Transactions.Get()));
		EXPECT_EQ(Session.Apply(Proposed).GetStatus(), EPropertyEditResult::Pending);
		EXPECT_TRUE(Session.HasPendingDeferredEdit());
		EXPECT_EQ(Object->Value, 5);
	};
	Start();
	ASSERT_TRUE(Completion);
	Completion({.Error = {.Code = EObjectValidationError::PropertyRejected,
		.ObjectPath = Object->GetObjectPath(), .PropertyReason = EPropertyEditRejection::Rejected,
		.PropertyName = Reflection.Property->NamePrivate.ToString()}});
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions.Get()->Undo());
	Start();
	auto LateCompletion = Completion;
	EXPECT_EQ(Session.Cancel().GetStatus(), EPropertyEditResult::NoChange);
	EXPECT_EQ(CancelCount, 1u);
	LateCompletion({});
	EXPECT_EQ(Object->Value, 5);
	EXPECT_FALSE(Transactions.Get()->Undo());
	Start();
	Completion({});
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Object->Value, 6);
	EXPECT_EQ(CancelCount, 1u);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
}

TEST(FReflectedPropertyViewTests, TargetValidationOwnsRejectedPathAndBounds)
{
	using namespace Durin;
	using namespace Durin::Editor;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedPathTarget");
	TStrongObjectPtr<DObject> Root(Object);
	EXPECT_EQ(FPropertyEditTarget{}.Validate().Error.Code, EPropertyEditPathError::MissingOwner);
	auto Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	ASSERT_TRUE(Target.Validate());
	Target.SnapshotArrayIndex = Reflection.Property->GetArrayDim();
	const auto Bounds = Target.Validate();
	EXPECT_EQ(Bounds.Error.Code, EPropertyEditPathError::SnapshotIndex);
	EXPECT_EQ(Bounds.Error.Actual, Reflection.Property->GetArrayDim());
	EXPECT_EQ(Bounds.Error.Expected, Reflection.Property->GetArrayDim());
	EXPECT_EQ(Bounds.Error.Owner, FObjectKey(Object));
	EXPECT_EQ(Bounds.Error.Snapshot, Reflection.Property->NamePrivate.ToString());
	Target.SnapshotArrayIndex = 0;
	Target.SnapshotContainer = nullptr;
	const auto MissingStorage = Target.Validate();
	EXPECT_EQ(MissingStorage.Error.Code, EPropertyEditPathError::IncompleteTarget);
	EXPECT_FALSE(MissingStorage.Error.HasSnapshotContainer);
	Target.SnapshotContainer = Object;
	Target.Path.clear();
	EXPECT_EQ(Target.Validate().Error.Code, EPropertyEditPathError::Endpoints);
	Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	Target.Path.push_back({});
	Target.Path.push_back(Target.Path.front());
	const auto EmptySegment = Target.Validate();
	EXPECT_EQ(EmptySegment.Error.Code, EPropertyEditPathError::EmptySegment);
	EXPECT_EQ(EmptySegment.Error.PathIndex, 1u);
	Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	Target.Path.front().MapKeyData = {std::byte{0x2a}};
	const auto RejectedKey = Target.Validate();
	EXPECT_EQ(RejectedKey.Error.Code, EPropertyEditPathError::UnexpectedKeyData);
	EXPECT_EQ(RejectedKey.Error.PathIndex, 0u);
	EXPECT_EQ(RejectedKey.Error.Actual, 1u);
	Target = {};
	ASSERT_EQ(RejectedKey.Error.Path.size(), 1u);
	EXPECT_EQ(RejectedKey.Error.Path.front().Property, Reflection.Property->NamePrivate.ToString());
	ASSERT_EQ(RejectedKey.Error.Path.front().KeyData.size(), 1u);
	EXPECT_EQ(RejectedKey.Error.Path.front().KeyData.front(), std::byte{0x2a});
	EXPECT_TRUE(Bounds.Error.HasSnapshotContainer);
	EXPECT_EQ(Object->Value, 5);
}

TEST(FReflectedPropertyViewTests, DraftRetainsInitializationAndRestoreCausesAcrossRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedDraftTarget");
	TStrongObjectPtr<DObject> Root(Object);
	auto Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	Target.SnapshotContainer = nullptr;
	FPropertyValueDraft Missing(Target);
	ASSERT_FALSE(Missing.IsValid());
	FPropertyValueSnapshotPayload Payload;
	const auto MissingCapture = Missing.Capture(Payload);
	EXPECT_EQ(MissingCapture.Error.Code, EPropertyValueDraftError::MissingRoot);
	EXPECT_EQ(MissingCapture.Error.Root, Reflection.Property->NamePrivate.ToString());
	EXPECT_FALSE(MissingCapture.Error.HasContainer);
	Target.SnapshotContainer = Object;
	FPropertyValueDraft Draft(Target);
	ASSERT_TRUE(Draft.IsValid());
	ASSERT_TRUE(Draft.Capture(Payload));
	const auto Rejected = Draft.Restore(FPropertyValueSnapshotPayload{});
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.Error.Code, EPropertyValueDraftError::Restore);
	ASSERT_TRUE(Rejected.Error.SnapshotCause.has_value());
	EXPECT_EQ(Rejected.Error.SnapshotCause->Code, EPropertySnapshotError::IncompatibleType);
	EXPECT_EQ(Rejected.Error.SnapshotCause->Operation, EPropertySnapshotOperation::Restore);
	EXPECT_TRUE(Draft.IsValid());
	ASSERT_TRUE(Draft.Restore(Payload));
	FPropertyValueSnapshot Snapshot;
	EXPECT_TRUE(Draft.Capture(Snapshot));
	EXPECT_EQ(Rejected.Error.SnapshotCause->Code, EPropertySnapshotError::IncompatibleType);
	EXPECT_EQ(Rejected.Error.Root, Reflection.Property->NamePrivate.ToString());
	EXPECT_EQ(Object->Value, 5);
}

TEST(FReflectedPropertyViewTests, TransactionRecordRetainsMutationCausesAcrossRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedObjectRecord");
	TStrongObjectPtr<DObject> Root(Object);
	const auto Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	FPropertyValueSnapshotPayload Before, After;
	ASSERT_TRUE(CapturePropertyValuePayload(Reflection.Property, Object, 0, Before));
	Object->Value = 6;
	ASSERT_TRUE(CapturePropertyValuePayload(Reflection.Property, Object, 0, After));
	Object->Value = 5;
	FTransactionObjectRecord Record;
	ASSERT_TRUE(FTransactionObjectRecord::Capture(Target, Before, After, Record));
	ASSERT_TRUE(Record.Validate());
	const auto Invalid = FTransactionObjectRecord::Capture(Target, {}, After, Record);
	EXPECT_EQ(Invalid.Error.Code, ETransactionObjectRecordError::Payload);
	EXPECT_EQ(Invalid.Error.Owner, FObjectKey(Object));
	EXPECT_FALSE(Invalid.Error.BeforeValid);
	EXPECT_TRUE(Invalid.Error.AfterValid);
	EXPECT_EQ(Record.GetBefore(), Before);
	bool Reject = true;
	struct FExtensionScope
	{
		FPropertyEditExtensionHandle Handle = 0;
		~FExtensionScope() { UnregisterPropertyEditExtension(Handle); }
	} Extension;
	Extension.Handle = RegisterPropertyEditExtension({
		.PreEdit = [&](DObject& Owner, FPropertyEditProposal& Proposal) -> FObjectValidationResult {
			if (&Owner != Object || !Reject) return {};
			return RejectPropertyEdit(Owner, Proposal, EPropertyEditRejection::IncompleteDraft);
		}});
	const auto Rejected = Record.Apply(false, EPropertyChangeOrigin::Redo);
	EXPECT_EQ(Rejected.Error.Code, ETransactionObjectRecordError::Mutation);
	ASSERT_TRUE(Rejected.Error.MutationCause);
	EXPECT_EQ(Rejected.Error.MutationCause->Code, EPropertyMutationError::ExtensionValidation);
	EXPECT_EQ(Rejected.Error.MutationCause->Origin, EPropertyChangeOrigin::Redo);
	ASSERT_TRUE(Rejected.Error.MutationCause->ValidationCause);
	EXPECT_EQ(Rejected.Error.MutationCause->ValidationCause->PropertyReason, EPropertyEditRejection::IncompleteDraft);
	EXPECT_EQ(Object->Value, 5);
	FTransaction History(42, {.Description = "Typed property history"});
	History.AddRecord(Record);
	const auto HistoryFailure = History.Apply(false, EPropertyChangeOrigin::Redo);
	EXPECT_EQ(HistoryFailure.Error.Code, ETransactionApplyError::Execution);
	EXPECT_EQ(HistoryFailure.Error.RecordIndex, 0u);
	EXPECT_EQ(HistoryFailure.Error.RecordCause.Code, ETransactionRecordError::Object);
	ASSERT_TRUE(HistoryFailure.Error.RecordCause.ObjectCause);
	ASSERT_TRUE(HistoryFailure.Error.RecordCause.ObjectCause->MutationCause);
	EXPECT_EQ(HistoryFailure.Error.RecordCause.ObjectCause->MutationCause->Code, EPropertyMutationError::ExtensionValidation);
	EXPECT_EQ(Object->Value, 5);
	Reject = false;
	ASSERT_TRUE(Record.Apply(false, EPropertyChangeOrigin::Redo));
	EXPECT_EQ(Object->Value, 6);
	ASSERT_TRUE(Record.Apply(true, EPropertyChangeOrigin::Undo));
	EXPECT_EQ(Object->Value, 5);
	EXPECT_EQ(Rejected.Error.MutationCause->ValidationCause->PropertyReason, EPropertyEditRejection::IncompleteDraft);
	EXPECT_EQ(Invalid.Error.Code, ETransactionObjectRecordError::Payload);
}

TEST(FReflectedPropertyViewTests, SessionMessagesSurviveResetAndRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	FPropertyEditSession Session;
	EXPECT_EQ(Session.Commit().GetStatus(), EPropertyEditResult::Failed);
	const auto Missing = Session.Begin({}, "Missing target");
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.GetStatus(), EPropertyEditResult::Failed);
	EXPECT_EQ(Missing.Message, "The edit target has no owning object.");
	auto& Reflection = GetPropertyViewHostTestReflection();
	auto* Object = NewObject<DPropertyViewHostTestObject>(nullptr, "TypedSessionErrors");
	TStrongObjectPtr<DObject> Root(Object);
	const auto Target = FPropertyEditTarget::ForMember(Object, Reflection.Property);
	ASSERT_TRUE(Session.Begin(Target, "Owned session description"));
	const auto Active = Session.Begin(Target, "Replacement description");
	EXPECT_FALSE(Active);
	EXPECT_EQ(Active.Message, "A reflected-property edit session is already active.");
	const auto Invalid = Session.Apply(FPropertyValueSnapshotPayload{});
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.GetStatus(), EPropertyEditResult::Failed);
	EXPECT_FALSE(Invalid.Message.empty());
	const auto InvalidMessage = Invalid.Message;
	EXPECT_EQ(Object->Value, 5);
	EXPECT_TRUE(Session.IsActive());
	Object->Value = 6;
	FPropertyValueSnapshotPayload Proposed;
	ASSERT_TRUE(CapturePropertyValuePayload(Reflection.Property, Object, 0, Proposed));
	Object->Value = 5;
	const auto Applied = Session.Apply(Proposed);
	ASSERT_TRUE(Applied);
	EXPECT_EQ(Applied.GetStatus(), EPropertyEditResult::Changed);
	EXPECT_TRUE(Applied.Message.empty());
	ASSERT_TRUE(Session.Commit());
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Object->Value, 6);
	EXPECT_EQ(Active.Message, "A reflected-property edit session is already active.");
	EXPECT_EQ(Invalid.Message, InvalidMessage);
	EXPECT_EQ(Missing.Message, "The edit target has no owning object.");
	// Presentation text does not determine the command outcome.
	auto WithoutMessage = Invalid;
	WithoutMessage.Message.clear();
	EXPECT_FALSE(WithoutMessage);
	EXPECT_EQ(WithoutMessage.GetStatus(), EPropertyEditResult::Failed);
}
