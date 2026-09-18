/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 */

#include "nir_test.h"

namespace {

class nir_opt_undistribute_select_test : public nir_test {
protected:
   nir_opt_undistribute_select_test()
      : nir_test::nir_test("nir_opt_undistribute_select_test")
   {
      /* What nouveau sets: fract() reaches the pass as a - floor(a). */
      options.lower_ffract = true;
   }

   nir_def *cond()
   {
      return nir_fge(b, in(1), nir_imm_float(b, 0.5));
   }

   nir_def *in(unsigned num_components, unsigned bit_size = 32)
   {
      return nir_load_ssbo(b, num_components, bit_size, nir_imm_int(b, 0),
                           nir_imm_int(b, 64 * offset++));
   }

   nir_intrinsic_instr *out(nir_def *def)
   {
      return nir_store_ssbo(b, def, nir_imm_int(b, 0), nir_imm_int(b, 0));
   }

   bool run_pass()
   {
      bool progress = nir_opt_undistribute_select(b->shader);
      nir_validate_shader(b->shader, "after nir_opt_undistribute_select");
      return progress;
   }

   /* The order nouveau's runOptLoop() runs them in. Returns false if the
    * loop has not settled after 32 rounds.
    */
   bool opt_loop()
   {
      for (unsigned i = 0; i < 32; i++) {
         bool progress = false;

         progress |= nir_opt_cse(b->shader);
         progress |= nir_opt_algebraic(b->shader);
         progress |= nir_opt_undistribute_select(b->shader);
         progress |= nir_opt_constant_folding(b->shader);
         progress |= nir_opt_copy_prop(b->shader);
         progress |= nir_opt_dce(b->shader);
         nir_validate_shader(b->shader, "in the opt loop");

         if (!progress)
            return true;
      }

      return false;
   }

   unsigned count(nir_op op)
   {
      unsigned n = 0;

      nir_foreach_block(block, b->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               n++;
         }
      }

      return n;
   }

   static nir_alu_instr *alu(nir_def *def)
   {
      EXPECT_EQ(nir_def_instr(def)->type, nir_instr_type_alu);
      return nir_def_as_alu(def);
   }

   void fract_case(nir_op op);

   unsigned offset = 0;
};

/* if (d >= 0.5) d = fract(2.0 * d); else d = fract(3.0 * d); with fract()
 * lowered: the arms differ only in the multiplication, read twice each. */
void
nir_opt_undistribute_select_test::fract_case(nir_op op)
{
   nir_def *d = in(1);
   nir_def *c = nir_fge(b, d, nir_imm_float(b, 0.5));
   nir_def *x = nir_fmul_imm(b, d, 2.0);
   nir_def *y = nir_fmul_imm(b, d, 3.0);

   if (op == nir_op_b32csel)
      c = nir_b2b32(b, c);

   nir_def *r = nir_build_alu(b, op, c,
                              nir_fsub(b, x, nir_ffloor(b, x)),
                              nir_fsub(b, y, nir_ffloor(b, y)),
                              NULL);
   nir_intrinsic_instr *store = out(r);

   ASSERT_TRUE(run_pass());

   nir_alu_instr *sub = alu(store->src[0].ssa);
   ASSERT_EQ(sub->op, nir_op_fsub);
   nir_def *sel = sub->src[0].src.ssa;
   ASSERT_EQ(alu(sel)->op, op);
   EXPECT_EQ(alu(sel)->src[0].src.ssa, c);
   EXPECT_EQ(alu(sel)->src[1].src.ssa, x);
   EXPECT_EQ(alu(sel)->src[2].src.ssa, y);
   nir_alu_instr *floor = alu(sub->src[1].src.ssa);
   ASSERT_EQ(floor->op, nir_op_ffloor);
   EXPECT_EQ(floor->src[0].src.ssa, sel);

   /* With the old arms gone, the multiplications are the arms. */
   nir_opt_dce(b->shader);
   ASSERT_TRUE(run_pass());

   nir_alu_instr *mul = alu(sub->src[0].src.ssa);
   ASSERT_EQ(mul->op, nir_op_fmul);
   EXPECT_EQ(mul->src[0].src.ssa, d);
   ASSERT_EQ(alu(mul->src[1].src.ssa)->op, op);

   EXPECT_FALSE(run_pass());

   nir_opt_dce(b->shader);
   EXPECT_EQ(count(nir_op_fmul), 1);
   EXPECT_EQ(count(op), 1);
   EXPECT_EQ(count(nir_op_ffloor), 1);
   EXPECT_EQ(count(nir_op_fsub), 1);
}

