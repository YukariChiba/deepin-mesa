/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "agx_compiler.h"
#include "nir_builder_opcodes.h"

#define ALL_SAMPLES 0xFF
#define BASE_Z      1
#define BASE_S      2

static bool
lower_zs_emit(nir_block *block)
{
   nir_intrinsic_instr *zs_emit = NULL;
   bool progress = false;

   nir_foreach_instr_reverse_safe(instr, block) {
      if (instr->type != nir_instr_type_intrinsic)
         continue;

      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_store_output)
         continue;

      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      if (sem.location != FRAG_RESULT_DEPTH &&
          sem.location != FRAG_RESULT_STENCIL)
         continue;

      nir_builder b = nir_builder_at(nir_before_instr(instr));

      nir_ssa_def *value = intr->src[0].ssa;
      bool z = (sem.location == FRAG_RESULT_DEPTH);

      unsigned src_idx = z ? 1 : 2;
      unsigned base = z ? BASE_Z : BASE_S;

      /* In the hw, depth is 32-bit but stencil is 16-bit. Instruction
       * selection checks this, so emit the conversion now.
       */
      if (z)
         value = nir_f2f32(&b, value);
      else
         value = nir_u2u16(&b, value);

      if (zs_emit == NULL) {
         /* Multisampling will get lowered later if needed, default to
          * broadcast
          */
         nir_ssa_def *sample_mask = nir_imm_intN_t(&b, ALL_SAMPLES, 16);
         zs_emit = nir_store_zs_agx(&b, sample_mask,
                                    nir_ssa_undef(&b, 1, 32) /* depth */,
                                    nir_ssa_undef(&b, 1, 16) /* stencil */);
      }

      assert((nir_intrinsic_base(zs_emit) & base) == 0 &&
             "each of depth/stencil may only be written once");

      nir_instr_rewrite_src_ssa(&zs_emit->instr, &zs_emit->src[src_idx], value);
      nir_intrinsic_set_base(zs_emit, nir_intrinsic_base(zs_emit) | base);

      nir_instr_remove(instr);
      progress = true;
   }

   return progress;
}

static bool
lower_discard(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_discard &&
       intr->intrinsic != nir_intrinsic_discard_if)
      return false;

   b->cursor = nir_before_instr(instr);

   nir_ssa_def *all_samples = nir_imm_intN_t(b, ALL_SAMPLES, 16);
   nir_ssa_def *no_samples = nir_imm_intN_t(b, 0, 16);
   nir_ssa_def *killed_samples = all_samples;

   if (intr->intrinsic == nir_intrinsic_discard_if)
      killed_samples = nir_bcsel(b, intr->src[0].ssa, all_samples, no_samples);

   /* This will get lowered later as needed */
   nir_discard_agx(b, killed_samples);
   nir_instr_remove(instr);
   return true;
}

static bool
agx_nir_lower_discard(nir_shader *s)
{
   if (!s->info.fs.uses_discard)
      return false;

   return nir_shader_instructions_pass(
      s, lower_discard, nir_metadata_block_index | nir_metadata_dominance,
      NULL);
}

static bool
agx_nir_lower_zs_emit(nir_shader *s)
{
   /* If depth/stencil isn't written, there's nothing to lower */
   if (!(s->info.outputs_written & (BITFIELD64_BIT(FRAG_RESULT_STENCIL) |
                                    BITFIELD64_BIT(FRAG_RESULT_DEPTH))))
      return false;

   bool any_progress = false;

   nir_foreach_function_impl(impl, s) {
      bool progress = false;

      nir_foreach_block(block, impl) {
         progress |= lower_zs_emit(block);
      }

      if (progress) {
         nir_metadata_preserve(
            impl, nir_metadata_block_index | nir_metadata_dominance);
      } else {
         nir_metadata_preserve(impl, nir_metadata_all);
      }

      any_progress |= progress;
   }

   return any_progress;
}

bool
agx_nir_lower_discard_zs_emit(nir_shader *s)
{
   bool progress = false;

   /* Lower depth/stencil writes before discard so the interaction works */
   progress |= agx_nir_lower_zs_emit(s);
   progress |= agx_nir_lower_discard(s);

   return progress;
}
