#pragma once

#include "StaticMeshTestAccess.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/AssetRetention.h"
#include "Asset/AssetCompilingManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/Archive.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/PackageFormat.h"
#include "DObject/ObjectLifecycle.h"
#include "EngineTestSupport.h"
#include "SceneInfo.h"
#include "Rendering/StaticMeshSceneProxy.h"
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
#include "Misc/MountPaths.h"
#include "Modules/ModuleTestSupport.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "SceneTestAccess.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2D.h"
#include "Widgets/MaterialPreview.h"

#include <gtest/gtest.h>

namespace
{
	auto FinishMaterialCompileForTest(
		Durin::DMaterial& Material,
		std::chrono::milliseconds Timeout = std::chrono::seconds(10)) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
			const auto State = Material.GetMaterialCompileStatus().State;
			if (State != Durin::EMaterialCompileState::Pending
				&& State != Durin::EMaterialCompileState::Running)
				return State == Durin::EMaterialCompileState::Ready;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

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
		Durin::FByteBuffer& Bytes,
		const Durin::FPackagePath& PackagePath,
		std::string_view CurrentName,
		std::string_view LegacyName
	) -> bool
	{
		Durin::ObjectPackage::FLinkerTables Linker;
		if (!Durin::ObjectPackage::ReadPackageV9(
			Bytes, {}, PackagePath, Linker)) return false;
		Durin::ObjectPackage::FSerializedSchema* MatchedSchema = nullptr;
		Durin::ObjectPackage::FSerializedField* MatchedField = nullptr;
		for (auto& Schema : Linker.Schemas)
		{
			for (auto& Field : Schema.Fields)
			{
				if (Field.Name != CurrentName) continue;
				if (MatchedField) return false;
				MatchedSchema = &Schema;
				MatchedField = &Field;
			}
		}
		if (!MatchedSchema || !MatchedField) return false;
		Durin::ObjectPackage::FSerializedType StringType{
			.Kind = Durin::ObjectPackage::EValueKind::String};
		Durin::ObjectPackage::FSerializedType MapType{
			.Kind = Durin::ObjectPackage::EValueKind::Map,
			.Children = {StringType, StringType}};
		MatchedField->Name = LegacyName;
		MatchedField->Type = MapType;
		size_t Rewritten = 0;
		for (auto& Export : Linker.Exports)
		{
			for (auto& Property : Export.Properties)
			{
				if (Property.DeclaringType != MatchedSchema->QualifiedName
					|| Property.FieldName != CurrentName) continue;
				Property.FieldName = LegacyName;
				Property.Type = MapType;
				Property.Value = {};
				++Rewritten;
			}
		}
		if (Rewritten == 0) return false;
		Durin::FByteBuffer Main;
		Durin::FByteBuffer Bulk;
		if (!Durin::ObjectPackage::WritePackageV9(Linker, Main, Bulk) || !Bulk.empty())
			return false;
		Bytes = std::move(Main);
		return true;
	}

	auto ContainsSerializedField(Durin::FByteView Bytes,
		const Durin::FPackagePath& PackagePath, std::string_view Name) -> bool
	{
		Durin::ObjectPackage::FLinkerTables Linker;
		if (!Durin::ObjectPackage::ReadPackageV9(
			Bytes, {}, PackagePath, Linker)) return false;
		for (const auto& Schema : Linker.Schemas)
		{
			if (std::ranges::any_of(
				Schema.Fields,
				[Name](const auto& Field) { return Field.Name == Name; })) return true;
		}
		return false;
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

		auto ResetTestScene() -> void { Durin::FSceneInterfaceTestAccess::ReleaseScene(MainScene); }
		auto SetTestRendererModule(Durin::IRendererModule* InRendererModule) -> void { RendererModule = InRendererModule; }
	};

	auto WaitForRenderingThread() -> void
	{
		Durin::FRenderCommandFence Fence;
		Fence.BeginFence();
		Fence.Wait();
	}

	using namespace Durin;
	// Test-only projection of familiar PBR names from generic compiled fields.
	struct FMaterialTestBinding : Durin::FMaterialRenderBinding
	{
		FVector4f BaseColor{0.5f, 0.5f, 0.5f, 1.0f};
		FVector3f Emissive{0.0f};
		FVector3f Normal{0.0f, 0.0f, 1.0f};
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float AmbientOcclusion = 1.0f;
		float OpacityMask = 1.0f;
		std::array<float, 8> UVChannels{};
		std::array<FVector2f, 8> UVScales{};
		std::array<FVector2f, 8> UVOffsets{};
		std::array<FRHITextureReferenceRef, 8> Textures{};
		std::array<float, 8> UVRotations{};
		std::array<FMaterialSamplerState, 8> Samplers{};

		FMaterialTestBinding()
		{
			UVScales.fill(FVector2f(1.0f, 1.0f));
		}
	};

	auto GetMaterialBinding(
		const Durin::FMaterialRenderData& RenderData)
		-> FMaterialTestBinding
	{
		FMaterialTestBinding Binding;
		Durin::FMaterialRenderValidationDiagnostic Diagnostic;
		EXPECT_TRUE(Durin::TryGetMaterialRenderBinding(
			RenderData.Representation, Binding, Diagnostic))
			<< Diagnostic.Message;
		if (Binding.LayoutIdentity.Version
			== Durin::CompiledMaterialRenderLayoutVersion)
		{
			const auto Payload = RenderData.Representation.GetUniformPayload();
			const auto Resources = RenderData.Representation.GetResources();
			auto Read = [&](const Durin::FGuid& Id, uint32 Component = 0) {
				const auto& Fields = RenderData.Representation.GetLayout().Fields;
				const auto It = std::ranges::find(
					Fields, Id, &Durin::FMaterialRenderField::ParameterId);
				float Value = 0.0f;
				if (It != Fields.end()
					&& It->Storage == Durin::EMaterialRenderFieldStorage::Uniform
					&& It->Offset + (Component + 1) * sizeof(float) <= Payload.size())
					std::memcpy(&Value, Payload.data() + It->Offset
						+ Component * sizeof(float), sizeof(Value));
				return Value;
			};
			using Role = Durin::MaterialParameters::EMaterialBuiltinParameterRole;
			using Kind = Durin::MaterialParameters::EMaterialBuiltinParameterKind;
			const auto ValueId = [](Role R) {
				return Durin::MaterialParameters::GetBuiltinParameterId(R, Kind::Value);
			};
			const auto& Fields = RenderData.Representation.GetLayout().Fields;
			const auto Has = [&](const Durin::FGuid& Id) {
				return std::ranges::find(Fields, Id,
					&Durin::FMaterialRenderField::ParameterId) != Fields.end();
			};
			if (Has(ValueId(Role::BaseColor)))
				Binding.BaseColor = {Read(ValueId(Role::BaseColor), 0),
					Read(ValueId(Role::BaseColor), 1), Read(ValueId(Role::BaseColor), 2),
					Has(ValueId(Role::Opacity)) ? Read(ValueId(Role::Opacity)) : Binding.BaseColor.a};
			if (Has(ValueId(Role::Normal))) Binding.Normal = {Read(ValueId(Role::Normal), 0),
				Read(ValueId(Role::Normal), 1), Read(ValueId(Role::Normal), 2)};
			if (Has(ValueId(Role::Metallic))) Binding.Metallic = Read(ValueId(Role::Metallic));
			if (Has(ValueId(Role::Roughness))) Binding.Roughness = Read(ValueId(Role::Roughness));
			if (Has(ValueId(Role::AmbientOcclusion))) Binding.AmbientOcclusion = Read(ValueId(Role::AmbientOcclusion));
			if (Has(ValueId(Role::Emissive))) Binding.Emissive = {Read(ValueId(Role::Emissive), 0),
				Read(ValueId(Role::Emissive), 1), Read(ValueId(Role::Emissive), 2)};
			if (Has(ValueId(Role::OpacityMask))) Binding.OpacityMask = Read(ValueId(Role::OpacityMask));
			for (size_t RoleIndex = 0; RoleIndex < 8; ++RoleIndex)
			{
				const Role R = static_cast<Role>(RoleIndex);
				const auto ChannelId = Durin::MaterialParameters::GetBuiltinParameterId(R, Kind::UVChannel);
				const auto ScaleId = Durin::MaterialParameters::GetBuiltinParameterId(R, Kind::UVScale);
				const auto OffsetId = Durin::MaterialParameters::GetBuiltinParameterId(R, Kind::UVOffset);
				const auto RotationId = Durin::MaterialParameters::GetBuiltinParameterId(R, Kind::UVRotation);
				if (Has(ChannelId)) Binding.UVChannels[RoleIndex] = Read(ChannelId);
				if (Has(ScaleId)) Binding.UVScales[RoleIndex] = {Read(ScaleId, 0), Read(ScaleId, 1)};
				if (Has(OffsetId)) Binding.UVOffsets[RoleIndex] = {Read(OffsetId, 0), Read(OffsetId, 1)};
				if (Has(RotationId)) Binding.UVRotations[RoleIndex] = Read(RotationId);
				const auto TextureId = Durin::MaterialParameters::GetBuiltinParameterId(R, Kind::Texture);
				const auto Field = std::ranges::find(Fields, TextureId,
					&Durin::FMaterialRenderField::ParameterId);
				if (Field != Fields.end() && Field->CompactIndex < Resources.size())
				{
					Binding.Textures[RoleIndex] = Resources[Field->CompactIndex];
					Binding.Samplers[RoleIndex] = Binding.CompiledSamplers[Field->CompactIndex];
				}
			}
		}
		// Error color is shader-owned; it has no declared uniform field.
		if (RenderData.Representation.IsError())
			Binding.BaseColor = Durin::FVector4f(1.0f, 0.0f, 1.0f, 1.0f);
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
			if (Scene->GetPrimitiveSceneInfos().empty()) return;
			const Durin::FPrimitiveSceneInfo* Info = Scene->GetPrimitiveSceneInfos().front();
			Snapshot.Proxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(&Info->GetProxy());
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
			if (Scene->GetPrimitiveSceneInfos().empty()) return;
			Snapshot.Proxy = dynamic_cast<Durin::FStaticMeshSceneProxy*>(&Scene->GetPrimitiveSceneInfos().front()->GetProxy());
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
			if (Durin::FMountPaths::FindMountForVirtualPath(
					Durin::DefaultMaterialPackagePath))
			{
				Durin::InitializeDefaultMaterialService();
			}
			Scene = Engine.CreateTestScene();
			Durin::GEngine = &Engine;
			World = Durin::NewObject<Durin::DWorld>(&Engine, "MaterialTestWorld");
			EXPECT_TRUE(World->InitializeSubsystems());
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
			if (Durin::FMountPaths::FindMountForVirtualPath(
					Durin::DefaultMaterialPackagePath))
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

namespace
{
	struct FDualLayerRustFixture
	{
		std::vector<Durin::FMaterialParameterDefinition> Definitions;
		Durin::FMaterialProgram Program;
	};

	inline auto MakeDualLayerRustFixture() -> FDualLayerRustFixture
	{
		using namespace Durin;
		auto Numeric = [](const char* Name, EMaterialParameterType Type,
			FMaterialParameterValue Value) {
			FMaterialParameterDefinition Result;
			Result.Id = FGuid::NewGuid(); Result.Name = FName(Name);
			Result.DisplayName = Name; Result.Type = Type; Result.Value = Value;
			return Result;
		};
		auto Texture = [](const char* Name, EMaterialTextureFallback Fallback) {
			FMaterialParameterDefinition Result;
			Result.Id = FGuid::NewGuid(); Result.Name = FName(Name);
			Result.DisplayName = Name; Result.Type = EMaterialParameterType::Texture;
			Result.Value = FMaterialParameterValue::MakeTexture(nullptr, {}, Fallback);
			return Result;
		};
		std::vector<FMaterialParameterDefinition> Definitions{
			Texture("MetalColorTexture", EMaterialTextureFallback::White),
			Texture("MetalNormalTexture", EMaterialTextureFallback::FlatRGNormal),
			Texture("RustColorTexture", EMaterialTextureFallback::White),
			Texture("RustNormalTexture", EMaterialTextureFallback::FlatRGNormal),
			Texture("RustMaskTexture", EMaterialTextureFallback::Black),
			Numeric("RustAmount", EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(0.35f)),
			Numeric("RustTiling", EMaterialParameterType::Scalar,
				FMaterialParameterValue::MakeScalar(2.0f))};
		const FGuid RustAmountId = Definitions[5].Id;
		FMaterialProgram Program;
		auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
			std::vector<FMaterialProgramLink> Inputs = {}, FGuid ParameterId = {}) {
			FMaterialProgramNode Node;
			Node.Id = FGuid::NewGuid(); Node.Opcode = Opcode; Node.ResultType = Type;
			Node.Inputs = std::move(Inputs); Node.ParameterId = ParameterId;
			Program.Nodes.push_back(Node);
			return FMaterialProgramLink{Node.Id, 0};
		};
		auto Swizzle = [&](FMaterialProgramLink Input,
			EMaterialProgramValueType Type, std::initializer_list<uint8> Channels) {
			const auto Result = Add(EMaterialProgramOpcode::Swizzle, Type, {Input});
			auto& Node = Program.Nodes.back(); Node.SwizzleLength = Channels.size();
			auto It = Channels.begin();
			if (It != Channels.end()) Node.SwizzleX = *It++;
			if (It != Channels.end()) Node.SwizzleY = *It++;
			if (It != Channels.end()) Node.SwizzleZ = *It++;
			if (It != Channels.end()) Node.SwizzleW = *It;
			return Result;
		};
		const auto Channel = Add(EMaterialProgramOpcode::Constant,
			EMaterialProgramValueType::Float);
		const auto UV = Add(EMaterialProgramOpcode::UVChannel,
			EMaterialProgramValueType::Float2, {Channel});
		const auto Tiling = Add(EMaterialProgramOpcode::Parameter,
			EMaterialProgramValueType::Float, {}, Definitions[6].Id);
		const auto Tiling2 = Add(EMaterialProgramOpcode::Splat2,
			EMaterialProgramValueType::Float2, {Tiling});
		const auto ScaledUV = Add(EMaterialProgramOpcode::Multiply,
			EMaterialProgramValueType::Float2, {UV, Tiling2});
		auto Sample = [&](size_t DefinitionIndex) {
			const auto Parameter = Add(EMaterialProgramOpcode::TextureParameter,
				EMaterialProgramValueType::Texture2D, {}, Definitions[DefinitionIndex].Id);
			return Add(EMaterialProgramOpcode::TextureSample2D,
				EMaterialProgramValueType::Float4, {Parameter, ScaledUV});
		};
		const auto MetalColor = Swizzle(Sample(0), EMaterialProgramValueType::Float3, {0, 1, 2});
		const auto MetalNormalRG = Swizzle(Sample(1), EMaterialProgramValueType::Float2, {0, 1});
		const auto RustColor = Swizzle(Sample(2), EMaterialProgramValueType::Float3, {0, 1, 2});
		const auto RustNormalRG = Swizzle(Sample(3), EMaterialProgramValueType::Float2, {0, 1});
		const auto RustMask = Swizzle(Sample(4), EMaterialProgramValueType::Float, {0});
		const auto Amount = Add(EMaterialProgramOpcode::Parameter,
			EMaterialProgramValueType::Float, {}, RustAmountId);
		const auto WeightedMask = Add(EMaterialProgramOpcode::Multiply,
			EMaterialProgramValueType::Float, {RustMask, Amount});
		const auto Alpha = Add(EMaterialProgramOpcode::Saturate,
			EMaterialProgramValueType::Float, {WeightedMask});
		Program.Outputs.BaseColor = Add(EMaterialProgramOpcode::Lerp,
			EMaterialProgramValueType::Float3, {MetalColor, RustColor, Alpha});
		const auto MetalNormal = Add(EMaterialProgramOpcode::DecodeNormalRG,
			EMaterialProgramValueType::Float3, {MetalNormalRG});
		const auto RustNormal = Add(EMaterialProgramOpcode::DecodeNormalRG,
			EMaterialProgramValueType::Float3, {RustNormalRG});
		Program.Outputs.Normal = Add(EMaterialProgramOpcode::Lerp,
			EMaterialProgramValueType::Float3, {MetalNormal, RustNormal, Alpha});
		return {std::move(Definitions), std::move(Program)};
	}
}
