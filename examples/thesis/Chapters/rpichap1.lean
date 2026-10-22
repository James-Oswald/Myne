/-!
# Programming Systems and the Halting Problem

This section develops acceptable programming systems and diagonal halting results.
-/

/-!
# Acceptable Programming Systems

We use the standard formulation by natural-number program codes,
universal evaluation, and the parameter theorem ($s$-$m$-$n$).
-/

/--
@name Programming System
@nl
A programming system assigns natural-number codes to numerical Turing machines and specifies their admissible input and output representations.
-/
structure ProgrammingSystem where
  denote : Nat → Codec.NatMachine
  admissibleInput : {σ : Type} → Codec Nat σ → Prop
  admissibleOutput : {σ : Type} → (List σ → Nat) → Prop

/--
@name Program evaluation
@nl
Evaluating program $e$ on input $x$ returns the partial numerical result of the machine denoted by $e$.
@formula
$$\varphi^P_e(x) := \varphi_{\mathtt{denote}_P(e)}(x)$$
-/
noncomputable def ProgrammingSystem.eval
    (P : ProgrammingSystem) (e x : Nat) : Part Nat :=
  (P.denote e).eval x

/--
@name Program Halting
@nl
Program $e$ halts on input $x$ when evaluation has a defined result.
@formula
$$\varphi^P_e(x)\downarrow$$
-/
def ProgrammingSystem.Halts
    (P : ProgrammingSystem) (e x : Nat) : Prop :=
  (P.eval e x).Dom
