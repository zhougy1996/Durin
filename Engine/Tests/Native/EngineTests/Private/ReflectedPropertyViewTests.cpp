#include "Editor/ReflectedPropertyView.h"

#include "AssetRedirector.h"
#include "AssetSystem.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"
#include "Editor/EditorTransaction.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using FSoftObjectViewValue = Durin::TSoftObjectPtr<Durin::DObject>;
	using FSoftObjectViewArray = std::vector<FSoftObjectViewValue>;
	using FSoftObjectViewMap = std::unordered_map<std::string, FSoftObjectViewValue>;

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

	class DPropertyViewHostTestObject final : public Durin::DObject
	{
	public:
		explicit DPropertyViewHostTestObject(Durin::DClass* Class, Durin::FName Name)
			: DObject(Class, nullptr, std::move(Name))
		{
		}

		auto PreEditChangeProperty(Durin::FPropertyEditProposal& Proposal, std::string& OutError) -> bool override
		{
			if (Proposal.Phase == Durin::EPropertyChangePhase::Cancelled && !bAllowRestore)
			{
				OutError = "Host transition restore rejected for testing.";
				return false;
			}
			return true;
		}

		Durin::int32 Value = 5;
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
				nullptr
			);
			DPropertyViewHostTestObject OffsetProbe(Class, Durin::FName("OffsetProbe"));
			const auto Offset = static_cast<Durin::uint16>(
				reinterpret_cast<const Durin::uint8*>(&OffsetProbe.Value)
				- reinterpret_cast<const Durin::uint8*>(&OffsetProbe)
			);
			Property = new Durin::FNumericProperty(
				Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 1, Offset, sizeof(Durin::int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
			);
			Property->SetValueLifecycle(sizeof(Durin::int32), alignof(Durin::int32),
				[](void* Memory) { std::construct_at(static_cast<Durin::int32*>(Memory)); },
				[](void* Memory) { std::destroy_at(static_cast<Durin::int32*>(Memory)); });

			const auto SoftValuesOffset = static_cast<Durin::uint16>(
				reinterpret_cast<const Durin::uint8*>(&OffsetProbe.SoftValues)
				- reinterpret_cast<const Durin::uint8*>(&OffsetProbe));
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
			const auto SoftArrayOffset = static_cast<Durin::uint16>(
				reinterpret_cast<const Durin::uint8*>(&OffsetProbe.SoftArray)
				- reinterpret_cast<const Durin::uint8*>(&OffsetProbe));
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
			const auto SoftMapOffset = static_cast<Durin::uint16>(
				reinterpret_cast<const Durin::uint8*>(&OffsetProbe.SoftMap)
				- reinterpret_cast<const Durin::uint8*>(&OffsetProbe));
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

		auto BeginPropertyViewHostPreview(
		Durin::FReflectedPropertyView& View,
		const Durin::FReflectedPropertyViewContext& Context,
		DPropertyViewHostTestObject& Object
	) -> bool
	{
		FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
		if (!View.HandleOwnerContext(Context, &Object)) return false;
		return View.SubmitPropertyValueEdit(
			Context,
			Durin::FReflectedPropertyEditTarget::ForMember(&Object, Reflection.Property),
			[](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*static_cast<Durin::int32*>(Property->GetValuePtr(Container, ArrayIndex)) = 8;
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
			Durin::PathUtilities::RegisterMountPointForTests(
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
			std::format("/SoftObjectPropertyView/{}", Name), Path));
		return Path;
	}
}

TEST(FReflectedPropertyViewTests, HidesConventionalBoolPrefixFromDisplayName)
{
	using Durin::DurinCodeGen::EPropertyGenFlags;

	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool), "Simulate Physics");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bUseHDR", EPropertyGenFlags::Bool), "Use HDR");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("border", EPropertyGenFlags::Bool), "border");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("b", EPropertyGenFlags::Bool), "b");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("GroundHeight", EPropertyGenFlags::Float), "Ground Height");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("URLValue", EPropertyGenFlags::String), "URL Value");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::String), "b Simulate Physics");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool, "Simulate Physics"), "Simulate Physics");
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
	FReflectedPropertyView View;
	const FObjectPropertyViewResult Result = View.EditObject({}, &Object, {
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
	EXPECT_EQ(MakeReflectedPropertyLabel(EditableProperty, 2), "Test Values[2]");
}

TEST(FReflectedPropertyViewTests, ObjectReplacementWaitsForFailedPreviewRestoration)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject First(Reflection.Class, Durin::FName("First"));
	DPropertyViewHostTestObject Second(Reflection.Class, Durin::FName("Second"));
	Durin::FReflectedPropertyView View;
	std::string Error;
	const Durin::FReflectedPropertyViewContext Context{
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
	EXPECT_EQ(Error, "Host transition restore rejected for testing.");

	First.bAllowRestore = true;
	EXPECT_TRUE(View.HandleOwnerContext(Context, &Second));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(First.Value, 5);
}

TEST(FReflectedPropertyViewTests, ReadOnlyTransitionWaitsForFailedPreviewRestoration)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject Object(Reflection.Class, Durin::FName("ReadOnly"));
	Durin::FReflectedPropertyView View;
	std::string Error;
	const Durin::FReflectedPropertyViewContext EditableContext{
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
	};
	Object.bAllowRestore = true;
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, EditableContext, Object));
	EXPECT_EQ(Object.Value, 8);

	Object.bAllowRestore = false;
	Durin::FReflectedPropertyViewContext ReadOnlyContext = EditableContext;
	ReadOnlyContext.bReadOnly = true;
	EXPECT_FALSE(View.HandleOwnerContext(ReadOnlyContext, &Object));
	EXPECT_TRUE(View.IsEditingObject(&Object));
	EXPECT_EQ(Object.Value, 8);
	EXPECT_EQ(Error, "Host transition restore rejected for testing.");

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

	auto State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Null);
	EXPECT_EQ(Durin::GetSoftObjectPropertyStateLabel(State.State), "Null");

	const Durin::FSoftObjectPath MissingPath = MakeSoftObjectPropertyViewPath("Missing");
	Object.SoftValues[0].SetPath(MissingPath);
	State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Missing);
	EXPECT_FALSE(State.Message.empty());
	EXPECT_FALSE(Object.SoftValues[0].IsLoaded());
	Durin::DObject* LoadedObject = nullptr;
	std::string Error;
	EXPECT_FALSE(Durin::LoadSoftObjectProperty(
		Reflection.SoftProperty, &Object, 0, LoadedObject, &Error));
	EXPECT_EQ(LoadedObject, nullptr);
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Object.SoftValues[0].GetSoftObjectPath(), MissingPath);

	const Durin::FSoftObjectPath AssetSoftPath = MakeSoftObjectPropertyViewPath("Loadable");
	const Durin::FAssetPath AssetPath = AssetSoftPath.GetAssetPath();
	Durin::DObject* Asset = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(AssetPath, Asset));
	ASSERT_NE(Asset, nullptr);
	ASSERT_TRUE(Durin::Asset::SavePackage(Asset->GetPackage()));
	Object.SoftValues[0].SetPath(AssetSoftPath);
	State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Loaded);
	EXPECT_EQ(State.LoadedObject, Asset);

	Durin::FSoftObjectProperty MismatchedProperty(
		Durin::FFieldVariant(), Durin::FName("Mismatched"), Durin::EObjectFlags::Transient,
		Durin::EPropertyFlags::Edit, 2, Reflection.SoftProperty->GetOffset(),
		sizeof(FSoftObjectViewValue), Durin::DPackage::StaticClass(),
		&GetMutableSoftObjectViewValue, &GetConstSoftObjectViewValue);
	State = Durin::InspectSoftObjectProperty(&MismatchedProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::TypeMismatch);
	EXPECT_FALSE(State.Message.empty());

	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	EXPECT_FALSE(Object.SoftValues[0].IsLoaded());
	State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Unloaded);
	EXPECT_EQ(State.LoadedObject, nullptr);
	EXPECT_FALSE(Object.SoftValues[0].IsLoaded());

	Error.clear();
	ASSERT_TRUE(Durin::LoadSoftObjectProperty(
		Reflection.SoftProperty, &Object, 0, LoadedObject, &Error)) << Error;
	ASSERT_NE(LoadedObject, nullptr);
	EXPECT_TRUE(Object.SoftValues[0].IsLoaded());
	State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Loaded);

	const Durin::FSoftObjectPath AliasSoftPath = MakeSoftObjectPropertyViewPath("AliasXXX");
	const Durin::FAssetPath AliasPath = AliasSoftPath.GetAssetPath();
	Durin::Asset::DAssetRedirector* Redirector = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAssetRedirector(AliasPath, AssetPath, Redirector));
	ASSERT_TRUE(Durin::Asset::SavePackage(Redirector->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AliasPath));
	Object.SoftValues[0].SetPath(AliasSoftPath);
	State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Redirected);
	EXPECT_EQ(State.Path, AliasPath);
	EXPECT_EQ(State.ResolvedPath, AssetPath);
	EXPECT_EQ(State.LoadedObject, LoadedObject);
	EXPECT_FALSE(State.Message.empty());
	EXPECT_EQ(Durin::GetSoftObjectPropertyStateLabel(State.State), "Redirected");

	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	State = Durin::InspectSoftObjectProperty(Reflection.SoftProperty, &Object, 0);
	EXPECT_EQ(State.State, Durin::ESoftObjectPropertyViewState::Redirected);
	EXPECT_EQ(State.LoadedObject, nullptr);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	Error.clear();
	ASSERT_TRUE(Durin::LoadSoftObjectProperty(
		Reflection.SoftProperty, &Object, 0, LoadedObject, &Error)) << Error;
	EXPECT_EQ(LoadedObject->GetPackage()->GetPackagePath(), AssetPath.ToString());
	EXPECT_EQ(Object.SoftValues[0].GetSoftObjectPath(), AliasSoftPath);
	EXPECT_EQ(Durin::Asset::FindLoadedPackage(AliasPath), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(DeleteAssetClosureForTest({AliasPath, AssetPath}));
}

