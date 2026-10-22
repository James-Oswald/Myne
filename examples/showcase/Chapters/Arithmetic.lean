/-!
# Arithmetic

The chapter introduces a definition and its basic properties.
-/

/-! ## A definition and a related theorem -/

/--
@name Doubling
@capture block
@nl
Doubling a natural number adds it to itself.
@formula
$$\mathtt{double}(n) := n+n$$
-/
def double (n : Nat) : Nat := n + n

theorem double_eq_add (n : Nat) : double n = n + n := by
  rfl
--myne:end

/-!
The next statement uses a regular doc comment, so one declaration is captured.
-/

/--
@name Doubling zero
@nl
Zero remains zero when doubled.
@formula
$$\mathtt{double}(0)=0$$
@proof
Unfold the definition and simplify the addition.
-/
theorem double_zero : double 0 = 0 := by
  rfl
