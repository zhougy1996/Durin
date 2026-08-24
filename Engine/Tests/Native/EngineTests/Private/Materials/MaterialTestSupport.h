#pragma once

#include "StaticMeshTestAccess.h"

#include "AssetTools.h"
#include "AssetPackageV5Codec.h"
#include "Asset/PackageObjectStreamReader.h"
#include "Asset/PackageTrailer.h"
#include "Asset/AssetRetention.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/Archive.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "Engine/Actor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Editor/PropertyEditing.h"
#include "Editor/PropertyView.h"
#include "DObject/Class.h"
#include "StaticMeshMaterialSlotDetails.h"
#include "Workspace/LevelEditorContext.h"
#include "Materials/Material.h"
#include "Materials/DefaultMaterialService.h"
#include "Materials/MaterialInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Scene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialPreview.h"

#include <gtest/gtest.h>

namespace
{
	constexpr uint8 MaterialTexturePngBytes[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
		0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
		0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

	auto WriteMaterialTextureFixture(const std::filesystem::path& Path) -> void
	{
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(MaterialTexturePngBytes), sizeof(MaterialTexturePngBytes));
	}

	auto RewriteSerializedFieldAsLegacyMap(
		std::vector<std::byte>& Bytes,
		std::string_view CurrentName,
		std::string_view LegacyName
	) -> bool
	{
		Durin::Asset::PackageTrailer::FInspection Trailer;
		if (!Durin::Asset::PackageTrailer::Inspect(Bytes, Trailer)
			|| Trailer.ObjectStreamEnd > Bytes.size()) return false;
		std::vector<std::byte> ObjectStream(
			Bytes.begin(), Bytes.begin() + static_cast<size_t>(Trailer.ObjectStreamEnd));
		Durin::Asset::PackageObjectStream::FDecodedPackage Package;
		if (!Durin::Asset::PackageObjectStream::DecodePackage(ObjectStream, Package)) return false;

		size_t SchemaIndex = std::string::npos;
		size_t FieldIndex = std::string::npos;
		for (size_t CandidateSchemaIndex = 0;
			CandidateSchemaIndex < Package.Schemas.size();
			++CandidateSchemaIndex)
		{
			auto& Fields = Package.Schemas[CandidateSchemaIndex].Fields;
			for (size_t CandidateFieldIndex = 0;
				CandidateFieldIndex < Fields.size();
				++CandidateFieldIndex)
			{
				if (Fields[CandidateFieldIndex].Name != CurrentName) continue;
				if (SchemaIndex != std::string::npos) return false;
				SchemaIndex = CandidateSchemaIndex;
				FieldIndex = CandidateFieldIndex;
			}
		}
		if (SchemaIndex == std::string::npos) return false;

		using Durin::Asset::PackageObjectStream::ETypeOpcode;
		const uint64 KeyTypeId = Package.Types.size() + 1;
		Package.Types.push_back({.Opcode = ETypeOpcode::String});
		const uint64 ValueTypeId = Package.Types.size() + 1;
		Package.Types.push_back({.Opcode = ETypeOpcode::String});
		const uint64 MapTypeId = Package.Types.size() + 1;
		Package.Types.push_back({
			.Opcode = ETypeOpcode::Map,
			.ChildTypeIds = {KeyTypeId, ValueTypeId}});
		auto& Field = Package.Schemas[SchemaIndex].Fields[FieldIndex];
		Field.Name = LegacyName;
		Field.TypeId = MapTypeId;

		size_t RewrittenOverrides = 0;
		for (auto& ObjectValues : Package.ObjectValues)
		{
			for (auto& Override : ObjectValues.Overrides)
			{
				if (Override.SchemaId != SchemaIndex + 1
					|| Override.FieldId != FieldIndex + 1) continue;
				if (Override.Provenance == 2) return false;
				Override.Value = {};
				++RewrittenOverrides;
			}
		}
		if (RewrittenOverrides == 0
			|| !Durin::Asset::PackageObjectStream::ReencodePackage(Package, ObjectStream))
			return false;
		return static_cast<bool>(
			Durin::Asset::Private::DastV5::BuildPackageFromObjectStream(
				ObjectStream, Bytes));
	}