TEST(FReflectedPropertyViewTests, SoftObjectPathEditsUndoRedoFixedArrayArrayAndMapValues)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject Object(Reflection.Class, Durin::FName("SoftTransactions"));
	const Durin::FSoftObjectPath First = MakeSoftObjectPropertyViewPath("First");
	const Durin::FSoftObjectPath Second = MakeSoftObjectPropertyViewPath("Second");
	const Durin::FSoftObjectPath Third = MakeSoftObjectPropertyViewPath("Third");
	const Durin::FSoftObjectPath Fourth = MakeSoftObjectPropertyViewPath("Fourth");
	Object.SoftValues[1].SetPath(First);
	Object.SoftArray.emplace_back(First);
	Object.SoftMap.emplace("Alpha", FSoftObjectViewValue(First));

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView View;
	const Durin::FReflectedPropertyViewContext Context{.Transactions = &Transactions};
	auto AssignPath = [](Durin::FSoftObjectPath Path) {
		return [Path = std::move(Path)](
			Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
			auto* Reference = static_cast<Durin::FSoftObjectProperty*>(Property)
				->GetSoftObjectPtr(Container, ArrayIndex);
			ASSERT_NE(Reference, nullptr);
			Reference->SetPath(Path);
		};
	};

	ASSERT_TRUE(View.SubmitPropertyValueEdit(
		Context,
		Durin::FReflectedPropertyEditTarget::ForMember(&Object, Reflection.SoftProperty, 1),
		AssignPath(Second), false));
	EXPECT_EQ(Object.SoftValues[1].GetSoftObjectPath(), Second);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Object.SoftValues[1].GetSoftObjectPath(), First);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Object.SoftValues[1].GetSoftObjectPath(), Second);
	Transactions.Clear();

	const Durin::FReflectedPropertyEditTarget ArrayTarget =
		Durin::FReflectedPropertyEditTarget::ForMember(&Object, Reflection.ArrayProperty)
			.ForArrayElement(Reflection.ArrayInner, 0);
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, ArrayTarget, AssignPath(Third), false));
	EXPECT_EQ(Object.SoftArray[0].GetSoftObjectPath(), Third);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Object.SoftArray[0].GetSoftObjectPath(), First);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Object.SoftArray[0].GetSoftObjectPath(), Third);
	Transactions.Clear();

	const std::string Alpha = "Alpha";
	Durin::FPropertyValueSnapshot KeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Reflection.MapKey, &Alpha, 0, KeySnapshot));
	const Durin::FReflectedPropertyEditTarget MapTarget =
		Durin::FReflectedPropertyEditTarget::ForMember(&Object, Reflection.MapProperty)
			.ForMapEntry(Reflection.MapValue, KeySnapshot, KeySnapshot.GetBytes());
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, MapTarget, AssignPath(Fourth), false));
	EXPECT_EQ(Object.SoftMap.at("Alpha").GetSoftObjectPath(), Fourth);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Object.SoftMap.at("Alpha").GetSoftObjectPath(), First);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Object.SoftMap.at("Alpha").GetSoftObjectPath(), Fourth);
}
