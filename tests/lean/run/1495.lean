section
variables (r : ℕ → ℕ → Prop)
local infix `≼` : 50 := r

example {a b : ℕ} : a ≼ b → true :=
begin
  show a ≼ b → true,
    from λx, trivial
end
end
