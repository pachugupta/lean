/-
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Leonardo de Moura
-/
prelude
import init.meta.tactic init.function

namespace tactic

open expr tactic decidable environment

private meta_definition contra_A_not_A : list expr → list expr → tactic unit
| []         Hs := failed
| (H1 :: Rs) Hs :=
  do t_0 ← infer_type H1,
     t   ← whnf t_0,
     (do a ← match_not t,
         H2 ← find_same_type a Hs,
         tgt ← target,
         pr ← mk_app `absurd [tgt, H2, H1],
         exact pr)
     <|> contra_A_not_A Rs Hs

private meta_definition contra_false : list expr → tactic unit
| []        := failed
| (H :: Hs) :=
  do t ← infer_type H,
     if is_false t = tt
     then do tgt ← target,
             pr  ← mk_app `false.rec [tgt, H],
             exact pr
     else contra_false Hs

private meta_definition contra_not_a_refl_rel_a : list expr → tactic unit
| []        := failed
| (H :: Hs) :=
  do t ← infer_type H,
     (do (lhs, rhs) ← match_ne t,
         unify lhs rhs,
         tgt ← target,
         refl_pr ← mk_app `eq.refl [lhs],
         mk_app `absurd [tgt, refl_pr, H] >>= exact)
     <|>
     (do p ← match_not t,
         (refl_lemma, lhs, rhs) ← match_refl_app p,
         unify lhs rhs,
         tgt ← target,
         refl_pr ← mk_app refl_lemma [lhs],
         mk_app `absurd [tgt, refl_pr, H] >>= exact)
     <|>
     contra_not_a_refl_rel_a Hs

private meta_definition contra_constructor_eq : list expr → tactic unit
| []        := failed
| (H :: Hs) :=
  do t ← infer_type H,
     match (is_eq t) with
     | (some (lhs_0, rhs_0)) :=
       do env ← get_env,
          lhs ← whnf lhs_0,
          rhs ← whnf rhs_0,
          if is_constructor_app env lhs = tt ∧
             is_constructor_app env rhs = tt ∧
             const_name (get_app_fn lhs) ≠ const_name (get_app_fn rhs)
          then do tgt    ← target,
                  I_name ← return $ name.get_prefix (const_name (get_app_fn lhs)),
                  pr ← mk_app (I_name <.> "no_confusion") [tgt, lhs, rhs, H],
                  exact pr
          else contra_constructor_eq Hs
     | none := contra_constructor_eq Hs
     end

meta_definition contradiction : tactic unit :=
do ctx ← local_context,
   (contra_false ctx <|>
    contra_not_a_refl_rel_a ctx <|>
    contra_A_not_A ctx ctx <|>
    contra_constructor_eq ctx <|>
    fail "contradiction tactic failed")

meta_definition exfalso : tactic unit :=
do fail_if_no_goals,
   assert `Hfalse (expr.const `false []),
   swap, contradiction

end tactic
