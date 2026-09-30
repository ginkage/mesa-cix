/*
 * Copyright 2011 Christoph Bumiller
 *           2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "nv50_ir.h"
#include "nv50_ir_build_util.h"

#include "nv50_ir_target_nvc0.h"
#include "nv50_ir_lowering_gm107.h"

#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

namespace nv50_ir {

#define QOP_ADD  0
#define QOP_SUBR 1
#define QOP_SUB  2
#define QOP_MOV2 3

//             UL UR LL LR
#define QUADOP(q, r, s, t)                      \
   ((QOP_##q << 6) | (QOP_##r << 4) |           \
    (QOP_##s << 2) | (QOP_##t << 0))

#define SHFL_BOUND_QUAD 0x1c03

void
GM107LegalizeSSA::handlePFETCH(Instruction *i)
{
   Value *src0;

   if (i->src(0).getFile() == FILE_GPR && !i->srcExists(1))
      return;

   bld.setPosition(i, false);
   src0 = bld.getSSA();

   if (i->srcExists(1))
      bld.mkOp2(OP_ADD , TYPE_U32, src0, i->getSrc(0), i->getSrc(1));
   else
      bld.mkOp1(OP_MOV , TYPE_U32, src0, i->getSrc(0));

   i->setSrc(0, src0);
   i->setSrc(1, NULL);
}

void
GM107LegalizeSSA::handleLOAD(Instruction *i)
{
   if (i->src(0).getFile() != FILE_MEMORY_CONST)
      return;
   if (i->src(0).isIndirect(0))
      return;
   if (typeSizeof(i->dType) != 4)
      return;

   i->op = OP_MOV;
}

void
GM107LegalizeSSA::handleQUADON(Instruction *i)
{
   i->setDef(0, NULL);
}

void
GM107LegalizeSSA::handleQUADPOP(Instruction *i)
{
   i->setSrc(0, NULL);
}

bool
GM107LegalizeSSA::visit(Instruction *i)
{
   switch (i->op) {
   case OP_QUADON:
      handleQUADON(i);
      break;
   case OP_QUADPOP:
      handleQUADPOP(i);
      break;
   case OP_PFETCH:
      handlePFETCH(i);
      break;
   case OP_LOAD:
      handleLOAD(i);
      break;
   default:
      break;
   }
   return true;
}

static bool
isDualIssueSecond(const Instruction *insn)
{
   switch (insn->op) {
   case OP_LINTERP:
   case OP_PINTERP:
   case OP_RCP:
   case OP_RSQ:
   case OP_LG2:
   case OP_EX2:
   case OP_SIN:
   case OP_COS:
      return typeSizeof(insn->dType) <= 4 && !insn->defExists(1);
   default:
      return false;
   }
}

static bool
isDualIssueFirst(const Instruction *insn)
{
   const OpClass cl = Target::operationClass[insn->op];
   if (cl != OPCLASS_ARITH && cl != OPCLASS_COMPARE && cl != OPCLASS_MOVE)
      return false;
   if (insn->fixed || typeSizeof(insn->dType) > 4 || !insn->defExists(0) ||
       insn->defExists(1) || insn->getDef(0)->reg.file != FILE_GPR)
      return false;
   return true;
}

// Pre-RA list scheduler for Maxwell, see GM107PreRAScheduling
namespace {

struct SchedNode {
   Instruction *insn;
   std::vector<std::pair<int, int> > succs; // node, latency
   int npreds = 0;
   int height = 0;
   int earliest = 0;
   int order;
   bool done = false;
   bool varLat = false;
};

static bool
schedPureValue(const Value *v)
{
   switch (v->reg.file) {
   case FILE_GPR:
   case FILE_PREDICATE:
      return v->join->reg.data.id < 0;
   case FILE_IMMEDIATE:
   case FILE_MEMORY_CONST:
   case FILE_SHADER_INPUT:
      return true;
   default:
      return false;
   }
}

static bool
isSchedulable(const Instruction *i)
{
   if (i->fixed || i->join || i->asFlow() || i->perPatch)
      return false;
   switch (Target::operationClass[i->op]) {
   case OPCLASS_MOVE:
      if (i->op != OP_MOV)
         return false;
      break;
   case OPCLASS_ARITH:
   case OPCLASS_SHIFT:
   case OPCLASS_LOGIC:
   case OPCLASS_COMPARE:
   case OPCLASS_CONVERT:
   case OPCLASS_SFU:
   case OPCLASS_BITFIELD:
      break;
   case OPCLASS_TEXTURE:
      if (i->op == OP_TXD || i->op == OP_TEXPREP || i->op == OP_TEXCSAA)
         return false;
      break;
   case OPCLASS_PSEUDO:
      if (i->op != OP_SPLIT && i->op != OP_MERGE)
         return false;
      break;
   case OPCLASS_LOAD:
      if (i->op != OP_LOAD || i->src(0).getFile() != FILE_MEMORY_CONST)
         return false;
      break;
   default:
      return false;
   }
   for (int d = 0; i->defExists(d); ++d)
      if (!schedPureValue(i->getDef(d)) ||
          i->getDef(d)->reg.file == FILE_IMMEDIATE ||
          i->getDef(d)->defs.size() != 1)
         return false;
   for (int s = 0; i->srcExists(s); ++s)
      if (!schedPureValue(i->getSrc(s)))
         return false;
   return true;
}

static int
schedLatency(const Instruction *i, bool &varLat)
{
   varLat = true;
   switch (Target::operationClass[i->op]) {
   case OPCLASS_TEXTURE:
      return 100;
   case OPCLASS_LOAD:
      return 30;
   case OPCLASS_PSEUDO:
      varLat = false;
      return 0;
   default:
      break;
   }
   switch (i->op) {
   case OP_LINTERP:
   case OP_PINTERP:
      return 16;
   case OP_RCP:
   case OP_RSQ:
   case OP_LG2:
   case OP_SIN:
   case OP_COS:
   case OP_EX2:
   case OP_SQRT:
      return 16;
   default:
      break;
   }
   varLat = i->dType == TYPE_F64 || i->sType == TYPE_F64;
   return varLat ? 20 : 6;
}

static int
gprSize(const Value *v)
{
   return v->reg.file == FILE_GPR ? (v->reg.size + 3) / 4 : 0;
}

class GM107PreRASched
{
public:
   GM107PreRASched(BasicBlock *bb) : bb(bb) { }
   void run();
private:
   void schedRegion(std::vector<Instruction *> &, Instruction *before);
   BasicBlock *bb;
};

void
GM107PreRASched::run()
{
   std::vector<Instruction *> region;
   for (Instruction *i = bb->getEntry(), *next; i; i = next) {
      next = i->next;
      if (isSchedulable(i)) {
         region.push_back(i);
         continue;
      }
      schedRegion(region, i);
      region.clear();
   }
   schedRegion(region, NULL);
}

void
GM107PreRASched::schedRegion(std::vector<Instruction *> &insns, Instruction *before)
{
   // below this, reordering costs more than it gains (particle shaders)
   const int n = insns.size();
   if (n < 24)
      return;

   std::unordered_map<const Instruction *, int> idx;
   for (int k = 0; k < n; ++k)
      idx[insns[k]] = k;

   std::vector<SchedNode> nodes(n);
   std::vector<int> lat(n);
   for (int k = 0; k < n; ++k) {
      nodes[k].insn = insns[k];
      nodes[k].order = k;
      lat[k] = schedLatency(insns[k], nodes[k].varLat);
   }
   for (int k = 0; k < n; ++k) {
      Instruction *i = insns[k];
      std::set<int> preds;
      for (int s = 0; i->srcExists(s); ++s) {
         Instruction *d = i->getSrc(s)->getUniqueInsn();
         auto it = d ? idx.find(d) : idx.end();
         if (it != idx.end() && it->second != k)
            preds.insert(it->second);
      }
      for (int p : preds) {
         nodes[p].succs.push_back(std::make_pair(k, lat[p]));
         nodes[k].npreds++;
      }
   }
   for (int k = n - 1; k >= 0; --k) {
      int h = 0;
      for (auto &e : nodes[k].succs)
         h = MAX2(h, nodes[e.first].height);
      nodes[k].height = h + lat[k];
   }

   // register pressure, relative to the start of the region
   std::set<const Instruction *> earlier;
   for (const Instruction *i = insns[0]->prev; i; i = i->prev)
      earlier.insert(i);
   std::unordered_map<const Value *, int> remUses;
   std::set<const Value *> liveOut;
   for (int k = 0; k < n; ++k) {
      Instruction *i = insns[k];
      for (int s = 0; i->srcExists(s); ++s) {
         const Value *v = i->getSrc(s);
         if (v->reg.file != FILE_GPR || remUses.count(v))
            continue;
         int in = 0;
         bool out = false;
         for (const ValueRef *u : v->uses) {
            const Instruction *ui = u->getInsn();
            if (idx.count(ui))
               ++in;
            else if (!earlier.count(ui))
               out = true;
         }
         remUses[v] = in;
         if (out)
            liveOut.insert(v);
      }
      for (int d = 0; i->defExists(d); ++d) {
         const Value *v = i->getDef(d);
         if (v->reg.file != FILE_GPR || remUses.count(v))
            continue;
         int in = 0;
         bool out = false;
         for (const ValueRef *u : v->uses) {
            if (idx.count(u->getInsn()))
               ++in;
            else
               out = true;
         }
         remUses[v] = in;
         if (out || !in)
            liveOut.insert(v);
      }
   }
   auto delta = [&](int k, std::unordered_map<const Value *, int> &rem) {
      const Instruction *i = insns[k];
      int dp = 0;
      for (int d = 0; i->defExists(d); ++d)
         if (!i->getDef(d)->uses.empty() || liveOut.count(i->getDef(d)))
            dp += gprSize(i->getDef(d));
      std::set<const Value *> seen;
      for (int s = 0; i->srcExists(s); ++s) {
         const Value *v = i->getSrc(s);
         if (v->reg.file != FILE_GPR || !seen.insert(v).second)
            continue;
         int uses = 0;
         for (int t = 0; i->srcExists(t); ++t)
            uses += i->getSrc(t) == v;
         if (rem[v] == uses && !liveOut.count(v))
            dp -= gprSize(v);
      }
      return dp;
   };
   auto commit = [&](int k, std::unordered_map<const Value *, int> &rem) {
      const Instruction *i = insns[k];
      for (int s = 0; i->srcExists(s); ++s)
         if (i->getSrc(s)->reg.file == FILE_GPR)
            rem[i->getSrc(s)]--;
   };

   int limit, soft, press = 0, peak = 0;
   {
      std::unordered_map<const Value *, int> rem = remUses;
      int peak = 0;
      for (int k = 0; k < n; ++k) {
         press += delta(k, rem);
         commit(k, rem);
         peak = MAX2(peak, press);
      }
      limit = peak;
      press = 0;
   }

   const int bars = 6, sfuTp = 2; // scoreboards, cycles per MUFU op
   // a scoreboard is held from issue to the first consumer in the region
   std::vector<bool> needsBar(n);
   for (int k = 0; k < n; ++k)
      needsBar[k] = nodes[k].varLat && !nodes[k].succs.empty();
   std::vector<int> outstanding; // var-lat producers nobody waited for yet
   std::vector<int> order;
   int cycle, sfuFree;
   auto isSfu = [&](int k) {
      return Target::operationClass[insns[k]->op] == OPCLASS_SFU && nodes[k].varLat;
   };
   auto ready = [&](int k) { // earliest issue cycle, -1 if blocked
      if (needsBar[k] && (int)outstanding.size() >= bars)
         return -1;
      int t = nodes[k].earliest;
      if (isSfu(k))
         t = MAX2(t, sfuFree);
      return t;
   };
   auto better = [&](int a, int b) { // is a a better pick than b
      if (b < 0)
         return true;
      const int da = delta(a, remUses), db = delta(b, remUses);
      const bool ova = press + da > soft, ovb = press + db > soft;
      if (ova != ovb)
         return !ova;
      if (ova && da != db)
         return da < db;
      if (nodes[a].height != nodes[b].height)
         return nodes[a].height > nodes[b].height;
      return nodes[a].order < nodes[b].order;
   };
   auto issue = [&](int k, int at) {
      SchedNode &nd = nodes[k];
      nd.done = true;
      order.push_back(k);
      press += delta(k, remUses);
      peak = MAX2(peak, press);
      commit(k, remUses);
      Instruction *i = insns[k];
      for (int s = 0; i->srcExists(s); ++s) {
         Instruction *d = i->getSrc(s)->getUniqueInsn();
         auto it = d ? idx.find(d) : idx.end();
         if (it == idx.end())
            continue;
         for (size_t o = 0; o < outstanding.size(); ++o)
            if (outstanding[o] == it->second) {
               outstanding.erase(outstanding.begin() + o);
               break;
            }
      }
      if (needsBar[k])
         outstanding.push_back(k);
      if (isSfu(k))
         sfuFree = at + sfuTp;
      for (auto &e : nd.succs) {
         nodes[e.first].earliest = MAX2(nodes[e.first].earliest, at + e.second);
         nodes[e.first].npreds--;
      }
   };

   const std::unordered_map<const Value *, int> remStart = remUses;
   std::vector<int> npreds(n);
   for (int k = 0; k < n; ++k)
      npreds[k] = nodes[k].npreds;

   // Greedy choices can corner themselves: every remaining candidate raises
   // the pressure. Aim lower and retry, else keep the original order.
   for (int slack = 0;; slack = slack ? slack * 2 : 1) {
      soft = limit - slack;
      for (int k = 0; k < n; ++k) {
         nodes[k].done = false;
         nodes[k].earliest = 0;
         nodes[k].npreds = npreds[k];
      }
      remUses = remStart;
      outstanding.clear();
      order.clear();
      press = peak = cycle = sfuFree = 0;
      while ((int)order.size() < n) {
         int best = -1, wait = -1, waitAt = 0, fit = -1, fitAt = 0, any = -1;
         for (int k = 0; k < n; ++k) {
            if (nodes[k].done || nodes[k].npreds)
               continue;
            any = k;
            const int t = ready(k);
            if (t < 0)
               continue;
            if (t <= cycle) {
               if (better(k, best))
                  best = k;
            } else {
               if (wait < 0 || t < waitAt || (t == waitAt && better(k, wait))) {
                  wait = k;
                  waitAt = t;
               }
               const int d = delta(k, remUses);
               if ((d <= 0 || press + d <= soft) &&
                   (fit < 0 || t < fitAt || (t == fitAt && better(k, fit)))) {
                  fit = k;
                  fitAt = t;
               }
            }
         }
         if (best < 0 && wait >= 0) {
            best = wait;
            cycle = waitAt;
         }
         // rather than raise the pressure above the limit, stall for a
         // candidate that stays within it or does not raise it
         const int db = best >= 0 ? delta(best, remUses) : 0;
         if (fit >= 0 && db > 0 && press + db > soft) {
            best = fit;
            cycle = fitAt;
         }
         if (best < 0) { // every candidate needs a scoreboard: share one
            best = any;
            for (int k = 0; k < n; ++k)
               if (!nodes[k].done && !nodes[k].npreds && better(k, best))
                  best = k;
            cycle = MAX2(cycle, nodes[best].earliest);
         }
         issue(best, cycle);
         if (isDualIssueFirst(insns[best])) {
            int sec = -1;
            for (int k = 0; k < n; ++k) {
               if (nodes[k].done || nodes[k].npreds ||
                   !isDualIssueSecond(insns[k]))
                  continue;
               const int t = ready(k);
               if (t < 0 || t > cycle)
                  continue;
               if (better(k, sec))
                  sec = k;
            }
            if (sec >= 0 && (delta(sec, remUses) <= 0 ||
                             press + delta(sec, remUses) <= soft))
               issue(sec, cycle);
         }
         cycle++;
      }
      if (peak <= limit)
         break;
      if (soft <= 0)
         return;
   }

   for (int k = 0; k < n; ++k)
      bb->remove(insns[k]);
   for (int k : order) {
      if (before)
         bb->insertBefore(before, insns[k]);
      else
         bb->insertTail(insns[k]);
   }
}

} // anonymous namespace

bool
GM107PreRAScheduling::visit(BasicBlock *bb)
{
   GM107PreRASched(bb).run();
   return true;
}

bool
GM107LoweringPass::handleManualTXD(TexInstruction *i)
{
   // See NVC0LoweringPass::handleManualTXD for rationale. This function
   // implements the same logic, but using SM50-friendly primitives.
   static const uint8_t qOps[2] =
      { QUADOP(MOV2, ADD,  MOV2, ADD),  QUADOP(MOV2, MOV2, ADD,  ADD) };
   Value *def[4][4];
   Value *crd[3], *arr, *shadow;
   Value *tmp;
   Instruction *tex, *add;
   Value *quad = bld.mkImm(SHFL_BOUND_QUAD);
   int l, c;
   const int dim = i->tex.target.getDim() + i->tex.target.isCube();
   const int array = i->tex.target.isArray();
   const int indirect = i->tex.rIndirectSrc >= 0;

   i->op = OP_TEX; // no need to clone dPdx/dPdy later
   i->tex.liveOnly = false; // the quad's helper lanes carry the derivatives

   for (c = 0; c < dim; ++c)
      crd[c] = bld.getScratch();
   arr = bld.getScratch();
   shadow = bld.getScratch();
   tmp = bld.getScratch();

   for (l = 0; l < 4; ++l) {
      Value *bar = bld.getSSA(4, FILE_BARRIER);
      Value *src[3], *val;
      Value *lane = bld.mkImm(l);
      bld.mkOp(OP_QUADON, TYPE_U32, bar);
      // Make sure lane 0 has the appropriate array/depth compare values
      if (l != 0) {
         if (array)
            bld.mkOp3(OP_SHFL, TYPE_F32, arr, i->getSrc(0), lane, quad);
         if (i->tex.target.isShadow())
            bld.mkOp3(OP_SHFL, TYPE_F32, shadow, i->getSrc(array + dim + indirect), lane, quad);
      }

      // mov coordinates from lane l to all lanes
      for (c = 0; c < dim; ++c) {
         bld.mkOp3(OP_SHFL, TYPE_F32, crd[c], i->getSrc(c + array), lane, quad);
      }

      // add dPdx from lane l to lanes dx
      for (c = 0; c < dim; ++c) {
         bld.mkOp3(OP_SHFL, TYPE_F32, tmp, i->dPdx[c].get(), lane, quad);
         add = bld.mkOp2(OP_QUADOP, TYPE_F32, crd[c], tmp, crd[c]);
         add->subOp = qOps[0];
         add->lanes = 1; /* abused for .ndv */
      }

      // add dPdy from lane l to lanes dy
      for (c = 0; c < dim; ++c) {
         bld.mkOp3(OP_SHFL, TYPE_F32, tmp, i->dPdy[c].get(), lane, quad);
         add = bld.mkOp2(OP_QUADOP, TYPE_F32, crd[c], tmp, crd[c]);
         add->subOp = qOps[1];
         add->lanes = 1; /* abused for .ndv */
      }

      // normalize cube coordinates if necessary
      if (i->tex.target.isCube()) {
         for (c = 0; c < 3; ++c)
            src[c] = bld.mkOp1v(OP_ABS, TYPE_F32, bld.getSSA(), crd[c]);
         val = bld.getScratch();
         bld.mkOp2(OP_MAX, TYPE_F32, val, src[0], src[1]);
         bld.mkOp2(OP_MAX, TYPE_F32, val, src[2], val);
         bld.mkOp1(OP_RCP, TYPE_F32, val, val);
         for (c = 0; c < 3; ++c)
            src[c] = bld.mkOp2v(OP_MUL, TYPE_F32, bld.getSSA(), crd[c], val);
      } else {
         for (c = 0; c < dim; ++c)
            src[c] = crd[c];
      }

      // texture
      bld.insert(tex = cloneForward(func, i));
      if (l != 0) {
         if (array)
            tex->setSrc(0, arr);
         if (i->tex.target.isShadow())
            tex->setSrc(array + dim + indirect, shadow);
      }
      for (c = 0; c < dim; ++c)
         tex->setSrc(c + array, src[c]);
      // broadcast results from lane 0 to all lanes
      if (l != 0)
         for (c = 0; i->defExists(c); ++c)
            bld.mkOp3(OP_SHFL, TYPE_F32, tex->getDef(c), tex->getDef(c), bld.mkImm(0), quad);
      bld.mkOp1(OP_QUADPOP, TYPE_U32, NULL, bar)->fixed = 1;

      // save results
      for (c = 0; i->defExists(c); ++c) {
         Instruction *mov;
         def[c][l] = bld.getSSA();
         mov = bld.mkMov(def[c][l], tex->getDef(c));
         mov->fixed = 1;
         mov->lanes = 1 << l;
      }
   }

   for (c = 0; i->defExists(c); ++c) {
      Instruction *u = bld.mkOp(OP_UNION, TYPE_U32, i->getDef(c));
      for (l = 0; l < 4; ++l)
         u->setSrc(l, def[c][l]);
   }

   i->bb->remove(i);
   return true;
}

