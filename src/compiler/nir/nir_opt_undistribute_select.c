/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"

/**
 * Undistribute a select over two isomorphic expression trees:
 *
 *    bcsel(c, f(..., x, ...), f(..., y, ...))  ->  f(..., bcsel(c, x, y), ...)
 */

#define MAX_DEPTH 8

static bool
srcs_swizzles_equal(const nir_alu_instr *a, const nir_alu_instr *b, unsigned i)
{
   unsigned n = nir_ssa_alu_instr_src_components(a, i);

   if (n != nir_ssa_alu_instr_src_components(b, i))
      return false;

   for (unsigned c = 0; c < n; c++) {
      if (a->src[i].swizzle[c] != b->src[i].swizzle[c])
         return false;
   }

   return true;
}

/* Same operation applied the same way, and both defs die with the tree. */
static bool
can_descend(nir_def *a, nir_def *b)
{
   if (nir_def_instr(a)->type != nir_instr_type_alu ||
       nir_def_instr(b)->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *x = nir_instr_as_alu(nir_def_instr(a));
   nir_alu_instr *y = nir_instr_as_alu(nir_def_instr(b));

   if (x->op != y->op ||
       a->bit_size != b->bit_size ||
       a->num_components != b->num_components ||
       x->fp_math_ctrl != y->fp_math_ctrl ||
       x->no_signed_wrap != y->no_signed_wrap ||
       x->no_unsigned_wrap != y->no_unsigned_wrap)
      return false;

   if (nir_op_infos[x->op].num_inputs == 0)
      return false;

   /* nir_opt_algebraic moves a division into a select of constant divisors. */
   if ((x->op == nir_op_idiv || x->op == nir_op_udiv ||
        x->op == nir_op_umod || x->op == nir_op_irem) &&
       x->src[1].src.ssa != y->src[1].src.ssa &&
       nir_src_is_const(x->src[1].src) && nir_src_is_const(y->src[1].src))
      return false;

   if (!list_is_singular(&a->uses) || !list_is_singular(&b->uses))
      return false;

   for (unsigned i = 0; i < nir_op_infos[x->op].num_inputs; i++) {
      if (!srcs_swizzles_equal(x, y, i))
         return false;
   }

   return true;
}

struct leaf {
   nir_def *a, *b;
};

static bool
trees_match(nir_def *a, nir_def *b, unsigned depth, struct leaf *l)
{
   if (a == b)
      return true;

   if (depth && can_descend(a, b)) {
      nir_alu_instr *x = nir_instr_as_alu(nir_def_instr(a));
      nir_alu_instr *y = nir_instr_as_alu(nir_def_instr(b));

      for (unsigned i = 0; i < nir_op_infos[x->op].num_inputs; i++) {
         if (!trees_match(x->src[i].src.ssa, y->src[i].src.ssa, depth - 1, l))
            return false;
      }

      return true;
   }

   if (a->bit_size != b->bit_size || a->num_components != b->num_components)
      return false;

   /* One select covers a repeat of the same pair, but not a second pair. */
   if (l->a)
      return l->a == a && l->b == b;

   l->a = a;
   l->b = b;

   return true;
}

static nir_def *
rebuild(nir_builder *bld, nir_def *a, nir_def *b, unsigned depth,
        const struct leaf *l, nir_def *sel)
{
   if (a == b)
      return a;
   if (a == l->a)
      return sel;

   assert(depth && can_descend(a, b));

   nir_alu_instr *x = nir_instr_as_alu(nir_def_instr(a));
   nir_alu_instr *y = nir_instr_as_alu(nir_def_instr(b));
   unsigned n = nir_op_infos[x->op].num_inputs;
   nir_def *src[NIR_ALU_MAX_INPUTS];

   for (unsigned i = 0; i < n; i++) {
      src[i] = rebuild(bld, x->src[i].src.ssa, y->src[i].src.ssa, depth - 1,
                       l, sel);
   }

   nir_alu_instr *nw = nir_alu_instr_create(bld->shader, x->op);

   for (unsigned i = 0; i < n; i++) {
      memcpy(nw->src[i].swizzle, x->src[i].swizzle, sizeof(nw->src[i].swizzle));
      nw->src[i].src = nir_src_for_ssa(src[i]);
   }

   nw->fp_math_ctrl = x->fp_math_ctrl;
   nw->no_signed_wrap = x->no_signed_wrap;
   nw->no_unsigned_wrap = x->no_unsigned_wrap;

   nir_def_init(&nw->instr, &nw->def, a->num_components, a->bit_size);
   nir_builder_instr_insert(bld, &nw->instr);

   return &nw->def;
}

static bool
opt_undistribute_select(nir_builder *bld, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_bcsel && alu->op != nir_op_b32csel)
      return false;

   /* A swizzle on an arm would have to be pushed through the rebuilt tree. */
   if (!nir_alu_src_is_trivial_ssa(&alu->src[1], alu->def.num_components) ||
       !nir_alu_src_is_trivial_ssa(&alu->src[2], alu->def.num_components))
      return false;

   /* A swizzle inside the tree would move a component to another lane's
    * condition, so the condition has to be the same for every lane. */
   for (unsigned c = 1; c < alu->def.num_components; c++) {
      if (alu->src[0].swizzle[c] != alu->src[0].swizzle[0])
         return false;
   }

   nir_def *a = alu->src[1].src.ssa;
   nir_def *b = alu->src[2].src.ssa;
   struct leaf l = { NULL, NULL };

   if (!trees_match(a, b, MAX_DEPTH, &l))
      return false;

   bld->cursor = nir_before_instr(&alu->instr);

   /* The arms are the same expression. CSE normally gets this one. */
   if (!l.a) {
      nir_def_rewrite_uses(&alu->def, a);
      nir_instr_remove(&alu->instr);
      return true;
   }

   /* Nothing above the leaf to hoist: this is the select we started with. */
   if (l.a == a)
      return false;

   /* The condition is per component, so the leaf has to have the result's shape. */
   if (l.a->num_components != alu->def.num_components)
      return false;

   nir_alu_instr *sel = nir_alu_instr_create(bld->shader, alu->op);

   memcpy(sel->src[0].swizzle, alu->src[0].swizzle, sizeof(sel->src[0].swizzle));
   sel->src[0].src = nir_src_for_ssa(alu->src[0].src.ssa);
   sel->src[1].src = nir_src_for_ssa(l.a);
   sel->src[2].src = nir_src_for_ssa(l.b);

   nir_def_init(&sel->instr, &sel->def, l.a->num_components, l.a->bit_size);
   nir_builder_instr_insert(bld, &sel->instr);

   nir_def_rewrite_uses(&alu->def,
                        rebuild(bld, a, b, MAX_DEPTH, &l, &sel->def));
   nir_instr_remove(&alu->instr);

   return true;
}

bool
nir_opt_undistribute_select(nir_shader *shader)
{
   return nir_shader_alu_pass(shader, opt_undistribute_select,
                              nir_metadata_control_flow, NULL);
}