TEST_F(nir_opt_undistribute_select_test, lowered_fract_bcsel)
{
   fract_case(nir_op_bcsel);
}

TEST_F(nir_opt_undistribute_select_test, lowered_fract_b32csel)
{
   fract_case(nir_op_b32csel);
}

/* The same shader as it comes in, fract() not yet lowered. */
TEST_F(nir_opt_undistribute_select_test, lowered_fract_opt_loop)
{
   nir_def *d = in(1);
   nir_def *r = nir_bcsel(b, nir_fge(b, d, nir_imm_float(b, 0.5)),
                          nir_ffract(b, nir_fmul_imm(b, d, 2.0)),
                          nir_ffract(b, nir_fmul_imm(b, d, 3.0)));
   out(r);

   ASSERT_TRUE(opt_loop());

   EXPECT_EQ(count(nir_op_fmul), 1);
   EXPECT_EQ(count(nir_op_bcsel), 1);
   EXPECT_EQ(count(nir_op_ffloor), 1);
}

TEST_F(nir_opt_undistribute_select_test, two_pairs)
{
   nir_def *r = nir_bcsel(b, cond(),
                          nir_fadd(b, in(1), in(1)),
                          nir_fadd(b, in(1), in(1)));
   out(r);

   EXPECT_FALSE(run_pass());
}

TEST_F(nir_opt_undistribute_select_test, arm_has_other_user)
{
   nir_def *x = nir_fneg(b, in(1));
   nir_def *y = nir_fneg(b, in(1));

   out(nir_bcsel(b, cond(), x, y));
   out(x);

   EXPECT_FALSE(run_pass());
}

/* The tree stops at the node with another user, which becomes the leaf. */
TEST_F(nir_opt_undistribute_select_test, interior_node_has_other_user)
{
   nir_def *k = in(1);
   nir_def *x = nir_fmul(b, in(1), k);
   nir_def *y = nir_fmul(b, in(1), k);
   nir_def *c = cond();
   nir_intrinsic_instr *store =
      out(nir_bcsel(b, c, nir_fneg(b, x), nir_fneg(b, y)));
   nir_intrinsic_instr *other = out(x);

   ASSERT_TRUE(run_pass());

   nir_alu_instr *neg = alu(store->src[0].ssa);
   ASSERT_EQ(neg->op, nir_op_fneg);
   nir_alu_instr *sel = alu(neg->src[0].src.ssa);
   ASSERT_EQ(sel->op, nir_op_bcsel);
   EXPECT_EQ(sel->src[1].src.ssa, x);
   EXPECT_EQ(sel->src[2].src.ssa, y);
   EXPECT_EQ(other->src[0].ssa, x);
}

TEST_F(nir_opt_undistribute_select_test, identical_arms)
{
   nir_def *v = in(1);
   nir_def *k = in(1);
   nir_def *x = nir_fadd(b, v, k);
   nir_def *y = nir_fadd(b, v, k);
   nir_intrinsic_instr *store =
      out(nir_bcsel(b, cond(), x, y));

   ASSERT_TRUE(run_pass());

   EXPECT_EQ(store->src[0].ssa, x);
   EXPECT_EQ(count(nir_op_bcsel), 0);
}