	auto ContainsSerializedField(std::span<const std::byte> Bytes, std::string_view Name) -> bool
	{
		Durin::Asset::PackageTrailer::FInspection Trailer;
		if (!Durin::Asset::PackageTrailer::Inspect(Bytes, Trailer)
			|| Trailer.ObjectStreamEnd > Bytes.size()) return false;
		Durin::Asset::PackageObjectStream::FDecodedPackage Package;
		if (!Durin::Asset::PackageObjectStream::DecodePackage(
			Bytes.first(static_cast<size_t>(Trailer.ObjectStreamEnd)), Package)) return false;
		for (const auto& Schema : Package.Schemas)
		{
			if (std::ranges::any_of(
				Schema.Fields,
				[Name](const auto& Field) { return Field.Name == Name; })) return true;
		}
		return false;
	}

	auto RemoveSerializedField(
		std::vector<std::byte>& Bytes,
		std::string_view Name) -> bool
	{
		Durin::Asset::PackageTrailer::FInspection Trailer;
		if (!Durin::Asset::PackageTrailer::Inspect(Bytes, Trailer)
			|| Trailer.ObjectStreamEnd > Bytes.size()) return false;
		std::vector<std::byte> ObjectStream(
			Bytes.begin(), Bytes.begin()
				+ static_cast<size_t>(Trailer.ObjectStreamEnd));
		Durin::Asset::PackageObjectStream::FDecodedPackage Package;
		if (!Durin::Asset::PackageObjectStream::DecodePackage(
			ObjectStream, Package)) return false;

		size_t SchemaIndex = std::string::npos;
		size_t FieldIndex = std::string::npos;
		for (size_t CandidateSchema = 0;
			CandidateSchema < Package.Schemas.size(); ++CandidateSchema)
		{
			for (size_t CandidateField = 0;
				CandidateField < Package.Schemas[CandidateSchema].Fields.size();
				++CandidateField)
			{
				if (Package.Schemas[CandidateSchema]
					.Fields[CandidateField].Name != Name) continue;
				if (SchemaIndex != std::string::npos) return false;
				SchemaIndex = CandidateSchema;
				FieldIndex = CandidateField;
			}
		}
		if (SchemaIndex == std::string::npos) return false;

		const uint64 SchemaId = SchemaIndex + 1;
		const uint64 FieldId = FieldIndex + 1;
		Package.Schemas[SchemaIndex].Fields.erase(
			Package.Schemas[SchemaIndex].Fields.begin() + FieldIndex);
		for (auto& ObjectValues : Package.ObjectValues)
		{
			std::erase_if(ObjectValues.Overrides, [&](const auto& Override) {
				return Override.SchemaId == SchemaId
					&& Override.FieldId == FieldId;
			});
			for (auto& Override : ObjectValues.Overrides)
				if (Override.SchemaId == SchemaId
					&& Override.FieldId > FieldId)
					--Override.FieldId;
		}
		if (!Durin::Asset::PackageObjectStream::ReencodePackage(
			Package, ObjectStream)) return false;
		return static_cast<bool>(
			Durin::Asset::Private::DastV5::BuildPackageFromObjectStream(
				ObjectStream, Bytes));
	}

	auto ReplaceAll(std::string& Text, std::string_view From, std::string_view To) -> void
	{
		size_t Offset = 0;
		while ((Offset = Text.find(From, Offset)) != std::string::npos)
		{
			Text.replace(Offset, From.size(), To);
			Offset += To.size();
		}
	}

