"""Narrow anchored transformations, preserving unrelated edits and line endings."""
from pathlib import Path

HEADER_PATH='source/blender/blenkernel/BKE_multires_undo_cache_guard.hh'
OBJECT_PATH='source/blender/blenkernel/intern/object.cc'
UNDO_PATH='source/blender/editors/undo/memfile_undo.cc'
CONFORM_PATH='source/blender/blenkernel/intern/multires_reshape.cc'
MARKER='LAYER_CCG_UNDO_CACHE_GUARD_V1'
OBJECT_ANCHOR='''  const int tot_level = mesh_eval->runtime->subdiv_ccg_tot_level;
  Object *object_orig = DEG_get_original(object);'''
OBJECT_GUARD='''  const int tot_level = mesh_eval->runtime->subdiv_ccg_tot_level;
  /* LAYER_CCG_UNDO_CACHE_GUARD_V1: discard writes from the old evaluated
   * layer surface during Memfile restore. Persistent base and layer payloads
   * come from that restore, not from obsolete CCG coordinates. */
  if (bke::multires_layers::undo_cache::discard_active()) {
    for (const ModifierData &md : object->modifiers) {
      if (md.type == eModifierType_Multires &&
          !reinterpret_cast<const MultiresModifierData &>(md).sculpt_layers.is_empty())
      {
        std::printf("[LAYER_CCG_UNDO_GUARD] mode=DISCARD_OLD_CACHE level=%d top=%d "
                    "dirty=%d/%d/%d\\n",
                    subdiv_ccg->level, tot_level, int(subdiv_ccg->dirty.coords),
                    int(subdiv_ccg->dirty.hidden), int(subdiv_ccg->dirty.masks));
        std::fflush(stdout);
        return;
      }
    }
  }
  Object *object_orig = DEG_get_original(object);'''

DECODE_GUARD='''  BLI_assert(undo_direction != STEP_INVALID);
  /* LAYER_CCG_UNDO_CACHE_GUARD_V1: remains active through scene/depsgraph
   * restoration, including destruction of evaluated meshes on workers. */
  const bke::multires_layers::undo_cache::ScopedDiscardCacheWrites layer_cache_restore_guard;
  if (bke::multires_layers::undo_cache::discard_active()) {
    /* Depsgraphs can be reused and cleaned up lazily AFTER decode returns.
     * Drop only the pending-write flags of obsolete, live layer CCGs now;
     * no persistent mesh, mask or layer payload is changed here. */
    for (Object &object : bmain->objects) {
      SculptSession *ss = object.runtime->sculpt_session;
      if (!ss || !ss->subdiv_ccg) {
        continue;
      }
      bool has_layers = false;
      for (const ModifierData &md : object.modifiers) {
        if (md.type == eModifierType_Multires &&
            !reinterpret_cast<const MultiresModifierData &>(md).sculpt_layers.is_empty())
        {
          has_layers = true;
          break;
        }
      }
      SubdivCCG &ccg = *ss->subdiv_ccg;
      if (!has_layers || (!ccg.dirty.coords && !ccg.dirty.hidden && !ccg.dirty.masks)) {
        continue;
      }
      std::printf("[LAYER_CCG_UNDO_GUARD] mode=DISCARD_PENDING direction=%d level=%d "
                  "dirty=%d/%d/%d\\n", int(undo_direction), ccg.level,
                  int(ccg.dirty.coords), int(ccg.dirty.hidden), int(ccg.dirty.masks));
      std::fflush(stdout);
      ccg.dirty.coords = false;
      ccg.dirty.hidden = false;
      ccg.dirty.masks = false;
    }
  }'''

def once(text,old,new,label):
    if text.count(old)!=1:
        raise RuntimeError('Contesto sorgente diverso o ambiguo: '+label+'. Nessuna modifica applicata.')
    return text.replace(old,new,1)

def prepare(source,kit):
    source,kit=Path(source),Path(kit)
    if (source/HEADER_PATH).exists():
        raise RuntimeError('Guardia gia presente; usare RunDir per ripresa o ripristino.')
    outputs={}
    for rel in (OBJECT_PATH,UNDO_PATH,CONFORM_PATH):
        path=source/rel
        raw=path.read_bytes()
        text=raw.decode('utf-8').replace('\r\n','\n')
        if MARKER in text:
            raise RuntimeError('Sorgente gia modificato: '+rel)
        if rel==OBJECT_PATH:
            if '#include "DNA_modifier_types.h"' not in text:
                text=once(text,'#include "DNA_object_types.h"','#include "DNA_object_types.h"\n#include "DNA_modifier_types.h"',rel+' DNA include')
            text=once(text,'#include "BKE_multires.hh"','#include "BKE_multires.hh"\n#include "BKE_multires_undo_cache_guard.hh"',rel+' guard include')
            if '#include <cstdio>' not in text:
                text=once(text,'#include "BKE_multires_undo_cache_guard.hh"','#include "BKE_multires_undo_cache_guard.hh"\n#include <cstdio>',rel+' trace include')
            text=once(text,OBJECT_ANCHOR,OBJECT_GUARD,rel+' CCG writeback')
        elif rel==UNDO_PATH:
            text=once(text,'#include "BKE_main.hh"','#include "BKE_main.hh"\n#include "BKE_multires_undo_cache_guard.hh"',rel+' guard include')
            text=once(text,'#include "BKE_multires_undo_cache_guard.hh"',
                      '#include "BKE_multires_undo_cache_guard.hh"\n#include "BKE_paint.hh"\n#include "BKE_object_types.hh"\n#include "BKE_subdiv_ccg.hh"\n#include "DNA_modifier_types.h"\n#include <cstdio>',rel+' CCG includes')
            text=once(text,'  BLI_assert(undo_direction != STEP_INVALID);',
                DECODE_GUARD,rel+' decode scope')
        else:
            anchor='''  /* All conversion and validation completed on owned scratch storage. Preserve
   * layer objects, names, visibility, strength and RNA animation paths. */'''
            text=once(text,anchor,anchor+'''
  /* LAYER_CCG_UNDO_CACHE_GUARD_V1: invalidate derived layer payload caches
   * before publishing the rebased buffers. Never flush an old Sculpt CCG here. */
  bke::multires_layers::weight_hot_delta_cache_invalidate();''',rel+' Conform publication')
        if b'\r\n' in raw:text=text.replace('\n','\r\n')
        outputs[rel]={'before':raw,'after':text.encode('utf-8')}
    outputs[HEADER_PATH]={'before':None,'after':(kit/'BKE_multires_undo_cache_guard.hh').read_bytes()}
    return outputs
