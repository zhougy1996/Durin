template <typename T>
void OrderingB(T& Value)
{
	checkf(
		Value.Update(
			[] (auto& Item) { return Item.Commit(); }),
		"nested {}", Value.Count());
	verify(++Value.Counter);
	check((Value.Reset(), Value.IsReady()));
}
