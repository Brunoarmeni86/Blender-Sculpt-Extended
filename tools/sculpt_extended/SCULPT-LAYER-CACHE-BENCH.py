"""Register a short interactive benchmark; run via F3 after opening a test scene."""
import bpy
import json
import math
import os
import pathlib
import time

class SCULPT_OT_layer_cache_benchmark(bpy.types.Operator):
    bl_idname = 'sculpt.layer_cache_benchmark'
    bl_label = 'Sculpt Layer Cache Benchmark'
    bl_description = 'Compare ten strength updates with support index off/on; use a test scene'

    def execute(self, context):
        ob=context.object
        if ob is None or ob.mode!='SCULPT':
            self.report({'ERROR'},'Seleziona la mesh ed entra in Sculpt Mode prima del test')
            return {'CANCELLED'}
        mmd=next((m for m in ob.modifiers if m.type=='MULTIRES' and len(m.sculpt_layers)),None)
        if mmd is None:
            self.report({'ERROR'},'Serve Multires con un Sculpt Layer contenente una pennellata')
            return {'CANCELLED'}
        index=mmd.active_sculpt_layer_index
        layer=mmd.sculpt_layers[index]
        if not layer.visible or not math.isfinite(layer.strength):
            self.report({'ERROR'},'Il layer attivo deve essere visibile e avere strength valida')
            return {'CANCELLED'}
        was_recording=mmd.sculpt_layer_recording
        previous_flag=os.environ.get('BLENDER_SCULPT_LAYER_SUPPORT_CACHE')
        original=layer.strength;timings={}
        try:
            for mode in ('0','1'):
                os.environ['BLENDER_SCULPT_LAYER_SUPPORT_CACHE']=mode
                durations=[]
                for value in (.25,.5,.75,1.0,original):
                    start=time.perf_counter();layer.strength=value;context.view_layer.update()
                    durations.append(1000*(time.perf_counter()-start))
                timings['index_off' if mode=='0' else 'index_on']=durations
        finally:
            layer.strength=original
            mmd.sculpt_layer_recording=was_recording
            if previous_flag is None:os.environ.pop('BLENDER_SCULPT_LAYER_SUPPORT_CACHE',None)
            else:os.environ['BLENDER_SCULPT_LAYER_SUPPORT_CACHE']=previous_flag
        out=pathlib.Path(__file__).with_name('native-layer-cache-bench.json')
        out.write_text(json.dumps(dict(blender=bpy.app.version_string,object=ob.name,
            layer=layer.name,sculpt_level=mmd.sculpt_levels,total_levels=mmd.total_levels,
            cache_mb=os.environ.get('BLENDER_SCULPT_LAYER_CACHE_MB','legacy'),times_ms=timings,
            scope='RNA and depsgraph updates only, excludes viewport drawing. First calls have cache warmup. Review native trace BUILD/HIT/fallback and inspect geometry manually; no automatic correctness claim.'),indent=2))
        self.report({'INFO'},'Test completato: native-layer-cache-bench.json e log nella cartella del kit')
        return {'FINISHED'}

bpy.utils.register_class(SCULPT_OT_layer_cache_benchmark)
print('Apri una scena di test, entra in Sculpt, fai una pennellata sul layer, poi F3: Sculpt Layer Cache Benchmark.')
