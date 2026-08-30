#include "Yaml/Yaml.h"

#include "Misc/FileHelper.h"
#include "Misc/StringHelper.h"

#include <c4/yml/emit.hpp>
#include <c4/yml/parse.hpp>
#include <ryml.hpp>
#include <ryml_std.hpp>

namespace Durin
{
	struct FYamlNodeAccess
	{
		static auto MakeView(void* InTreePtr, size_t InNodeIndex) -> FYamlNodeView
		{
			return FYamlNodeView::FromOpaque(InTreePtr, InNodeIndex);
		}

		static auto MakeRef(void* InTreePtr, size_t InNodeIndex) -> FYamlNodeRef
		{
			return FYamlNodeRef::FromOpaque(InTreePtr, InNodeIndex);
		}
	};

	namespace
	{
		constexpr size_t InvalidNodeIndex = static_cast<size_t>(ryml::NONE);

		auto ToNodeIndex(ryml::id_type InNodeId) -> size_t
		{
			return InNodeId == ryml::NONE ? InvalidNodeIndex : static_cast<size_t>(InNodeId);
		}

		auto ToCSubstr(std::string_view InValue) -> c4::csubstr
		{
			return {InValue.data(), InValue.size()};
		}

		auto MakeConstNode(void* InTreePtr, size_t InNodeIndex) -> ryml::ConstNodeRef
		{
			if (!InTreePtr || InNodeIndex == InvalidNodeIndex)
			{
				return {};
			}

			return {static_cast<const ryml::Tree*>(InTreePtr), static_cast<ryml::id_type>(InNodeIndex)};
		}

		auto MakeNode(void* InTreePtr, size_t InNodeIndex) -> ryml::NodeRef
		{
			if (!InTreePtr || InNodeIndex == InvalidNodeIndex)
			{
				return {};
			}

			return {static_cast<ryml::Tree*>(InTreePtr), static_cast<ryml::id_type>(InNodeIndex)};
		}

		auto ToNodeView(const ryml::ConstNodeRef& InNode) -> FYamlNodeView
		{
			return InNode.readable()
				? FYamlNodeAccess::MakeView(const_cast<ryml::Tree*>(InNode.tree()), ToNodeIndex(InNode.id()))
				: FYamlNodeView{};
		}

		auto ToNodeRef(const ryml::NodeRef& InNode) -> FYamlNodeRef
		{
			return InNode.readable()
				? FYamlNodeAccess::MakeRef(const_cast<ryml::Tree*>(InNode.tree()), ToNodeIndex(InNode.id()))
				: FYamlNodeRef{};
		}

		auto ToOwnedString(c4::csubstr InValue) -> std::string
		{
			return InValue.str ? std::string(InValue.str, InValue.len) : std::string{};
		}

		auto EqualsIgnoreCase(std::string_view Left, std::string_view Right) -> bool
		{
			if (Left.size() != Right.size())
			{
				return false;
			}

			for (size_t Index = 0; Index < Left.size(); ++Index)
			{
				if (StringUtils::ToLowerAscii(Left[Index])
					!= StringUtils::ToLowerAscii(Right[Index]))
				{
					return false;
				}
			}

			return true;
		}

		auto TryGetScalarText(const ryml::ConstNodeRef& InNode, std::string_view* OutText) -> bool
		{
			if (!InNode.readable() || InNode.is_map() || InNode.is_seq() || !InNode.has_val())
			{
				return false;
			}

			const c4::csubstr Value = InNode.val();
			if (!Value.str)
			{
				return false;
			}

			*OutText = std::string_view(Value.str, Value.len);
			return true;
		}

		auto TryParseBool(std::string_view InText, bool* OutValue) -> bool
		{
			if (EqualsIgnoreCase(InText, "true") || EqualsIgnoreCase(InText, "yes") || EqualsIgnoreCase(InText, "on") || InText == "1")
			{
				*OutValue = true;
				return true;
			}

			if (EqualsIgnoreCase(InText, "false") || EqualsIgnoreCase(InText, "no") || EqualsIgnoreCase(InText, "off") || InText == "0")
			{
				*OutValue = false;
				return true;
			}

			return false;
		}

