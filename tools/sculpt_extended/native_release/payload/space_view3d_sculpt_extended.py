# SPDX-License-Identifier: GPL-2.0-or-later
import bpy

class VIEW3D_PT_facesets_rc1(bpy.types.Panel):
    bl_label='Face Sets HR'
    bl_idname='VIEW3D_PT_facesets_rc1'
    bl_space_type='VIEW_3D'
    bl_region_type='UI'
    bl_category='Sculpt Extended'
    @classmethod
    def poll(cls,context):
        return context.object is not None and context.object.type=='MESH'
    def draw(self,context):
        ob=context.object
        mods=[m for m in ob.modifiers if m.type=='MULTIRES']
        if len(mods)!=1 or len(ob.modifiers)!=1:
            self.layout.label(text='Serve un solo modificatore Multires.')
            return
        m=mods[0]
        side=(1<<(m.total_levels-1))+1 if 1<=m.total_levels<=8 else 0
        if not side or len(ob.data.loops)*side*side>16777216:
            self.layout.label(text='Mesh oltre i limiti Face Sets HR supportati.')
            return
        self.layout.label(text='Master al livello massimo: '+str(m.total_levels))
        if ob.mode!='SCULPT':
            self.layout.operator('object.mode_set',text='Entra in Sculpt').mode='SCULPT'
            return
        self.layout.operator('sculpt.facesets_brush_activate_v05',text='Attiva pennello Face Sets HR')
        self.layout.prop(m,'sculpt_levels',text='Livello Sculpt')
        gestures=self.layout.row(align=True)
        gestures.operator_context='INVOKE_REGION_WIN'
        gestures.operator('sculpt.face_set_box_gesture',text='Face Set Box')
        gestures.operator('sculpt.face_set_lasso_gesture',text='Lasso')
        gestures.operator('sculpt.face_set_line_gesture',text='Linea')
        self.layout.separator()
        self.layout.label(text='Visibilita HR (L0 incluso)')
        col=self.layout.column()
        col.enabled=True
        col.prop(context.scene,'facesets_rc2_id',text='ID Face Set')
        row=col.row(align=True)
        op=row.operator('sculpt.face_set_change_visibility',text='Nascondi ID')
        op.mode='HIDE_ACTIVE';op.active_face_set=context.scene.facesets_rc2_id
        op=row.operator('sculpt.face_set_change_visibility',text='Isola / Mostra tutto')
        op.mode='TOGGLE';op.active_face_set=context.scene.facesets_rc2_id
        op=col.operator('sculpt.face_set_change_visibility',text='Mostra ID')
        op.mode='SHOW_ACTIVE';op.active_face_set=context.scene.facesets_rc2_id
        self.layout.separator()
        self.layout.label(text='Strumenti sul Face Set scelto')
        row=self.layout.row(align=True)
        row.operator_context='EXEC_REGION_WIN'
        for mode,label in [('GROW','Grow'),('SHRINK','Shrink')]:
            op=row.operator('sculpt.face_set_edit',text=label)
            op.mode=mode
            op.active_face_set=context.scene.facesets_rc2_id
            op.modify_hidden=False
        col=self.layout.column()
        col.enabled=m.sculpt_levels>0
        row=col.row(align=True)
        row.operator_context='EXEC_REGION_WIN'
        for mode,label in [('REPLACE','Maschera ID'),('OTHERS','Maschera altri')]:
            op=row.operator('sculpt.multires_face_sets_hr_mask',text=label)
            op.mode=mode
            op.active_face_set=context.scene.facesets_rc2_id
        col.prop(context.scene,'facesets_rc10_solidify',text='Aggiungi spessore')
        col.prop(context.scene,'facesets_rc10_limit',text='Limite quad / vertici')
        col.operator_context='EXEC_REGION_WIN'
        op=col.operator('sculpt.multires_hr_extract_v05',text='Extract Face Set HR')
        op.source='FACE_SET'
        op.active_face_set=context.scene.facesets_rc2_id
        op.add_solidify=context.scene.facesets_rc10_solidify
        op.max_vertices=op.max_faces=context.scene.facesets_rc10_limit
        self.layout.label(text='Extract usa il livello Sculpt corrente (L1+).')
        self.layout.label(text='Se il layer ha modifiche pendenti: Commit.')
        self.layout.label(text='F3: Face Set Extract seleziona sotto il cursore.')
        self.layout.label(text='Shift+H: isola sotto il cursore / mostra tutto.')
        self.layout.label(text='L0 dipinge la faccia e i discendenti.')
        self.layout.label(text='I piccoli ID possono sparire in basso.')