bool
GM107LoweringPass::handleDFDX(Instruction *insn)
{
   Instruction *shfl;
   int qop = 0, xid = 0;

   switch (insn->op) {
   case OP_DFDX:
      qop = QUADOP(SUB, SUBR, SUB, SUBR);
      xid = 1;
      break;
   case OP_DFDY:
      qop = QUADOP(SUB, SUB, SUBR, SUBR);
      xid = 2;
      break;
   default:
      assert(!"invalid dfdx opcode");
      break;
   }

   shfl = bld.mkOp3(OP_SHFL, TYPE_F32, bld.getScratch(), insn->getSrc(0),
                    bld.mkImm(xid), bld.mkImm(SHFL_BOUND_QUAD));
   shfl->subOp = NV50_IR_SUBOP_SHFL_BFLY;
   insn->op = OP_QUADOP;
   insn->subOp = qop;
   insn->lanes = 0; /* abused for !.ndv */
   insn->setSrc(1, insn->getSrc(0));
   insn->setSrc(0, shfl->getDef(0));
   return true;
}

bool
GM107LoweringPass::handlePFETCH(Instruction *i)
{
   Value *tmp0 = bld.getScratch();
   Value *tmp1 = bld.getScratch();
   Value *tmp2 = bld.getScratch();
   bld.mkOp1(OP_RDSV, TYPE_U32, tmp0, bld.mkSysVal(SV_INVOCATION_INFO, 0));
   bld.mkOp3(OP_PERMT, TYPE_U32, tmp1, tmp0, bld.mkImm(0x4442), bld.mkImm(0));
   bld.mkOp3(OP_PERMT, TYPE_U32, tmp0, tmp0, bld.mkImm(0x4440), bld.mkImm(0));
   if (i->getSrc(1))
      bld.mkOp2(OP_ADD , TYPE_U32, tmp2, i->getSrc(0), i->getSrc(1));
   else
      bld.mkOp1(OP_MOV , TYPE_U32, tmp2, i->getSrc(0));
   bld.mkOp3(OP_MAD , TYPE_U32, tmp0, tmp0, tmp1, tmp2);
   i->setSrc(0, tmp0);
   i->setSrc(1, NULL);
   return true;
}

