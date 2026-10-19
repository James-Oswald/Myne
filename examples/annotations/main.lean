/-! # Annotated Statements -/

/--
@name Addition is commutative
@nl
For all natural numbers $a$ and $b$, their sum is unchanged when the terms are reversed.
@formula
$$a + b = b + a$$
@proof
The result follows from commutativity of addition on the natural numbers.
-/
theorem add_comm_example (a b : Nat) : a + b = b + a := Nat.add_comm a b

/--
@name Natural numbers have a zero element
@nl
Zero is a natural number that acts as an additive identity.
@formula
$$a + 0 = a$$
-/
def zero_identity_example (a : Nat) : a + 0 = a := Nat.add_zero a