/* Ten levels deep: the select goes down eight and no further. */
TEST_F(nir_opt_undistribute_select_test, depth_limit)
{
   nir_def *x[11], *y[11];

   x[0] = in(1);
   y[0] = in(1);
   for (unsigned i = 1; i < 11; i++) {
      x[i] = nir_fneg(b, x[i - 1]);
      y[i] = nir_fneg(b, y[i - 1]);
   }

   nir_intrinsic_instr *store =
      out(nir_bcsel(b, cond(), x[10], y[10]));

   ASSERT_TRUE(run_pass());

   nir_def *def = store->src[0].ssa;
   for (unsigned i = 0; i < 8; i++) {
      ASSERT_EQ(alu(def)->op, nir_op_fneg);
      def = alu(def)->src[0].src.ssa;
   }

   ASSERT_EQ(alu(def)->op, nir_op_bcsel);
   EXPECT_EQ(alu(def)->src[1].src.ssa, x[2]);
   EXPECT_EQ(alu(def)->src[2].src.ssa, y[2]);
}

TEST_F(nir_opt_undistribute_select_test, arm_swizzle)
{
   nir_def *k = in(2);
   nir_def *r = nir_bcsel(b, cond(),
                          nir_fadd(b, in(2), k), nir_fadd(b, in(2), k));
   out(r);

   alu(r)->src[1].swizzle[0] = 1;
   alu(r)->src[1].swizzle[1] = 0;

   EXPECT_FALSE(run_pass());
}

/* A swizzle inside the tree is fine when every lane has the same condition. */
TEST_F(nir_opt_undistribute_select_test, inner_swizzle_scalar_condition)
{
   nir_def *k = in(2);
   nir_def *x = in(2);
   nir_def *y = in(2);
   nir_def *ax = nir_fadd(b, x, k);
   nir_def *ay = nir_fadd(b, y, k);

   for (nir_def *a : {ax, ay}) {
      alu(a)->src[0].swizzle[0] = 1;
      alu(a)->src[0].swizzle[1] = 0;
   }

   nir_intrinsic_instr *store =
      out(nir_bcsel(b, cond(), ax, ay));

   ASSERT_TRUE(run_pass());

   nir_alu_instr *add = alu(store->src[0].ssa);
   ASSERT_EQ(add->op, nir_op_fadd);
   EXPECT_EQ(add->src[0].swizzle[0], 1);
   EXPECT_EQ(add->src[0].swizzle[1], 0);
   EXPECT_EQ(add->src[1].src.ssa, k);
   nir_alu_instr *sel = alu(add->src[0].src.ssa);
   ASSERT_EQ(sel->op, nir_op_bcsel);
   EXPECT_EQ(sel->def.num_components, 2);
   EXPECT_EQ(sel->src[0].swizzle[0], 0);
   EXPECT_EQ(sel->src[0].swizzle[1], 0);
   EXPECT_EQ(sel->src[1].src.ssa, x);
   EXPECT_EQ(sel->src[2].src.ssa, y);
}

TEST_F(nir_opt_undistribute_select_test, per_lane_condition)
{
   nir_def *k = in(2);
   nir_def *r = nir_bcsel(b, nir_fge(b, in(2), nir_imm_vec2(b, 0.5, 0.5)),
                          nir_fadd(b, in(2), k), nir_fadd(b, in(2), k));
   out(r);

   EXPECT_FALSE(run_pass());
}

/* The leaves are 16-bit, the arms 32-bit: the select takes the leaves' size. */
TEST_F(nir_opt_undistribute_select_test, leaf_bit_size)
{
   nir_def *x = in(1, 16);
   nir_def *y = in(1, 16);
   nir_intrinsic_instr *store =
      out(nir_bcsel(b, cond(),
                    nir_f2f32(b, x), nir_f2f32(b, y)));

   ASSERT_TRUE(run_pass());

   nir_alu_instr *cvt = alu(store->src[0].ssa);
   ASSERT_EQ(cvt->op, nir_op_f2f32);
   nir_alu_instr *sel = alu(cvt->src[0].src.ssa);
   ASSERT_EQ(sel->op, nir_op_bcsel);
   EXPECT_EQ(sel->def.bit_size, 16);
   EXPECT_EQ(sel->src[1].src.ssa, x);
}