		template<typename TInteger>
		auto TryParseInteger(std::string_view InText, TInteger* OutValue) -> bool
		{
			TInteger ParsedValue{};
			const auto [Ptr, Error] = std::from_chars(InText.data(), InText.data() + InText.size(), ParsedValue);
			if (Error != std::errc{} || Ptr != InText.data() + InText.size())
			{
				return false;
			}

			*OutValue = ParsedValue;
			return true;
		}

		auto TryParseDouble(std::string_view InText, double* OutValue) -> bool
		{
			double ParsedValue = 0.0;
			const auto [Ptr, Error] = std::from_chars(InText.data(), InText.data() + InText.size(), ParsedValue);
			if (Error != std::errc{} || Ptr != InText.data() + InText.size())
			{
				return false;
			}

			*OutValue = ParsedValue;
			return true;
		}

		auto EnsureMapNode(ryml::NodeRef Node) -> void
		{
			if (!Node.readable())
			{
				return;
			}

			if (!Node.is_map())
			{
				Node.clear();
				Node |= ryml::MAP;
			}
		}

		auto EnsureSequenceNode(ryml::NodeRef Node) -> void
		{
			if (!Node.readable())
			{
				return;
			}

			if (!Node.is_seq())
			{
				Node.clear();
				Node |= ryml::SEQ;
			}
		}

		template<typename TValue>
		auto SetNodeScalar(ryml::NodeRef Node, const TValue& InValue) -> void
		{
			if (!Node.readable())
			{
				return;
			}

			const bool bHasKey = Node.has_key();
			Node.clear_children();
			Node.clear_val();
			Node.set_type(bHasKey ? ryml::KEYVAL : ryml::VAL);
			Node << InValue;
		}

		template<typename TValue>
		auto SetChildScalar(ryml::NodeRef ParentNode, std::string_view InKey, const TValue& InValue) -> void
		{
			EnsureMapNode(ParentNode);
			if (!ParentNode.readable())
			{
				return;
			}

			ryml::NodeRef ChildNode = ParentNode[ToCSubstr(InKey)];
			if (ChildNode.readable())
			{
				ChildNode.clear_children();
				ChildNode.clear_val();
				ChildNode.set_type(ryml::KEYVAL);
			}
			ChildNode << InValue;
		}

		template<typename TValue>
		auto AppendScalar(ryml::NodeRef ParentNode, const TValue& InValue) -> void
		{
			EnsureSequenceNode(ParentNode);
			if (!ParentNode.readable())
			{
				return;
			}

			ryml::NodeRef ChildNode = ParentNode.append_child();
			ChildNode << InValue;
		}

		auto PopulateLocation(FYamlParseError* OutError, const c4::yml::Location& InLocation) -> void
		{
			if (!OutError)
			{
				return;
			}

			if (InLocation.offset != c4::yml::npos)
			{
				OutError->BytePosition = InLocation.offset;
			}
			if (InLocation.line != c4::yml::npos)
			{
				OutError->Line = InLocation.line;
			}
			if (InLocation.col != c4::yml::npos)
			{
				OutError->Column = InLocation.col;
			}
		}

		struct FYamlParseException final : public std::exception
		{
			FYamlParseError Error;

			explicit FYamlParseException(FYamlParseError InError)
				: Error(std::move(InError))
			{
				if (Error.Code == 0)
				{
					Error.Code = 1;
				}
				if (Error.Message.empty())
				{
					Error.Message = "Unknown YAML parse failure.";
				}
			}

			const char* what() const noexcept override
			{
				return Error.Message.c_str();
			}
		};

		struct FYamlCallbackContext
		{
		};

