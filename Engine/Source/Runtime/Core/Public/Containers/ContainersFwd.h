#pragma once

#include <vector>
#include <array>
#include <list>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include <queue>
#include <string>
#include <tuple>

// Container types
template<typename Key, typename Value>
using TPair = std::pair<Key, Value>;

template<typename T, size_t N>
using TStaticArray = std::array<T, N>;

template<typename T, typename Allocator = std::allocator<T>>
using TArray = std::vector<T, Allocator>;

template<typename T>
using TLinkedList = std::list<T>;

template<typename T>
using TSet = std::set<T>;

template<typename Key, typename Value>
using TMap = std::map<Key, Value>;

template<typename T>
using THashSet = std::unordered_set<T>;

template<typename Key, typename Value>
using THashMap = std::unordered_map<Key, Value>;

template<typename Key, typename Value>
using TMultiMap = std::multimap<Key, Value>;

template<typename T>
using TQueue = std::queue<T>;

template<typename T>
using TStack = std::stack<T>;

// Tuple type
template<typename... Types>
using TTuple = std::tuple<Types...>;

// String types
using FString = std::string;
using FName = std::string;
using FText = std::string;
using FStringView = std::string_view;