/* A vec2 leaf under a scalar result has no per-component condition. */
TEST_F(nir_opt_undistribute_select_test, leaf_num_components)
{
   nir_def *k = in(2);
   nir_def *r = nir_bcsel(b, cond(),
                          nir_fdot2(b, in(2), k), nir_fdot2(b, in(2), k));
   out(r);

   EXPECT_FALSE(run_pass());
}

/* vec5/vec8/vec16 take more sources than any other ALU operation. */
TEST_F(nir_opt_undistribute_select_test, wide_vectors)
{
   for (unsigned n : {5, 8, 16}) {
      nir_def *x = in(n);
      nir_def *y = in(n);
      nir_scalar xs[16], ys[16];

      for (unsigned i = 0; i < n; i++) {
         xs[i] = nir_get_scalar(x, i);
         ys[i] = nir_get_scalar(y, i);
      }

      nir_intrinsic_instr *store =
         out(nir_bcsel(b, cond(),
                       nir_vec_scalars(b, xs, n), nir_vec_scalars(b, ys, n)));

      ASSERT_TRUE(run_pass());

      nir_alu_instr *vec = alu(store->src[0].ssa);
      ASSERT_EQ(vec->op, nir_op_vec(n));
      nir_alu_instr *sel = alu(vec->src[0].src.ssa);
      ASSERT_EQ(sel->op, nir_op_bcsel);
      EXPECT_EQ(sel->def.num_components, n);
      EXPECT_EQ(sel->src[1].src.ssa, x);
      EXPECT_EQ(sel->src[2].src.ssa, y);

      for (unsigned i = 0; i < n; i++) {
         EXPECT_EQ(vec->src[i].src.ssa, &sel->def);
         EXPECT_EQ(vec->src[i].swizzle[0], i);
      }
   }
}

/* nir_opt_algebraic pushes these into a select of constants. The pass must
 * leave that alone or the two take turns forever.
 */
TEST_F(nir_opt_undistribute_select_test, algebraic_fneg_of_constants)
{
   nir_def *c = cond();
   out(nir_fneg(b, nir_bcsel(b, c, nir_imm_float(b, 1.5),
                             nir_imm_float(b, 2.5))));

   EXPECT_TRUE(opt_loop());
}

TEST_F(nir_opt_undistribute_select_test, algebraic_fsat_of_constant_arm)
{
   nir_def *c = cond();
   out(nir_fsat(b, nir_bcsel(b, c, in(1), nir_imm_float(b, 0.5))));

   EXPECT_TRUE(opt_loop());
}

TEST_F(nir_opt_undistribute_select_test, algebraic_fmin_of_b2f)
{
   nir_def *c = cond();
   out(nir_fmin(b, nir_b2f32(b, c), nir_imm_float(b, 0.5)));

   EXPECT_TRUE(opt_loop());
}

TEST_F(nir_opt_undistribute_select_test, algebraic_compare_constant)
{
   nir_def *c = cond();
   out(nir_b2i32(b, nir_flt(b, nir_bcsel(b, c, in(1), nir_imm_float(b, 1.0)),
                                 nir_imm_float(b, 2.0))));

   EXPECT_TRUE(opt_loop());
}

TEST_F(nir_opt_undistribute_select_test, algebraic_ineg_of_constants)
{
   nir_def *c = cond();
   out(nir_ineg(b, nir_bcsel(b, c, nir_imm_int(b, 3), nir_imm_int(b, 5))));

   EXPECT_TRUE(opt_loop());
}

TEST_F(nir_opt_undistribute_select_test, algebraic_constant_divisor)
{
   nir_def *c = cond();
   out(nir_idiv(b, in(1), nir_bcsel(b, c, nir_imm_int(b, 3),
                                    nir_imm_int(b, 5))));

   EXPECT_TRUE(opt_loop());
   EXPECT_EQ(count(nir_op_idiv), 2);
}

} /* namespace */
