/*
 * Copyright © 2026 mojyack
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include "nv50_ir.h"
#include "nv50_ir_build_util.h"
#include "nv50_ir_target_gm107.h"
#include "nv50_ir_sched_gm107.h"

using namespace nv50_ir;

namespace {

class SchedGM107Test : public ::testing::TestWithParam<unsigned> {
protected:
   void SetUp() override
   {
      targ = Target::create(GetParam());
      prog = new Program(Program::TYPE_FRAGMENT, targ);
      func = prog->main;
   }

   void TearDown() override
   {
      delete prog;
      Target::destroy(targ);
   }

   LValue *reg(DataFile file, int id)
   {
      LValue *v = new_LValue(func, file);
      v->reg.data.id = id;
      return v;
   }
   LValue *r(int id) { return reg(FILE_GPR, id); }

   /* stall cycles from the issue of i to the end of its block */
   static int toExit(const Instruction *i)
   {
      int c = 0;
      for (; i; i = i->next)
         c += i->sched & 0xf;
      return c;
   }

   /* stall cycles from the entry of i's block to the issue of i */
   static int fromEntry(const Instruction *i)
   {
      int c = 0;
      for (const Instruction *p = i->bb->getEntry(); p != i; p = p->next)
         c += p->sched & 0xf;
      return c;
   }

   Target *targ;
   Program *prog;
   Function *func;
};

} /* namespace */

// b2 starts with an add of $r0 once paired, written by a xor in both predecessors
TEST_P(SchedGM107Test, PairedEntryWaitsForPredecessors)
{
   BuildUtil bld(prog);
   BasicBlock *b0 = new BasicBlock(func);
   BasicBlock *b1 = new BasicBlock(func);
   BasicBlock *b2 = new BasicBlock(func);

   func->setEntry(b0);
   func->setExit(b2);
   b0->cfg.attach(&b1->cfg, Graph::Edge::TREE);
   b0->cfg.attach(&b2->cfg, Graph::Edge::TREE);
   b1->cfg.attach(&b2->cfg, Graph::Edge::FORWARD);

   bld.setPosition(b0, true);
   Instruction *xor0 = bld.mkOp2(OP_XOR, TYPE_U32, r(0), r(1), r(2));
   bld.mkFlow(OP_BRA, b2, CC_P, reg(FILE_PREDICATE, 0));
   bld.setPosition(b1, true);
   Instruction *xor1 = bld.mkOp2(OP_XOR, TYPE_U32, r(0), r(3), r(4));
   bld.setPosition(b2, true);
   Instruction *rcp = bld.mkOp1(OP_RCP, TYPE_F32, r(5), r(8));
   Instruction *add = bld.mkOp2(OP_ADD, TYPE_F32, r(6), r(0), r(9));
   bld.mkFlow(OP_EXIT, NULL, CC_ALWAYS, NULL);

   SchedDataCalculatorGM107 sched(static_cast<TargetGM107 *>(targ));
   ASSERT_TRUE(sched.run(func, true, true));

   if (GetParam() < 0x140) {
      EXPECT_EQ(b2->getEntry(), add);
      EXPECT_EQ(add->next, rcp);
   } else {
      /* no dual issue on Volta */
      EXPECT_EQ(b2->getEntry(), rcp);
   }

   const int lat = targ->getLatency(xor0);
   EXPECT_GE(toExit(xor0) + fromEntry(add), lat);
   EXPECT_GE(toExit(xor1) + fromEntry(add), lat);
}

INSTANTIATE_TEST_SUITE_P(Chipsets, SchedGM107Test,
                         ::testing::Values(0x12b, 0x130, 0x140));
