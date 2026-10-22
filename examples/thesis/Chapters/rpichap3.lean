/-!
# The Halting Problem
-/

/--
@name Diagonal Halting Set
@capture block
@nl
The diagonal halting set contains exactly the program codes that halt when given their own code.
@formula
$$\mathtt{K}_P := \{e : \varphi^P_e(e)\downarrow\}$$
-/
def ProgrammingSystem.K (P : ProgrammingSystem) : Set Nat :=
  {e | P.Halts e e}

def ProgrammingSystem.H (P : ProgrammingSystem) : Set Nat :=
  {z | P.Halts z.unpair.1 z.unpair.2}
--myne:end

/--
@name Halting Sets
@nl
The conventional diagonal halting set consists of the codes of programs
that halt when given their own code. The paired halting set allows an
independent input.

The word language uses any chosen codec $C$ for natural numbers.
@formula
$$\begin{aligned}
\mathtt{K}_P &:= \{e\mid\varphi^P_e(e)\downarrow\},\\
\mathtt{H}_P &:= \{\langle e,x\rangle\mid\varphi^P_e(x)\downarrow\},\\
\mathtt{L}_P(C) &:= \{C(e)\mid e\in\mathtt{K}_P\}
\end{aligned}$$
-/
def ProgrammingSystem.haltingLanguage
    (P : ProgrammingSystem) (C : Codec Nat σ) :
    Set (List σ) :=
  C.encode '' P.K