		auto WriteStaticMeshSlotVariant(
		const std::filesystem::path& Path,
		std::string_view MaterialDeclarations,
		std::optional<std::pair<std::string_view, std::string_view>> PrimitiveReplacement = std::nullopt,
		bool bReplaceLastOnly = false,
		std::optional<uint32> AppendedMaterialIndex = std::nullopt,
		bool bSwapPrimitiveMaterialIndices = false) -> void
	{
		std::ifstream Input(std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf");
		ASSERT_TRUE(Input.is_open());
		std::string Text((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
		const size_t MaterialsBegin = Text.find("\"materials\": [");
		const size_t MaterialsEnd = Text.find("],", MaterialsBegin);
		ASSERT_NE(MaterialsBegin, std::string::npos);
		ASSERT_NE(MaterialsEnd, std::string::npos);
		const size_t ContentBegin = Text.find('[', MaterialsBegin) + 1;
		Text.replace(ContentBegin, MaterialsEnd - ContentBegin, MaterialDeclarations);
		if (PrimitiveReplacement)
		{
			if (bReplaceLastOnly)
			{
				const size_t Offset = Text.rfind(PrimitiveReplacement->first);
				ASSERT_NE(Offset, std::string::npos);
				Text.replace(Offset, PrimitiveReplacement->first.size(), PrimitiveReplacement->second);
			}
			else
			{
				ReplaceAll(Text, PrimitiveReplacement->first, PrimitiveReplacement->second);
			}
		}
		if (AppendedMaterialIndex)
		{
			const size_t PrimitiveBegin = Text.find("{ \"attributes\"");
			const size_t PrimitiveEnd = Text.find('\n', PrimitiveBegin);
			ASSERT_NE(PrimitiveBegin, std::string::npos);
			ASSERT_NE(PrimitiveEnd, std::string::npos);
			std::string Primitive = Text.substr(PrimitiveBegin, PrimitiveEnd - PrimitiveBegin);
			if (!Primitive.empty() && Primitive.back() == ',') Primitive.pop_back();
			const size_t MaterialOffset = Primitive.rfind("\"material\": ");
			ASSERT_NE(MaterialOffset, std::string::npos);
			const size_t ValueOffset = MaterialOffset + std::string_view("\"material\": ").size();
			Primitive.replace(ValueOffset, 1, std::to_string(*AppendedMaterialIndex));
			const size_t PrimitivesEnd = Text.find("\n      ]", PrimitiveEnd);
			ASSERT_NE(PrimitivesEnd, std::string::npos);
			Text.insert(PrimitivesEnd, ",\n        " + Primitive);
		}
		if (bSwapPrimitiveMaterialIndices)
		{
			ReplaceAll(Text, R"("material": 0)", R"("material": 2)");
			ReplaceAll(Text, R"("material": 1)", R"("material": 0)");
			ReplaceAll(Text, R"("material": 2)", R"("material": 1)");
			const size_t FirstBegin = Text.find("        { \"attributes\"");
			const size_t FirstEnd = Text.find('\n', FirstBegin);
			const size_t SecondBegin = Text.find("        { \"attributes\"", FirstEnd);
			const size_t SecondEnd = Text.find('\n', SecondBegin);
			ASSERT_NE(FirstBegin, std::string::npos);
			ASSERT_NE(FirstEnd, std::string::npos);
			ASSERT_NE(SecondBegin, std::string::npos);
			ASSERT_NE(SecondEnd, std::string::npos);
			std::string First = Text.substr(FirstBegin, FirstEnd - FirstBegin);
			std::string Second = Text.substr(SecondBegin, SecondEnd - SecondBegin);
			if (!First.empty() && First.back() == ',') First.pop_back();
			if (!Second.empty() && Second.back() == ',') Second.pop_back();
			Text.replace(FirstBegin, SecondEnd - FirstBegin, Second + ",\n" + First);
		}
		std::ofstream Output(Path, std::ios::trunc);
		ASSERT_TRUE(Output.is_open());
		Output << Text;
		ASSERT_TRUE(Output.good());
	}

	class FMaterialTestEngine final : public Durin::DEngine
	{
	public:
		FMaterialTestEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto CreateTestScene() -> Durin::FScene*
		{
			Durin::FRendererModule SceneFactory;
			MainScene = SceneFactory.CreateScene();
			auto* Result = static_cast<Durin::FScene*>(MainScene.get());
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
		auto SetTestRendererModule(Durin::IRendererModule* InRendererModule) -> void { RendererModule = InRendererModule; }
	};

	auto WaitForRenderingThread() -> void
	{
		Durin::FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}

	auto GetMaterialBinding(
		const Durin::FMaterialRenderData& RenderData)
		-> Durin::FMaterialRenderBinding
	{
		Durin::FMaterialRenderBinding Binding;
		Durin::FMaterialRenderValidationDiagnostic Diagnostic;
		EXPECT_TRUE(Durin::TryGetMaterialRenderBinding(
			RenderData.Representation, Binding, Diagnostic))
			<< Diagnostic.Message;
		return Binding;
	}

	struct FSceneSnapshot
	{
		Durin::FStaticMeshSceneProxy* Proxy = nullptr;
		Durin::FMaterialRenderData Material;
		Durin::FMatrix Transform{1.0};
		uint64 ComponentRevision = 0;
		uint64 ProxyCount = 0;
	};

	struct FMaterialSlotsSnapshot
	{
		Durin::FStaticMeshSceneProxy* Proxy = nullptr;
		const Durin::FStaticMeshRenderData* RenderData = nullptr;
		std::vector<Durin::FMaterialRenderData> Materials;
		std::vector<const Durin::FMaterialRenderProxy*> MaterialProxies;
		uint64 ComponentRevision = 0;
	};

	auto CaptureScene(Durin::FScene* Scene) -> FSceneSnapshot
	{
		FSceneSnapshot Snapshot;
		struct FCaptureMaterialTestSceneCommand
		{
			static constexpr const char* GetName() { return "CaptureMaterialTestScene"; }
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialTestSceneCommand>([Scene, &Snapshot](Durin::FRHICommandListImmediate& CommandList) {
			Snapshot.ProxyCount = Scene->GetPrimitiveSceneInfos().size();
			if (Scene->GetStaticMeshSceneInfos().empty()) return;
			const Durin::FPrimitiveSceneInfo* Info = Scene->GetStaticMeshSceneInfos().front();
			Snapshot.Proxy = &Info->GetStaticMeshProxy();
			if (Snapshot.Proxy == nullptr) return;
			Snapshot.Material =
				Snapshot.Proxy->ResolveMaterialRenderData_RenderThread();
			Snapshot.Transform = Info->GetTransform();
			Snapshot.ComponentRevision = Snapshot.Proxy->GetMaterialComponentRevision();
		});
		WaitForRenderingThread();
		return Snapshot;
	}

	auto CaptureMaterialSlots(Durin::FScene* Scene) -> FMaterialSlotsSnapshot
	{
		FMaterialSlotsSnapshot Snapshot;
		struct FCaptureMaterialSlotsCommand
		{
			static constexpr const char* GetName() { return "CaptureMaterialSlots"; }
		};
		Durin::EnqueueRenderCommand<FCaptureMaterialSlotsCommand>([Scene, &Snapshot](Durin::FRHICommandListImmediate&) {
			if (Scene->GetStaticMeshSceneInfos().empty()) return;
			Snapshot.Proxy = &Scene->GetStaticMeshSceneInfos().front()->GetStaticMeshProxy();
			if (Snapshot.Proxy == nullptr) return;
			Snapshot.RenderData = Snapshot.Proxy->GetRenderData();
			Snapshot.ComponentRevision = Snapshot.Proxy->GetMaterialComponentRevision();
			for (uint32 SlotIndex = 0; SlotIndex < Snapshot.Proxy->GetNumMaterials(); ++SlotIndex)
			{
				Snapshot.Materials.push_back(
					Snapshot.Proxy->ResolveMaterialRenderData_RenderThread(
						SlotIndex));
				Snapshot.MaterialProxies.push_back(
					Snapshot.Proxy->GetMaterialRenderProxy(SlotIndex).GetReference());
			}
		});
		WaitForRenderingThread();
		return Snapshot;
	}

	class FRenderSceneHarness
	{
	public:
		FRenderSceneHarness()
		{
			InitializeDObjectSystem();
			bOwnsRenderingThread =
				Durin::GetRenderCommandAdmissionState()
					== Durin::ERenderCommandAdmissionState::Stopped;
			if (bOwnsRenderingThread) Durin::InitRenderingThread();
			if (Durin::PathUtilities::FindMountForVirtualPath(
					Durin::DefaultMaterialAssetPath))
			{
				Durin::InitializeDefaultMaterialService();
			}
			Scene = Engine.CreateTestScene();
			Durin::GEngine = &Engine;
			World = Durin::NewObject<Durin::DWorld>(&Engine, "MaterialTestWorld");
			Durin::AddToRoot(World.Get());
			World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World.Get(), "MaterialTestLevel"));
			Engine.SetWorld(World.Get());
		}

		~FRenderSceneHarness() { Shutdown(); }

		auto Shutdown() -> void
		{
			if (!bActive) return;
			Engine.SetWorld(nullptr);
			if (Scene != nullptr)
			{
				Engine.ResetTestScene();
				WaitForRenderingThread();
				Scene = nullptr;
			}
			if (World)
			{
				Durin::RemoveFromRoot(World.Get());
				Durin::MarkObjectHierarchyAsGarbage(World.Get());
				World = nullptr;
			}
			Durin::GEngine = nullptr;
			Durin::ShutdownDefaultMaterialService();
			if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
			bActive = false;
		}

		auto CreateStaticMeshComponent(Durin::FName Name)
			-> Durin::DStaticMeshComponent*
		{
			Durin::AActor* Actor = World->SpawnActor<Durin::AActor>(
				Durin::FName(std::format("{}Owner", Name.ToString())));
			return Actor
				? Durin::Cast<Durin::DStaticMeshComponent>(Actor->AddInstanceComponent(
					Durin::DStaticMeshComponent::StaticClass(), Name))
				: nullptr;
		}

		FMaterialTestEngine Engine;
		Durin::FScene* Scene = nullptr;
		Durin::TObjectPtr<Durin::DWorld> World;
		bool bActive = true;
		bool bOwnsRenderingThread = false;
	};

	class FMaterialPreviewHarness
	{
	public:
		FMaterialPreviewHarness()
			: RendererLifecycle("MaterialPreviewRendererTest")
		{
			InitializeDObjectSystem();
			bOwnsRenderingThread =
				Durin::GetRenderCommandAdmissionState()
					== Durin::ERenderCommandAdmissionState::Stopped;
			if (bOwnsRenderingThread) Durin::InitRenderingThread();
			if (Durin::PathUtilities::FindMountForVirtualPath(
					Durin::DefaultMaterialAssetPath))
			{
				Durin::InitializeDefaultMaterialService();
			}
			RendererLifecycle.Start(RendererModule);
			Engine.SetTestRendererModule(&RendererModule);
			Durin::GEngine = &Engine;
		}

		~FMaterialPreviewHarness()
		{
			Durin::GEngine = nullptr;
			RendererLifecycle.Shutdown();
			WaitForRenderingThread();
			Durin::ShutdownDefaultMaterialService();
			if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
		}

		FMaterialTestEngine Engine;
		Durin::FRendererModule RendererModule;
		Durin::FModuleTestHarness RendererLifecycle;
		bool bOwnsRenderingThread = false;
	};

	auto ExpectColorNear(const Durin::FVector4f& Actual, const Durin::FVector4f& Expected) -> void
	{
		EXPECT_NEAR(Actual.r, Expected.r, 1.e-6f);
		EXPECT_NEAR(Actual.g, Expected.g, 1.e-6f);
		EXPECT_NEAR(Actual.b, Expected.b, 1.e-6f);
		EXPECT_NEAR(Actual.a, Expected.a, 1.e-6f);
	}

	auto FindObjectByName(std::string_view Name) -> Durin::DObject*
	{
		const auto Objects = Durin::GDObjectArray.GetAll(Durin::EObjectQueryScope::LiveOnly);
		const auto It = std::ranges::find_if(Objects, [Name](const Durin::DObject* Object) {
			return Object && Object->GetName() == Name;
		});
		return It == Objects.end() ? nullptr : *It;
	}

	auto AddDebugMaterialSlot(Durin::DStaticMesh* Mesh, std::string_view Name) -> void
	{
		auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
		EXPECT_NE(Slots, nullptr);
		const uint64 Index = Slots->Num(Mesh);
		Slots->Resize(Mesh, Index + 1);
		auto* Slot = static_cast<Durin::FMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, Index));
		Slot->Name = Durin::FName(Name);
		Slot->SourceName = std::string(Name);
		Slot->SourceMaterialIndex = static_cast<uint32>(Index);
		Durin::FStaticMeshTestAccess::GetMutableRenderData(Mesh)
			->MaterialSlots.push_back(
				{std::string(Name),
					static_cast<uint32>(Index)});
	}

	auto MakeMaterialValueTarget(Durin::DMaterial* Material, const Durin::FGuid& Id, Durin::FName FieldName)
		-> std::optional<Durin::Editor::FPropertyEditTarget>
	{
		Durin::FProperty* DefinitionsProperty = Material->GetClass()->FindPropertyByName("ParameterDefinitions");
		if (!DefinitionsProperty || DefinitionsProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Array) return std::nullopt;
		auto* Definitions = static_cast<Durin::FArrayProperty*>(DefinitionsProperty);
		if (!Definitions->GetInner()
			|| Definitions->GetInner()->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return std::nullopt;
		auto* DefinitionProperty = static_cast<Durin::FStructProperty*>(Definitions->GetInner());
		Durin::FProperty* ValueProperty = DefinitionProperty->GetStruct()->FindPropertyByName("Value");
		if (!ValueProperty || ValueProperty->GetKind() != Durin::DurinCodeGen::EPropertyGenFlags::Struct) return std::nullopt;
		auto* ValueStructProperty = static_cast<Durin::FStructProperty*>(ValueProperty);
		Durin::FProperty* Field = ValueStructProperty->GetStruct()->FindPropertyByName(FieldName);
		if (!Field) return std::nullopt;
		const std::span DefinitionsView = Material->GetParameterDefinitions();
		const auto It = std::ranges::find(DefinitionsView, Id, &Durin::FMaterialParameterDefinition::Id);
		if (It == DefinitionsView.end()) return std::nullopt;
		const uint64 Index = static_cast<uint64>(It - DefinitionsView.begin());
		void* Definition = Definitions->GetMutableElementPtr(Material, Index);
		void* Value = ValueProperty->GetValuePtr(Definition);
		return Durin::Editor::FPropertyEditTarget::ForMember(Material, Definitions)
			.ForArrayElement(Definitions->GetInner(), Index)
			.ForStructMember(ValueProperty)
			.ForStructMember(Field);
	}
}