bool
GM107LoweringPass::handlePOPCNT(Instruction *i)
{
   Value *tmp = bld.mkOp2v(OP_AND, i->sType, bld.getScratch(),
                           i->getSrc(0), i->getSrc(1));
   i->setSrc(0, tmp);
   i->setSrc(1, NULL);
   return true;
}

bool
GM107LoweringPass::handleSUQ(TexInstruction *suq)
{
   Value *ind = suq->getIndirectR();
   Value *handle;
   const int slot = suq->tex.r;
   const int mask = suq->tex.mask;

   if (suq->tex.bindless)
      handle = ind;
   else
      handle = loadTexHandle(ind, slot + 32);

   suq->tex.r = 0xff;
   suq->tex.s = 0x1f;

   suq->setIndirectR(NULL);
   suq->setSrc(0, handle);
   suq->tex.rIndirectSrc = 0;
   suq->setSrc(1, bld.loadImm(NULL, 0));
   suq->tex.query = TXQ_DIMS;
   suq->op = OP_TXQ;

   // We store CUBE / CUBE_ARRAY as a 2D ARRAY. Make sure that depth gets
   // divided by 6.
   if (mask & 0x4 && suq->tex.target.isCube()) {
      int d = util_bitcount(mask & 0x3);
      bld.setPosition(suq, true);
      bld.mkOp2(OP_DIV, TYPE_U32, suq->getDef(d), suq->getDef(d),
                bld.loadImm(NULL, 6));
   }

   // Samples come from a different query. If we want both samples and dims,
   // create a second suq.
   if (mask & 0x8) {
      int d = util_bitcount(mask & 0x7);
      Value *dst = suq->getDef(d);
      TexInstruction *samples = suq;
      assert(dst);

      if (mask != 0x8) {
         suq->setDef(d, NULL);
         suq->tex.mask &= 0x7;
         samples = cloneShallow(func, suq);
         for (int i = 0; i < d; i++)
            samples->setDef(d, NULL);
         samples->setDef(0, dst);
         suq->bb->insertAfter(suq, samples);
      }
      samples->tex.mask = 0x4;
      samples->tex.query = TXQ_TYPE;
   }

   if (suq->tex.target.isMS()) {
      bld.setPosition(suq, true);

      if (mask & 0x1)
         bld.mkOp2(OP_SHR, TYPE_U32, suq->getDef(0), suq->getDef(0),
                   loadMsAdjInfo32(suq->tex.target, 0, slot, ind, suq->tex.bindless));
      if (mask & 0x2) {
         int d = util_bitcount(mask & 0x1);
         bld.mkOp2(OP_SHR, TYPE_U32, suq->getDef(d), suq->getDef(d),
                   loadMsAdjInfo32(suq->tex.target, 1, slot, ind, suq->tex.bindless));
      }
   }

   return true;
}

