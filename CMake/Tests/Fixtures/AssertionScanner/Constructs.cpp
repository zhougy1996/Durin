void Constructs()
{
	check(Value = Next);
	check(++Value);
	check(new Widget());
	check(delete Pointer);
	check(co_await Task);
	check(Value + Other);
	requiref(Value.Commit(), "required operation");
}