		[[noreturn]] auto ThrowYamlError(std::string Message, const c4::yml::Location* InLocation = nullptr) -> void
		{
			FYamlParseError Error;
			Error.Code = 1;
			Error.Message = std::move(Message);

			if (InLocation)
			{
				PopulateLocation(&Error, *InLocation);
			}

			throw FYamlParseException(std::move(Error));
		}

		[[noreturn]] auto OnYamlBasicError(c4::csubstr InMessage, const c4::yml::ErrorDataBasic& ErrorData, void* UserData) -> void
		{
			(void)UserData;
			ThrowYamlError(ToOwnedString(InMessage), &ErrorData.location);
		}

		[[noreturn]] auto OnYamlParseError(c4::csubstr InMessage, const c4::yml::ErrorDataParse& ErrorData, void* UserData) -> void
		{
			(void)UserData;
			ThrowYamlError(ToOwnedString(InMessage), &ErrorData.ymlloc);
		}

		[[noreturn]] auto OnYamlVisitError(c4::csubstr InMessage, const c4::yml::ErrorDataVisit& ErrorData, void* UserData) -> void
		{
			(void)UserData;
			ThrowYamlError(ToOwnedString(InMessage), &ErrorData.cpploc);
		}

		auto MakeYamlCallbacks(void* UserData) -> c4::yml::Callbacks
		{
			c4::yml::Callbacks Callbacks = c4::yml::get_callbacks();
			Callbacks.set_user_data(UserData);
			Callbacks.set_error_basic(&OnYamlBasicError);
			Callbacks.set_error_parse(&OnYamlParseError);
			Callbacks.set_error_visit(&OnYamlVisitError);
			return Callbacks;
		}

		auto PopulateParseError(FYamlParseError* OutError, const FYamlParseException& InException) -> void
		{
			if (!OutError)
			{
				return;
			}

			*OutError = InException.Error;
		}

		auto PopulateLoadError(FYamlParseError* OutError, std::string_view InFilePath) -> void
		{
			if (!OutError)
			{
				return;
			}

			*OutError = {};
			OutError->Message = std::format("Failed to load YAML file: {}", InFilePath);
		}

		auto PopulateUnhandledError(FYamlParseError* OutError, const std::exception& InException) -> void
		{
			if (!OutError)
			{
				return;
			}

			*OutError = {};
			OutError->Code = 1;
			OutError->Message = InException.what();
		}
	} // namespace

	auto FYamlNodeView::IsValid() const -> bool
	{
		return MakeConstNode(TreePtr, NodeIndex).readable();
	}

