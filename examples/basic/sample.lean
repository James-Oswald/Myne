
/-! # Basic Arithmetic Examples -/


/-! ## Definitions -/

/-!
Here we define basic arithmetic operations on natural numbers, starting with addition.
-/

/--
Given a natural number $a$ and a natural number $b$, returns their sum. \citet{euclid1956elements}

$$ add(a, b) := a + b $$
-/
def add (a b : Nat) : Nat :=
    a + b

/-! ## Theorems -/

/-! Symmetry of addition -/

/--
Given two natural numbers $a$ and $b$, proves that their sum is symmetric. \cite{euclid1956elements}

$$ add(a, b) = add(b, a) $$
-/
theorem add_symm (a b : Nat) : add a b = add b a := by
  simp [add] <;> grind
