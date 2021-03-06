/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "nir.h"

/*
 * This pass lowers the neg, abs, and sat operations to source modifiers on
 * ALU operations to make things nicer for the backend.  It's just much
 * easier to not have them when we're doing optimizations.
 */

static bool
nir_lower_to_source_mods_block(nir_block *block, void *state)
{
   nir_foreach_instr(block, instr) {
      if (instr->type != nir_instr_type_alu)
         continue;

      nir_alu_instr *alu = nir_instr_as_alu(instr);

      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         if (!alu->src[i].src.is_ssa)
            continue;

         if (alu->src[i].src.ssa->parent_instr->type != nir_instr_type_alu)
            continue;

         nir_alu_instr *parent = nir_instr_as_alu(alu->src[i].src.ssa->parent_instr);

         if (parent->dest.saturate)
            continue;

         switch (nir_op_infos[alu->op].input_types[i]) {
         case nir_type_float:
            if (parent->op != nir_op_fmov)
               continue;
            break;
         case nir_type_int:
            if (parent->op != nir_op_imov)
               continue;
            break;
         default:
            continue;
         }

         nir_instr_rewrite_src(instr, &alu->src[i].src, parent->src[0].src);
         if (alu->src[i].abs) {
            /* abs trumps both neg and abs, do nothing */
         } else {
            alu->src[i].negate = (alu->src[i].negate != parent->src[0].negate);
            alu->src[i].abs |= parent->src[0].abs;
         }

         for (int j = 0; j < 4; ++j) {
            if (!nir_alu_instr_channel_used(alu, i, j))
               continue;
            alu->src[i].swizzle[j] = parent->src[0].swizzle[alu->src[i].swizzle[j]];
         }

         if (parent->dest.dest.ssa.uses->entries == 0 &&
             parent->dest.dest.ssa.if_uses->entries == 0)
            nir_instr_remove(&parent->instr);
      }

      switch (alu->op) {
      case nir_op_fsat:
         alu->op = nir_op_fmov;
         alu->dest.saturate = true;
         break;
      case nir_op_ineg:
         alu->op = nir_op_imov;
         alu->src[0].negate = !alu->src[0].negate;
         break;
      case nir_op_fneg:
         alu->op = nir_op_fmov;
         alu->src[0].negate = !alu->src[0].negate;
         break;
      case nir_op_iabs:
         alu->op = nir_op_imov;
         alu->src[0].abs = true;
         alu->src[0].negate = false;
         break;
      case nir_op_fabs:
         alu->op = nir_op_fmov;
         alu->src[0].abs = true;
         alu->src[0].negate = false;
         break;
      default:
         break;
      }

      /* We've covered sources.  Now we're going to try and saturate the
       * destination if we can.
       */

      if (!alu->dest.dest.is_ssa)
         continue;

      if (alu->dest.dest.ssa.if_uses->entries != 0)
         continue;

      bool all_children_are_sat = true;
      struct set_entry *entry;
      set_foreach(alu->dest.dest.ssa.uses, entry) {
         const nir_instr *child = entry->key;
         if (child->type != nir_instr_type_alu) {
            all_children_are_sat = false;
            continue;
         }

         nir_alu_instr *child_alu = nir_instr_as_alu(child);
         if (child_alu->src[0].negate || child_alu->src[0].abs) {
            all_children_are_sat = false;
            continue;
         }

         if (child_alu->op != nir_op_fsat &&
             !(child_alu->op == nir_op_fmov && child_alu->dest.saturate)) {
            all_children_are_sat = false;
            continue;
         }
      }

      if (!all_children_are_sat)
         continue;

      alu->dest.saturate = true;

      set_foreach(alu->dest.dest.ssa.uses, entry) {
         nir_alu_instr *child_alu = nir_instr_as_alu((nir_instr *)entry->key);
         child_alu->op = nir_op_fmov;
         child_alu->dest.saturate = false;
         /* We could propagate the dest of our instruction to the
          * destinations of the uses here.  However, one quick round of
          * copy propagation will clean that all up and then we don't have
          * the complexity.
          */
      }
   }

   return true;
}

static void
nir_lower_to_source_mods_impl(nir_function_impl *impl)
{
   nir_foreach_block(impl, nir_lower_to_source_mods_block, NULL);
}

void
nir_lower_to_source_mods(nir_shader *shader)
{
   nir_foreach_overload(shader, overload) {
      if (overload->impl)
         nir_lower_to_source_mods_impl(overload->impl);
   }
}