// Interpolate without w and multiply by 1/w after, like the blob: the IPA
// then need not wait for the RCP.
void
GM107LoweringPass::handlePINTERP(Instruction *i)
{
   if (prog->getTarget()->getChipset() >= NVISA_GV100_CHIPSET ||
       i->getInterpMode() != NV50_IR_INTERP_PERSPECTIVE ||
       i->getSampleMode() == NV50_IR_INTERP_OFFSET ||
       i->src(0).isIndirect(0) || typeSizeof(i->dType) != 4 ||
       i->srcExists(2))
      return;

   Value *w = i->getSrc(1), *def = i->getDef(0), *t = bld.getSSA();
   i->op = OP_LINTERP;
   i->setInterpolate((i->ipa & ~NV50_IR_INTERP_MODE_MASK) | NV50_IR_INTERP_LINEAR);
   i->setSrc(1, NULL);
   i->setDef(0, t);
   bld.setPosition(i, true);
   bld.mkOp2(OP_MUL, TYPE_F32, def, t, w);
   // issue the interpolation before the reciprocal it no longer waits for
   Instruction *rcp = i->prev;
   while (rcp && !(rcp->op == OP_RCP && rcp->getDef(0) == w))
      rcp = rcp->prev;
   if (rcp) {
      i->bb->remove(i);
      rcp->bb->insertBefore(rcp, i);
   }
}

//
// - add quadop dance for texturing
// - put FP outputs in GPRs
// - convert instruction sequences
//
bool
GM107LoweringPass::visit(Instruction *i)
{
   bld.setPosition(i, false);

   if (i->cc != CC_ALWAYS)
      checkPredicate(i);

   switch (i->op) {
   case OP_PFETCH:
      return handlePFETCH(i);
   case OP_DFDX:
   case OP_DFDY:
      return handleDFDX(i);
   case OP_POPCNT:
      return handlePOPCNT(i);
   case OP_SUQ:
      return handleSUQ(i->asTex());
   case OP_PINTERP:
      handlePINTERP(i);
      return NVC0LoweringPass::visit(i);
   default:
      return NVC0LoweringPass::visit(i);
   }
}

} // namespace nv50_ir