	auto FYamlNodeView::IsScalar() const -> bool
	{
		std::string_view Unused;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &Unused);
	}

	auto FYamlNodeView::IsMap() const -> bool
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		return Node.readable() && Node.is_map();
	}

	auto FYamlNodeView::IsSequence() const -> bool
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		return Node.readable() && Node.is_seq();
	}

	auto FYamlNodeView::Num() const -> size_t
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		return Node.readable() && Node.is_container() ? Node.num_children() : 0;
	}

	auto FYamlNodeView::Contains(std::string_view InKey) const -> bool
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		return Node.readable() && Node.is_map() && Node.has_child(ToCSubstr(InKey));
	}

	auto FYamlNodeView::GetKey() const -> std::string
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		return Node.readable() && Node.has_key() ? ToOwnedString(Node.key()) : std::string{};
	}

	auto FYamlNodeView::GetView(std::string_view InKey) const -> FYamlNodeView
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		if (!Node.readable() || !Node.is_map())
		{
			return {};
		}

		return ToNodeView(Node.find_child(ToCSubstr(InKey)));
	}

	auto FYamlNodeView::GetView(size_t Index) const -> FYamlNodeView
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		if (!Node.readable() || !Node.is_container() || Index >= Node.num_children())
		{
			return {};
		}

		return ToNodeView(Node.child(static_cast<ryml::id_type>(Index)));
	}

	auto FYamlNodeView::GetString(std::string DefaultValue) const -> std::string
	{
		std::string_view ScalarText;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) ? std::string(ScalarText) : std::move(DefaultValue);
	}

	auto FYamlNodeView::GetBool(bool DefaultValue) const -> bool
	{
		std::string_view ScalarText;
		bool ParsedValue = DefaultValue;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseBool(ScalarText, &ParsedValue) ? ParsedValue : DefaultValue;
	}

	auto FYamlNodeView::GetInt(int64 DefaultValue) const -> int64
	{
		std::string_view ScalarText;
		int64 ParsedValue = DefaultValue;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseInteger(ScalarText, &ParsedValue) ? ParsedValue : DefaultValue;
	}

	auto FYamlNodeView::GetUInt(uint64 DefaultValue) const -> uint64
	{
		std::string_view ScalarText;
		uint64 ParsedValue = DefaultValue;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseInteger(ScalarText, &ParsedValue) ? ParsedValue : DefaultValue;
	}

	auto FYamlNodeView::GetDouble(double DefaultValue) const -> double
	{
		std::string_view ScalarText;
		double ParsedValue = DefaultValue;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseDouble(ScalarText, &ParsedValue) ? ParsedValue : DefaultValue;
	}

	auto FYamlNodeView::GetValue(std::string& OutValue) const -> bool
	{
		std::string_view ScalarText;
		if (!TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText))
		{
			return false;
		}

		OutValue.assign(ScalarText.data(), ScalarText.size());
		return true;
	}

	auto FYamlNodeView::GetValue(bool& bOutValue) const -> bool
	{
		std::string_view ScalarText;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseBool(ScalarText, &bOutValue);
	}

	auto FYamlNodeView::GetValue(int64& OutValue) const -> bool
	{
		std::string_view ScalarText;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseInteger(ScalarText, &OutValue);
	}

	auto FYamlNodeView::GetValue(uint64& OutValue) const -> bool
	{
		std::string_view ScalarText;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseInteger(ScalarText, &OutValue);
	}

	auto FYamlNodeView::GetValue(double& OutValue) const -> bool
	{
		std::string_view ScalarText;
		return TryGetScalarText(MakeConstNode(TreePtr, NodeIndex), &ScalarText) && TryParseDouble(ScalarText, &OutValue);
	}

	auto FYamlNodeView::GetChildValue(std::string_view InKey, std::string& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FYamlNodeView::GetChildValue(std::string_view InKey, bool& bOutValue) const -> bool
	{
		return GetView(InKey).GetValue(bOutValue);
	}

	auto FYamlNodeView::GetChildValue(std::string_view InKey, int64& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FYamlNodeView::GetChildValue(std::string_view InKey, uint64& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FYamlNodeView::GetChildValue(std::string_view InKey, double& OutValue) const -> bool
	{
		return GetView(InKey).GetValue(OutValue);
	}

	auto FYamlNodeRef::GetRef(std::string_view InKey) const -> FYamlNodeRef
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		if (!Node.readable() || !Node.is_map())
		{
			return {};
		}

		const ryml::ConstNodeRef ChildNode = Node.find_child(ToCSubstr(InKey));
		if (!ChildNode.readable())
		{
			return {};
		}

		return FYamlNodeAccess::MakeRef(TreePtr, ToNodeIndex(ChildNode.id()));
	}

	auto FYamlNodeRef::GetRef(size_t Index) const -> FYamlNodeRef
	{
		const auto Node = MakeConstNode(TreePtr, NodeIndex);
		if (!Node.readable() || !Node.is_container() || Index >= Node.num_children())
		{
			return {};
		}

		const ryml::ConstNodeRef ChildNode = Node.child(static_cast<ryml::id_type>(Index));
		if (!ChildNode.readable())
		{
			return {};
		}

		return FYamlNodeAccess::MakeRef(TreePtr, ToNodeIndex(ChildNode.id()));
	}

	auto FYamlNodeRef::EnsureMap() -> FYamlNodeRef&
	{
		EnsureMapNode(MakeNode(TreePtr, NodeIndex));
		return *this;
	}

	auto FYamlNodeRef::EnsureSequence() -> FYamlNodeRef&
	{
		EnsureSequenceNode(MakeNode(TreePtr, NodeIndex));
		return *this;
	}

	auto FYamlNodeRef::SetValue(std::string_view InValue) -> void
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		if (Node.readable())
		{
			Node.clear();
			Node << ToCSubstr(InValue);
		}
	}

	auto FYamlNodeRef::SetValue(const char* InValue) -> void
	{
		SetValue(InValue != nullptr ? std::string_view(InValue) : std::string_view{});
	}

	auto FYamlNodeRef::SetValue(bool bInValue) -> void
	{
		SetNodeScalar(MakeNode(TreePtr, NodeIndex), bInValue);
	}

	auto FYamlNodeRef::SetValue(int32 InValue) -> void
	{
		SetNodeScalar(MakeNode(TreePtr, NodeIndex), static_cast<int64>(InValue));
	}

	auto FYamlNodeRef::SetValue(int64 InValue) -> void
	{
		SetNodeScalar(MakeNode(TreePtr, NodeIndex), InValue);
	}

	auto FYamlNodeRef::SetValue(uint32 InValue) -> void
	{
		SetNodeScalar(MakeNode(TreePtr, NodeIndex), static_cast<uint64>(InValue));
	}

	auto FYamlNodeRef::SetValue(uint64 InValue) -> void
	{
		SetNodeScalar(MakeNode(TreePtr, NodeIndex), InValue);
	}

	auto FYamlNodeRef::SetValue(double InValue) -> void
	{
		SetNodeScalar(MakeNode(TreePtr, NodeIndex), InValue);
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, std::string_view InValue) -> void
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		EnsureMapNode(Node);
		if (Node.readable())
		{
			ryml::NodeRef ChildNode = Node[ToCSubstr(InKey)];
			if (ChildNode.readable())
			{
				ChildNode.clear_children();
				ChildNode.clear_val();
				ChildNode.set_type(ryml::KEYVAL);
			}
			ChildNode << ToCSubstr(InValue);
		}
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, const char* InValue) -> void
	{
		SetChildValue(InKey, InValue != nullptr ? std::string_view(InValue) : std::string_view{});
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, bool bInValue) -> void
	{
		SetChildScalar(MakeNode(TreePtr, NodeIndex), InKey, bInValue);
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, int32 InValue) -> void
	{
		SetChildScalar(MakeNode(TreePtr, NodeIndex), InKey, static_cast<int64>(InValue));
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, int64 InValue) -> void
	{
		SetChildScalar(MakeNode(TreePtr, NodeIndex), InKey, InValue);
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, uint32 InValue) -> void
	{
		SetChildScalar(MakeNode(TreePtr, NodeIndex), InKey, static_cast<uint64>(InValue));
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, uint64 InValue) -> void
	{
		SetChildScalar(MakeNode(TreePtr, NodeIndex), InKey, InValue);
	}

	auto FYamlNodeRef::SetChildValue(std::string_view InKey, double InValue) -> void
	{
		SetChildScalar(MakeNode(TreePtr, NodeIndex), InKey, InValue);
	}

	auto FYamlNodeRef::AddMap(std::string_view InKey) -> FYamlNodeRef
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		EnsureMapNode(Node);
		if (!Node.readable())
		{
			return {};
		}

		ryml::NodeRef ChildNode = Node[ToCSubstr(InKey)];
		if (ChildNode.readable())
		{
			ChildNode.clear();
		}
		ChildNode |= ryml::MAP;
		return ToNodeRef(ChildNode);
	}

	auto FYamlNodeRef::AddSequence(std::string_view InKey) -> FYamlNodeRef
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		EnsureMapNode(Node);
		if (!Node.readable())
		{
			return {};
		}

		ryml::NodeRef ChildNode = Node[ToCSubstr(InKey)];
		if (ChildNode.readable())
		{
			ChildNode.clear();
		}
		ChildNode |= ryml::SEQ;
		return ToNodeRef(ChildNode);
	}

	auto FYamlNodeRef::AppendValue(std::string_view InValue) -> FYamlNodeRef&
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		EnsureSequenceNode(Node);
		if (Node.readable())
		{
			ryml::NodeRef ChildNode = Node.append_child();
			ChildNode << ToCSubstr(InValue);
		}
		return *this;
	}

	auto FYamlNodeRef::AppendValue(const char* InValue) -> FYamlNodeRef&
	{
		return AppendValue(InValue != nullptr ? std::string_view(InValue) : std::string_view{});
	}

	auto FYamlNodeRef::AppendValue(bool bInValue) -> FYamlNodeRef&
	{
		AppendScalar(MakeNode(TreePtr, NodeIndex), bInValue);
		return *this;
	}

	auto FYamlNodeRef::AppendValue(int32 InValue) -> FYamlNodeRef&
	{
		AppendScalar(MakeNode(TreePtr, NodeIndex), static_cast<int64>(InValue));
		return *this;
	}

	auto FYamlNodeRef::AppendValue(int64 InValue) -> FYamlNodeRef&
	{
		AppendScalar(MakeNode(TreePtr, NodeIndex), InValue);
		return *this;
	}

	auto FYamlNodeRef::AppendValue(uint32 InValue) -> FYamlNodeRef&
	{
		AppendScalar(MakeNode(TreePtr, NodeIndex), static_cast<uint64>(InValue));
		return *this;
	}

	auto FYamlNodeRef::AppendValue(uint64 InValue) -> FYamlNodeRef&
	{
		AppendScalar(MakeNode(TreePtr, NodeIndex), InValue);
		return *this;
	}

	auto FYamlNodeRef::AppendValue(double InValue) -> FYamlNodeRef&
	{
		AppendScalar(MakeNode(TreePtr, NodeIndex), InValue);
		return *this;
	}

	auto FYamlNodeRef::AppendMap() -> FYamlNodeRef
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		EnsureSequenceNode(Node);
		if (!Node.readable())
		{
			return {};
		}

		ryml::NodeRef ChildNode = Node.append_child();
		ChildNode |= ryml::MAP;
		return ToNodeRef(ChildNode);
	}

	auto FYamlNodeRef::AppendSequence() -> FYamlNodeRef
	{
		ryml::NodeRef Node = MakeNode(TreePtr, NodeIndex);
		EnsureSequenceNode(Node);
		if (!Node.readable())
		{
			return {};
		}

		ryml::NodeRef ChildNode = Node.append_child();
		ChildNode |= ryml::SEQ;
		return ToNodeRef(ChildNode);
	}

		struct FYamlDocument::FImpl
		{
			FYamlCallbackContext CallbackContext;
			ryml::Tree Tree = ryml::Tree(MakeYamlCallbacks(&CallbackContext));
			std::string SourceText;
			std::string SourceName;
			bool bIsValid = false;

			auto Reset() -> void
			{
				Tree = ryml::Tree(MakeYamlCallbacks(&CallbackContext));
				SourceText.clear();
				SourceName.clear();
				bIsValid = false;
			}

			auto EnsureRoot() -> void
			{
				if (Tree.empty())
				{
					Tree.reserve();
				}
			}
		};

	FYamlDocument::FYamlDocument()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FYamlDocument::~FYamlDocument() = default;

	FYamlDocument::FYamlDocument(FYamlDocument&& Other) noexcept = default;

	auto FYamlDocument::operator=(FYamlDocument&& Other) noexcept -> FYamlDocument& = default;

	auto FYamlDocument::Parse(std::string_view YamlText, FYamlParseError* OutError) -> bool
	{
		Impl->Reset();

		if (OutError)
		{
			*OutError = {};
		}

		Impl->SourceText.assign(YamlText);

		try
		{
			ryml::parse_in_arena(c4::csubstr{}, c4::to_csubstr(Impl->SourceText), &Impl->Tree, c4::yml::ParserOptions{}.locations(true));
			Impl->bIsValid = true;
			return true;
		}
		catch (const FYamlParseException& Exception)
		{
			PopulateParseError(OutError, Exception);
		}
		catch (const std::exception& Exception)
		{
			PopulateUnhandledError(OutError, Exception);
		}

		Impl->Reset();
		return false;
	}

	auto FYamlDocument::LoadFromFile(std::string_view FilePath, FYamlParseError* OutError) -> bool
	{
		std::string YamlText;
		if (!FFileHelper::LoadFileToString(YamlText, FilePath))
		{
			PopulateLoadError(OutError, FilePath);
			return false;
		}

		Impl->Reset();

		if (OutError)
		{
			*OutError = {};
		}

		Impl->SourceText = std::move(YamlText);
		Impl->SourceName.assign(FilePath);

		try
		{
			ryml::parse_in_arena(c4::to_csubstr(Impl->SourceName), c4::to_csubstr(Impl->SourceText), &Impl->Tree, c4::yml::ParserOptions{}.locations(true));
			Impl->bIsValid = true;
			return true;
		}
		catch (const FYamlParseException& Exception)
		{
			PopulateParseError(OutError, Exception);
		}
		catch (const std::exception& Exception)
		{
			PopulateUnhandledError(OutError, Exception);
		}

		Impl->Reset();
		return false;
	}

	auto FYamlDocument::Reset() -> void
	{
		Impl->Reset();
	}

	auto FYamlDocument::IsValid() const -> bool
	{
		return Impl->bIsValid;
	}

	auto FYamlDocument::GetRootView() const -> FYamlNodeView
	{
		if (!Impl->bIsValid)
		{
			return {};
		}

		return FYamlNodeAccess::MakeView(const_cast<ryml::Tree*>(&Impl->Tree), ToNodeIndex(Impl->Tree.root_id()));
	}

	auto FYamlDocument::GetMutableRoot() -> FYamlNodeRef
	{
		Impl->EnsureRoot();
		Impl->bIsValid = true;
		return FYamlNodeAccess::MakeRef(&Impl->Tree, ToNodeIndex(Impl->Tree.root_id()));
	}

	auto FYamlDocument::ToString() const -> std::string
	{
		if (!Impl->bIsValid)
		{
			return {};
		}

		try
		{
			return ryml::emitrs_yaml<std::string>(Impl->Tree);
		}
		catch (const std::exception&)
		{
			return {};
		}
	}

	auto FYamlDocument::SaveToFile(std::string_view FilePath) const -> bool
	{
		if (!Impl->bIsValid)
		{
			return false;
		}

		const std::filesystem::path OutputPath(FilePath);
		if (OutputPath.has_parent_path())
		{
			std::error_code ErrorCode;
			std::filesystem::create_directories(OutputPath.parent_path(), ErrorCode);
			if (ErrorCode)
			{
				DURIN_ERROR("Failed to create directories for YAML file {}: {}", OutputPath.parent_path().string(), ErrorCode.message());
				return false;
			}
		}

		const std::string YamlText = ToString();
		std::ofstream File(OutputPath, std::ios::binary | std::ios::out);
		if (!File.is_open())
		{
			DURIN_ERROR("Failed to open YAML file for writing: {}", OutputPath.string());
			return false;
		}

		File.write(YamlText.data(), static_cast<std::streamsize>(YamlText.size()));
		if (File.fail())
		{
			DURIN_ERROR("Failed to write YAML file: {}", OutputPath.string());
			return false;
		}

		File.close();
		return !File.fail();
	}
}
