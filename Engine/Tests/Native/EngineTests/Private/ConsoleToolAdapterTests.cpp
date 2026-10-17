#include "Panels/ConsoleToolAdapter.h"
#include "Panels/ConsoleToolArguments.h"
#include "Tools/NativeToolset.h"
#include "Editor/EditorEngine.h"
#include "Modules/ModuleManager.h"
#include "Editor/LevelEditorSubsystem.h"
#include "DObject/Class.h"
#include "DObject/FunctionParameters.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Transactions/Transactor.h"
#include "Json/Json.h"

#include <gtest/gtest.h>

namespace Durin
{
	class FConsoleTestEditor : public DEditorEngine
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FConsoleTestEditor, DEditorEngine, FConsoleTestEditor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FConsoleTestEditor)
	public:
		explicit FConsoleTestEditor(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Start() -> FSubsystemResult { return InitializeEditorSubsystems(); }
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FConsoleTestEditor)
	struct FEditorEngineTestAccess
	{
		static auto SetState(DEditorEngine& Editor, Editor::EPlayState State) -> void { Editor.PlayState = State; }
		static auto SetEditorWorld(DEditorEngine& Editor, DWorld* World) -> void { Editor.EditorWorld = World; Editor.SetWorld(World); }
	};
}

namespace Durin::Editor::MainFrame
{
	namespace
	{
		auto EchoDescriptor() -> FToolDescriptor
		{
			FToolDescriptor Descriptor{.Name = "echo"};
			Descriptor.Input.Properties.emplace("text", FToolSchema(EToolValueType::String));
			Descriptor.Input.Properties.at("text").Default = "default";
			Descriptor.Output = Descriptor.Input;
			return Descriptor;
		}
		auto ResultField(const FConsoleCommandResult& Result, std::string_view Name) -> std::string
		{
			FJsonDocument Document;
			if (auto Parsed = Document.Parse(Result.Message); !Parsed) { ADD_FAILURE() << Result.Message; return {}; }
			return Document.GetRootView().GetView(Name).GetString();
		}
		class FConsoleToolTests : public testing::Test
		{
		protected:
			auto SetUp() -> void override
			{
				Testing::InitializeDObjectSystemForTests();
				ASSERT_TRUE(FModuleManager::Get().LoadModule("LevelEditor"));
				Host = NewHost();
				Adapter = std::make_unique<FConsoleToolAdapter>(*Host, Legacy);
				Temp = Testing::CreateTestFixtureDirectory("ConsoleTools");
			}
			auto TearDown() -> void override
			{
				Adapter.reset(); Registration.Reset(); Toolset.reset();
				for (auto& Editor : Editors) { Editor->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Editor.Get()); }
				Editors.clear(); CollectGarbage();
				Testing::RemoveTestWorkDirectory(Temp);
			}
			auto NewHost() -> FConsoleTestEditor*
			{
				auto* Editor = NewObject<FConsoleTestEditor>(nullptr, {});
				Editors.emplace_back(Editor); EXPECT_TRUE(Editor->Start()); return Editor;
			}
			auto Register(FToolDescriptor Descriptor = EchoDescriptor()) -> void
			{
				FToolBinding Binding{std::move(Descriptor)};
				Binding.Handler = [&](const auto& Request, const auto& Context) {
					++Calls; LastArguments = Request.Arguments; LastSource = Context.Source;
					if (Busy) return FToolResult{EToolStatus::Failed, {}, {{EToolError::Busy, "Domain busy", {}}}};
					return FToolResult{EToolStatus::Succeeded, Request.Arguments};
				};
				Toolset = std::make_unique<FNativeToolset>("tests", "Console test tools", std::vector<FToolBinding>{std::move(Binding)});
				Registration = FModularFeatureRegistry::Get().RegisterFeature(*Toolset);
			}
			auto File(std::string_view Text) -> std::string
			{
				auto Path = Temp / "arguments with spaces.json";
				std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
				Stream.write(Text.data(), static_cast<std::streamsize>(Text.size())); Stream.close();
				return "\"" + Path.string() + "\"";
			}
			auto Tools() -> DEditorToolSubsystem* { return Host->GetEditorSubsystem<DEditorToolSubsystem>(); }
			FConsoleCommandRegistry Legacy;
			std::unique_ptr<FConsoleToolAdapter> Adapter;
			std::unique_ptr<FNativeToolset> Toolset;
			FModularFeatureRegistration Registration;
			std::vector<TStrongObjectPtr<FConsoleTestEditor>> Editors;
			FConsoleTestEditor* Host = nullptr;
			std::filesystem::path Temp;
			FToolValue LastArguments;
			EToolSource LastSource = EToolSource::Native;
			int Calls = 0;
			bool Busy = false;
		};
	}

	TEST_F(FConsoleToolTests, ParsesTypedScalarsDefaultsAndPathsWithoutLegacyEscaping)
	{
		auto Descriptor = EchoDescriptor();
		Descriptor.Input.Properties.emplace("enabled", FToolSchema(EToolValueType::Boolean));
		Descriptor.Input.Properties.emplace("count", FToolSchema(EToolValueType::Integer));
		Descriptor.Input.Properties.emplace("id", FToolSchema(EToolValueType::UnsignedInteger));
		Descriptor.Input.Properties.emplace("ratio", FToolSchema(EToolValueType::Number));
		Descriptor.Output = Descriptor.Input; Register(Descriptor);
		auto Result = Adapter->Execute(R"(tests/echo --enabled true --count -9 --id 18446744073709551615 --ratio 1.25 --text 'C:\Temp\a b.txt')");
		ASSERT_TRUE(Result.bSuccess) << Result.Message;
		const auto& Object = std::get<FToolValue::FObject>(LastArguments.Data);
		EXPECT_EQ(FToolValue(uint64{18446744073709551615ull}), Object.at("id"));
		EXPECT_EQ(FToolValue(R"(C:\Temp\a b.txt)"), Object.at("text"));
		EXPECT_EQ(EToolSource::Console, LastSource);
		FJsonDocument Output; ASSERT_TRUE(Output.Parse(Result.Message));
		EXPECT_EQ(uint64{18446744073709551615ull}, Output.GetRootView().GetView("id").GetUInt());
		EXPECT_TRUE(Adapter->Execute("tests/echo").bSuccess);
		EXPECT_EQ(FToolValue("default"), std::get<FToolValue::FObject>(LastArguments.Data).at("text"));
	}

	TEST_F(FConsoleToolTests, RejectsInvalidOptionsEnumsNumbersAndInlineContainersWithoutExecution)
	{
		auto Descriptor = EchoDescriptor();
		Descriptor.Input.Properties.at("text").Enum = {"a", "b"}; Descriptor.Input.Properties.at("text").Default = "a";
		Descriptor.Input.Properties.emplace("enabled", FToolSchema(EToolValueType::Boolean));
		Descriptor.Input.Properties.emplace("count", FToolSchema(EToolValueType::Integer));
		Descriptor.Input.Properties.emplace("id", FToolSchema(EToolValueType::UnsignedInteger));
		Descriptor.Input.Properties.emplace("ratio", FToolSchema(EToolValueType::Number));
		FToolSchema Array(EToolValueType::Array); Array.Items = std::make_unique<FToolSchema>(EToolValueType::Integer);
		Descriptor.Input.Properties.emplace("items", std::move(Array)); Descriptor.Output = Descriptor.Input; Register(Descriptor);
		for (const auto* Input : {"--enabled yes", "--count 2x", "--count 9223372036854775808", "--id -1", "--ratio nan", "--ratio inf",
			"--unknown 1", "--count 1 --count 2", "--text c", "--items [1]", "--text", "--text 'unterminated"})
			EXPECT_FALSE(Adapter->Execute(std::string("tests/echo ") + Input).bSuccess) << Input;
		EXPECT_FALSE(Adapter->Execute("TESTS/ECHO").bSuccess);
		EXPECT_FALSE(Adapter->Execute(R"(tests/echo\)").bSuccess);
		EXPECT_FALSE(Adapter->Execute(R"(tests\/echo)").bSuccess);
		EXPECT_EQ(0, Calls);
	}

	TEST_F(FConsoleToolTests, StructuredQuotesEscapeOnlyTheirDelimiterAndBackslash)
	{
		auto Tokens = TokenizeToolText(R"(tests/echo --text "a\"b\\c" --other 'd\'e\\f')");
		ASSERT_TRUE(Tokens);
		EXPECT_EQ((std::vector<std::string>{"tests/echo", "--text", R"(a"b\c)", "--other", R"(d'e\f)"}), *Tokens);
		Tokens = TokenizeToolText("tests/echo --text \"\" trailing\\"); ASSERT_TRUE(Tokens);
		EXPECT_EQ("", (*Tokens)[2]); EXPECT_EQ("trailing\\", (*Tokens)[3]);
	}

	TEST_F(FConsoleToolTests, LoadsBoundedOwnedNestedJsonWithExactIntegerTypes)
	{
		auto Descriptor = EchoDescriptor();
		FToolSchema Nested(EToolValueType::Object); Nested.Properties.emplace("id", FToolSchema(EToolValueType::UnsignedInteger));
		FToolSchema Array(EToolValueType::Array); Array.Items = std::make_unique<FToolSchema>(EToolValueType::Integer);
		Nested.Properties.emplace("values", std::move(Array));
		Descriptor.Input.Properties.emplace("nested", std::move(Nested)); Descriptor.Output = Descriptor.Input; Register(Descriptor);
		auto Result = Adapter->Execute("tests/echo --args-file " + File(R"({"nested":{"id":18446744073709551615,"values":[-1,9223372036854775807]},"text":"quoted \\\" value"})"));
		ASSERT_TRUE(Result.bSuccess) << Result.Message;
		const auto& NestedObject = std::get<FToolValue::FObject>(std::get<FToolValue::FObject>(LastArguments.Data).at("nested").Data);
		EXPECT_EQ(FToolValue(uint64{18446744073709551615ull}), NestedObject.at("id"));
		EXPECT_EQ(FToolValue(int64{9223372036854775807ll}), std::get<FToolValue::FArray>(NestedObject.at("values").Data)[1]);
		EXPECT_EQ(1, Calls);
	}

	TEST_F(FConsoleToolTests, RejectsMixedInvalidDuplicateUnknownAndOversizedJsonFiles)
	{
		Register();
		for (auto Text : {R"({"text":"a","text":"b"})", R"({"unknown":1})", R"({"text":null})", "[]", "{", "{\"text\":\"\xff\"}"})
			EXPECT_FALSE(Adapter->Execute("tests/echo --args-file " + File(Text)).bSuccess) << Text;
		EXPECT_FALSE(Adapter->Execute("tests/echo --args-file " + File("{}") + " --text x").bSuccess);
		EXPECT_FALSE(Adapter->Execute("tests/echo --text x --args-file " + File("{}")).bSuccess);
		EXPECT_FALSE(Adapter->Execute("tests/echo --args-file " + File(std::string(MaxConsoleToolTextBytes + 1, ' '))).bSuccess);
		EXPECT_FALSE(Adapter->Execute("tests/echo --args-file " + File(R"({"text":1})")).bSuccess);
		EXPECT_FALSE(Adapter->Execute("tests/echo --text " + std::string(MaxConsoleToolTextBytes, 'x')).bSuccess);
		EXPECT_EQ(0, Calls);
	}

	TEST_F(FConsoleToolTests, RejectsJsonNumericOverflowRatherThanRoundingIntegers)
	{
		auto Descriptor = EchoDescriptor(); Descriptor.Input.Properties.emplace("count", FToolSchema(EToolValueType::Integer));
		Descriptor.Input.Properties.emplace("id", FToolSchema(EToolValueType::UnsignedInteger)); Descriptor.Output = Descriptor.Input; Register(Descriptor);
		for (auto Text : {R"({"count":9223372036854775808})", R"({"count":1.5})", R"({"id":-1})", R"({"id":18446744073709551616})"})
			EXPECT_FALSE(Adapter->Execute("tests/echo --args-file " + File(Text)).bSuccess) << Text;
		EXPECT_EQ(0, Calls);
	}

	TEST_F(FConsoleToolTests, MergesHelpAndCompletionAndFailsClosedOnNameCollisions)
	{
		Register();
		auto Matches = Adapter->FindCompletions("tests/"); EXPECT_EQ((std::vector<std::string>{"tests/echo"}), Matches);
		EXPECT_NE(std::string::npos, Adapter->Execute("help tests/echo").Message.find("--text"));
		EXPECT_NE(std::string::npos, Adapter->Execute("help").Message.find("editor.pie/pause"));
		int LegacyCalls = 0;
		ASSERT_NE(0u, Legacy.RegisterCommand({"TESTS/ECHO", "collision", "", [&](auto) { ++LegacyCalls; return FConsoleCommandResult::Success(); }}));
		EXPECT_FALSE(Adapter->Execute("tests/echo").bSuccess);
		EXPECT_FALSE(Adapter->Execute(R"(tests\/echo)").bSuccess);
		EXPECT_NE(std::string::npos, Adapter->Execute("help tests/echo").Message.find("[COLLISION]"));
		ASSERT_NE(0u, Legacy.RegisterCommand({"editor.pie/status", "collision", "", [&](auto) { ++LegacyCalls; return FConsoleCommandResult::Success(); }}));
		EXPECT_FALSE(Adapter->Execute("editor.pie/status").bSuccess);
		EXPECT_EQ(0, LegacyCalls); EXPECT_EQ(0, Calls);
	}

	TEST_F(FConsoleToolTests, LeavesLegacyArgumentsInTheirOriginalParser)
	{
		ASSERT_NE(0u, Legacy.RegisterCommand({"legacy.echo", "Legacy", "legacy.echo <args>", [](std::span<const std::string> Args) {
			std::string Text; for (const auto& Arg : Args) Text += "[" + Arg + "]";
			return FConsoleCommandResult::Success(Text);
		}}));
		for (const auto* Line : {R"(legacy.echo "hello world" C:\Temp\x)", R"(legacy.echo 'a\'b')", R"(legacy.echo trailing\)", "legacy.echo \"unterminated"})
		{
			auto Expected = Legacy.Execute(Line), Actual = Adapter->Execute(Line);
			EXPECT_EQ(Expected.bSuccess, Actual.bSuccess); EXPECT_EQ(Expected.Message, Actual.Message);
		}
	}

	TEST_F(FConsoleToolTests, PIECallsUseTheAdmittingEditorAndRejectRemovedCompatibilityCommands)
	{
		auto* Other = NewHost(); EXPECT_EQ(Other, GEditor);
		for (const auto* Removed : {"pie.status", "pie.play", "pie.stop", "pie.pause", "pie.pause toggle", "editor.pie/toggle_pause"})
			EXPECT_FALSE(Adapter->Execute(Removed).bSuccess) << Removed;
		EXPECT_TRUE(Adapter->FindCompletions("pie.").empty());
		EXPECT_FALSE(Tools()->DescribeTool({"editor.pie", "toggle_pause"}));
		auto Set = Tools()->DescribeToolset("editor.pie"); ASSERT_TRUE(Set);
		ASSERT_EQ(4u, Set->Descriptor.Tools.size());
		auto Native = Tools()->CallNow({{"editor.pie", "status"}, 1});
		auto Console = Adapter->Execute("editor.pie/status"); ASSERT_TRUE(Console.bSuccess);
		EXPECT_EQ(FormatConsoleToolResult(Native).Message, Console.Message);
		EXPECT_EQ("stopped", ResultField(Console, "state"));
		EXPECT_FALSE(Adapter->Execute("editor.pie/stop").bSuccess);
		EXPECT_FALSE(Adapter->Execute("editor.pie/pause --paused true").bSuccess);
		EXPECT_FALSE(Adapter->Execute("editor.pie/play").bSuccess);
		FEditorEngineTestAccess::SetState(*Host, EPlayState::Playing);
		EXPECT_FALSE(Adapter->Execute("editor.pie/pause").bSuccess);
		EXPECT_TRUE(Adapter->Execute("editor.pie/pause --paused true").bSuccess);
		EXPECT_TRUE(Host->IsPlaySessionPaused()); EXPECT_FALSE(Other->IsPlaying());
		EXPECT_TRUE(Adapter->Execute("editor.pie/pause --paused false").bSuccess);
		EXPECT_FALSE(Host->IsPlaySessionPaused());
		EXPECT_TRUE(Adapter->Execute("editor.pie/pause --paused true").bSuccess);
		EXPECT_TRUE(Adapter->Execute("editor.pie/pause --paused true").bSuccess);
		EXPECT_TRUE(Host->IsPlaySessionPaused());
		FEditorEngineTestAccess::SetState(*Host, EPlayState::Stopped);
	}

	TEST_F(FConsoleToolTests, QueuedExplicitPauseStatesPublishStructuredReceipts)
	{
		FEditorEngineTestAccess::SetState(*Host, EPlayState::Playing);
		{
			FScopedTransaction Transaction(Host->GetTransactor(), {"defer tools"});
			EXPECT_FALSE(Tools()->CanCallNow());
			EXPECT_NE(std::string::npos, Adapter->Execute("editor.pie/pause --paused true").Message.find("Queued invocation"));
			EXPECT_NE(std::string::npos, Adapter->Execute("editor.pie/pause --paused false").Message.find("Queued invocation"));
			EXPECT_TRUE(Adapter->Poll().empty());
			Host->SetPlaySessionPaused(true);
		}
		EXPECT_TRUE(Tools()->CanCallNow());
		for (int I = 0; I < 3; ++I) Tools()->TickTools();
		auto Results = Adapter->Poll(); ASSERT_EQ(2u, Results.size());
		EXPECT_TRUE(Results[0].bSuccess); EXPECT_TRUE(Results[1].bSuccess);
		EXPECT_NE(std::string::npos, Results[0].Message.find("paused"));
		EXPECT_NE(std::string::npos, Results[1].Message.find("playing"));
		EXPECT_FALSE(Host->IsPlaySessionPaused()); EXPECT_TRUE(Adapter->Poll().empty());
		FEditorEngineTestAccess::SetState(*Host, EPlayState::Stopped);
	}

	TEST_F(FConsoleToolTests, DoesNotRetryAProviderBusyFailure)
	{
		Register(); Busy = true;
		EXPECT_FALSE(Adapter->Execute("tests/echo").bSuccess);
		Tools()->TickTools(); EXPECT_TRUE(Adapter->Poll().empty()); EXPECT_EQ(1, Calls);
		{
			FScopedTransaction Transaction(Host->GetTransactor(), {"defer tools"});
			EXPECT_TRUE(Adapter->Execute("tests/echo").bSuccess);
		}
		Tools()->TickTools();
		auto Results = Adapter->Poll(); ASSERT_EQ(1u, Results.size());
		EXPECT_FALSE(Results.front().bSuccess); EXPECT_EQ(2, Calls);
	}

	TEST_F(FConsoleToolTests, AdapterDestructionCancelsQueuedWork)
	{
		Register();
		{
			FScopedTransaction Transaction(Host->GetTransactor(), {"defer tools"});
			EXPECT_TRUE(Adapter->Execute("tests/echo").bSuccess); Adapter.reset();
		}
		Tools()->TickTools(); EXPECT_EQ(0, Calls);
	}

	TEST_F(FConsoleToolTests, ShutdownSettlesPendingReceiptsAndDetachedAdapterSurvivesHostGC)
	{
		Register();
		{
			FScopedTransaction Transaction(Host->GetTransactor(), {"defer tools"});
			EXPECT_TRUE(Adapter->Execute("tests/echo").bSuccess);
		}
		Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host); Editors.clear(); CollectGarbage();
		auto Results = Adapter->Poll(); ASSERT_EQ(1u, Results.size()); EXPECT_FALSE(Results.front().bSuccess);
		(void)NewHost();
		EXPECT_FALSE(Adapter->Execute("editor.pie/status").bSuccess); EXPECT_EQ(0, Calls);
	}

	TEST_F(FConsoleToolTests, PIEStartPauseStopShareNativeDomainServicesWithoutRendering)
	{
		auto* World = NewObject<DWorld>(Host, "ConsoleEditorWorld"); World->SetWorldType(EWorldType::Editor);
		ASSERT_TRUE(World->InitializeSubsystems());
		auto* Level = NewObject<DLevel>(World, "ConsoleEditorLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
		FEditorEngineTestAccess::SetEditorWorld(*Host, World);
		auto Started = Adapter->Execute("editor.pie/play"); ASSERT_TRUE(Started.bSuccess) << Started.Message;
		EXPECT_EQ("Play session started.", ResultField(Started, "message")); EXPECT_TRUE(Host->IsPlaying());
		EXPECT_FALSE(Adapter->Execute("editor.pie/play").bSuccess);
		EXPECT_TRUE(Adapter->Execute("editor.pie/pause --paused true").bSuccess); EXPECT_TRUE(Host->GetPlayWorld()->IsPaused());
		EXPECT_EQ("paused", ResultField(Adapter->Execute("editor.pie/status"), "state"));
		EXPECT_TRUE(Adapter->Execute("editor.pie/stop").bSuccess); EXPECT_FALSE(Host->IsPlaying());
		FEditorEngineTestAccess::SetEditorWorld(*Host, nullptr);
	}
	TEST_F(FConsoleToolTests, ReflectedPIEEntrypointsShareToolStateAndPreserveFailures)
	{
		auto* Levels = Host->GetEditorSubsystem<DLevelEditorSubsystem>(); ASSERT_NE(nullptr, Levels);
		auto Invoke = [&](const char* Name, std::optional<bool> Paused = {}) {
			const auto* Function = Levels->GetClass()->FindFunctionByName(Name);
			EXPECT_NE(nullptr, Function);
			if (!Function) return FLevelEditorPlayResult{};
			FFunctionParameterBuffer Parameters(*Function);
			if (Paused) Parameters.GetParameter<bool>(0) = *Paused;
			EXPECT_EQ(EFunctionInvokeError::None, Levels->ProcessEvent(Function, Parameters.GetView()));
			return Parameters.GetReturnValue<FLevelEditorPlayResult>();
		};
		EXPECT_EQ("stopped", Invoke("GetPlayStatus").State);
		auto MissingWorld = Invoke("StartPlaySession");
		EXPECT_FALSE(MissingWorld.bSucceeded); EXPECT_EQ("The editor world is unavailable.", MissingWorld.Message);
		EXPECT_FALSE(Invoke("SetPlaySessionPaused", true).bSucceeded);
		EXPECT_EQ("No Play session is active.", Invoke("StopPlaySession").Message);

		auto* World = NewObject<DWorld>(Host, "ReflectedEditorWorld"); World->SetWorldType(EWorldType::Editor);
		ASSERT_TRUE(World->InitializeSubsystems());
		auto* Level = NewObject<DLevel>(World, "ReflectedEditorLevel"); ASSERT_TRUE(World->SetCurrentLevel(Level));
		FEditorEngineTestAccess::SetEditorWorld(*Host, World);
		auto Started = Invoke("StartPlaySession"); ASSERT_TRUE(Started.bSucceeded) << Started.Message;
		EXPECT_EQ("Play session started.", Started.Message); EXPECT_EQ("playing", Started.State);
		EXPECT_FALSE(Invoke("StartPlaySession").bSucceeded);
		EXPECT_EQ("playing", ResultField(Adapter->Execute("editor.pie/status"), "state"));
		auto Paused = Invoke("SetPlaySessionPaused", true); EXPECT_TRUE(Paused.bSucceeded);
		EXPECT_EQ("paused", Paused.State); EXPECT_TRUE(Host->GetPlayWorld()->IsPaused());
		EXPECT_TRUE(Invoke("SetPlaySessionPaused", true).bSucceeded);
		EXPECT_TRUE(Adapter->Execute("editor.pie/pause --paused false").bSuccess);
		EXPECT_EQ("playing", Invoke("GetPlayStatus").State);
		auto Stopped = Invoke("StopPlaySession"); EXPECT_TRUE(Stopped.bSucceeded);
		EXPECT_EQ("Play session stopped.", Stopped.Message); EXPECT_EQ("stopped", Stopped.State);
		EXPECT_EQ(Levels, Host->GetEditorSubsystem<DLevelEditorSubsystem>());
		EXPECT_TRUE(Invoke("StartPlaySession").bSucceeded);
		EXPECT_TRUE(Invoke("StopPlaySession").bSucceeded);
	}
	TEST_F(FConsoleToolTests, LevelEditorOwnsThePlayProviderBeforeTheHostSnapshot)
	{
		const auto Providers = FSubsystemRegistration::Snapshot(DEditorSubsystem::StaticClass());
		const auto Count = std::ranges::count_if(Providers, [](const auto& Entry) {
			return Entry.Descriptor.Type == DLevelEditorSubsystem::StaticClass()
				&& Entry.Descriptor.Provider == FName("LevelEditor");
		});
		EXPECT_EQ(1, Count);
		ASSERT_NE(nullptr, Host->GetEditorSubsystem<DLevelEditorSubsystem>());
		auto PIE = Tools()->DescribeToolset("editor.pie"); ASSERT_TRUE(PIE);
		EXPECT_EQ(4u, PIE->Descriptor.Tools.size());
		EXPECT_TRUE(Adapter->Execute("editor.pie/status").bSuccess);
	}
	TEST_F(FConsoleToolTests, LevelEditorPlayServiceIsHostBoundAndRejectsWrongThreadAndRetirement)
	{
		auto* Other = NewHost();
		TStrongObjectPtr<DLevelEditorSubsystem> Levels(Host->GetEditorSubsystem<DLevelEditorSubsystem>());
		ASSERT_TRUE(Levels);
		EXPECT_NE(Levels.Get(), Other->GetEditorSubsystem<DLevelEditorSubsystem>());
		FEditorEngineTestAccess::SetState(*Host, EPlayState::Playing);
		EXPECT_TRUE(Levels->SetPlaySessionPaused(true).bSucceeded);
		EXPECT_FALSE(Other->IsPlaying());
		auto* Service = Levels.Get(); // The owning strong reference remains on GameThread until the worker joins.
		auto WrongThread = std::async(std::launch::async, [Service] {
			return std::array{Service->GetPlayStatus(), Service->StartPlaySession(), Service->StopPlaySession(), Service->SetPlaySessionPaused(false)};
		}).get();
		for (const auto& Result : WrongThread)
		{
			EXPECT_FALSE(Result.bSucceeded); EXPECT_EQ("Play controls require the game thread.", Result.Message);
		}
		EXPECT_TRUE(Host->IsPlaySessionPaused());
		FEditorEngineTestAccess::SetState(*Host, EPlayState::Stopped);
		Host->PrepareForShutdown();
		EXPECT_FALSE(Levels->GetPlayStatus().bSucceeded);
		EXPECT_FALSE(Levels->StartPlaySession().bSucceeded);
		EXPECT_FALSE(Levels->StopPlaySession().bSucceeded);
		EXPECT_FALSE(Levels->SetPlaySessionPaused(false).bSucceeded);
		EXPECT_TRUE(Other->GetEditorSubsystem<DLevelEditorSubsystem>()->GetPlayStatus().bSucceeded);
	}
}