class VIEW3D_PT_polypaint_blur_v01(bpy.types.Panel):
    bl_label='Polypaint HR'
    bl_idname='VIEW3D_PT_polypaint_blur_v01'
    bl_space_type='VIEW_3D'
    bl_region_type='UI'
    bl_category='Sculpt Extended'
    @classmethod
    def poll(cls,context):return context.object is not None and context.object.mode=='SCULPT'
    def draw(self,context):
        col=self.layout.column()
        col.label(text='Paint / Blur / Smear: L0 e livelli superiori.')
        col.label(text='L0: anteprima sulla mesh base.')
        col.label(text='Nessun limite globale di campioni.')
        col.operator_context='EXEC_REGION_WIN'
        op=col.operator('sculpt.polypaint_blur_probe_v01',text='Attiva Paint HR');op.action='PAINT'
        op=col.operator('sculpt.polypaint_blur_probe_v01',text='Attiva Blur HR');op.action='BLUR'
        op=col.operator('sculpt.polypaint_blur_probe_v01',text='Attiva Smear HR');op.action='SMEAR'
        col.label(text='Shift da Paint/Smear richiama Blur.')
        brush=context.tool_settings.sculpt.brush
        if brush:
            col.prop(brush,'strength',text='Intensita')
            col.prop(brush,'size',text='Raggio')
            if brush.sculpt_brush_type=='PAINT':
                for prop,label in [('blend','Fusione'),('flow','Flusso'),('wet_mix','Wet Mix'),('wet_persistence','Wet Persistence'),('density','Densita')]:
                    col.prop(brush,prop,text=label)
            if brush.sculpt_brush_type=='SMEAR':col.prop(brush,'smear_deform_type',text='Modo')


class SCULPT_OT_color_v03_clear_confirm(bpy.types.Operator):
    bl_idname='sculpt.color_v03_clear_confirm'
    bl_label='Rimuovi tutti i colori HR'
    bl_description='Rimuove tutti i Color Attribute e tutti i master HR, anche quelli rimasti senza attributo; supporta Undo'
    def invoke(self,context,event):return context.window_manager.invoke_confirm(self,event)
    def execute(self,context):
        return bpy.ops.sculpt.multires_polypaint_attribute('EXEC_DEFAULT',True,action='CLEAR')

class VIEW3D_PT_color_v03_attributes(bpy.types.Panel):
    bl_label='Color Attribute HR'
    bl_idname='VIEW3D_PT_color_v03_attributes'
    bl_space_type='VIEW_3D';bl_region_type='UI';bl_category='Sculpt Extended'
    @classmethod
    def poll(cls,context):
        return context.object is not None and context.object.type=='MESH' and context.object.mode in {'OBJECT','SCULPT'}
    def draw(self,context):
        ob=context.object;mods=[m for m in ob.modifiers if m.type=='MULTIRES']
        if len(mods)!=1:return
        m=mods[0];names={a.name for a in ob.data.color_attributes}
        if m.polypaint_attribute:names.add(m.polypaint_attribute)
        names.update(a.name for a in m.polypaint_attributes)
        col=self.layout.column();col.operator_context='EXEC_REGION_WIN'
        for name in sorted(names):
            row=col.row(align=True)
            if name in ob.data.color_attributes:
                op=row.operator('sculpt.multires_polypaint_attribute',text=name,depress=name==m.polypaint_attribute)
                op.action='SELECT';op.name=name
            else:row.label(text=name+' (master residuo)')
            op=row.operator('sculpt.multires_polypaint_attribute',text='',icon='X')
            op.action='REMOVE';op.name=name
        if not names:col.label(text='Nessun Color Attribute.')
        col.operator_context='INVOKE_REGION_WIN'
        col.operator('sculpt.color_v03_clear_confirm',text='Rimuovi tutti i colori HR',icon='TRASH')



classes = (
    SCULPT_OT_color_v03_clear_confirm,
    VIEW3D_PT_facesets_rc1,
    VIEW3D_PT_polypaint_blur_v01,
    VIEW3D_PT_color_v03_attributes,
)


def register_props():
    bpy.types.Scene.facesets_rc2_id=bpy.props.IntProperty(name='ID Face Set',default=2,min=1)
    bpy.types.Scene.facesets_rc10_solidify=bpy.props.BoolProperty(default=False)
    bpy.types.Scene.facesets_rc10_limit=bpy.props.IntProperty(default=250000,min=4,max=4000000)


def unregister_props():
    for name in ('facesets_rc2_id', 'facesets_rc10_solidify', 'facesets_rc10_limit'):
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
