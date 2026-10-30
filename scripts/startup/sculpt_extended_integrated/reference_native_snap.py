"""Use Blender's transform conventions without leaving cursor side effects."""
import bpy
from mathutils import Matrix,Vector
from bpy.props import EnumProperty,FloatProperty,FloatVectorProperty,BoolProperty,BoolVectorProperty

def same_matrix(a,b):
 return max(abs(a[i][j]-b[i][j]) for i in range(4) for j in range(4))<1e-7

def install(ref):
 class NativeReferenceTransform(bpy.types.Operator):
  bl_idname='sculpt.reference_native_transform_preview'
  bl_label='Posiziona con snap'
  # Reference placement is a tool setting, cancelled with Esc/the tool session.
  # The pivot target records its own native metadata-only Undo transaction.
  bl_options={'REGISTER','BLOCKING'}
  mode:EnumProperty(items=(('G','Sposta',''),('R','Ruota',''),('S','Dimensione piani','')),default='G')
  target:EnumProperty(items=(('REFERENCE','Riferimento','Posiziona i piani di simmetria'),('PIVOT','Pivot Sculpt','Posiziona il pivot per Sposta, Ruota e Scala')),default='REFERENCE')
  result_location:FloatVectorProperty(size=3,options={'HIDDEN'})
  result_rotation:FloatVectorProperty(size=3,subtype='EULER',options={'HIDDEN'})
  result_scale:FloatProperty(default=1.0,options={'HIDDEN'})
  has_result:BoolProperty(default=False,options={'HIDDEN'})
  constraint_axis:BoolVectorProperty(size=3,default=(False,False,False),options={'HIDDEN'})
  release_confirm:BoolProperty(default=False,options={'HIDDEN'})
  @classmethod
  def poll(cls,c):return c.area and c.area.type=='VIEW_3D' and c.mode in {'SCULPT','EDIT_MESH'} and c.object and hasattr(c.object,'sculpt_symref_preview')
  def apply_reference(self,c):
   if self.target!='REFERENCE' and not self.ob.sculpt_symref_preview.transform_pivot_linked:return
   if self.mode=='G':
    if not ref.set_world_location(self.ob,c.scene.cursor.location):
     raise RuntimeError('Ancoraggio non valido; sgancia il riferimento prima di spostarlo')
   else:self.ob.sculpt_symref_preview.rotation=(self.ob.matrix_world.to_quaternion().inverted()@c.scene.cursor.matrix.to_quaternion()).to_euler('XYZ')
   ref.redraw()
  def apply_scale(self,c,factor):
   size=max(.01,self.saved['display_size']*factor)
   if abs(c.object.sculpt_symref_preview.display_size-size)>1e-9:
    c.object.sculpt_symref_preview.display_size=size
    ref.redraw()
  def finish_scale(self,c,cancel=False):
   if cancel:
    for key,value in self.saved.items():setattr(c.object.sculpt_symref_preview,key,value)
   if c.area:c.area.header_text_set(None)
   ref.redraw()
   return {'CANCELLED'} if cancel else {'FINISHED'}
  def modal_scale(self,c,event):
   if event.type in {'ESC','RIGHTMOUSE'} and event.value=='PRESS':return self.finish_scale(c,True)
   if event.type in {'RET','NUMPAD_ENTER','LEFTMOUSE'} and event.value=='PRESS':return self.finish_scale(c)
   if event.value=='PRESS':
    if event.type=='BACK_SPACE':self.numeric=self.numeric[:-1]
    elif event.unicode and event.unicode in '0123456789.-':self.numeric+=event.unicode
   if event.type!='MOUSEMOVE' and event.value!='PRESS':return {'RUNNING_MODAL'}
   try:factor=float(self.numeric) if self.numeric else max(.001,1+(event.mouse_region_x-self.start_mouse_x)*.01)
   except ValueError:return {'RUNNING_MODAL'}
   if event.ctrl:factor=max(.001,round(factor*10.0)/10.0)
   self.apply_scale(c,factor)
   return {'RUNNING_MODAL'}
  def cleanup(self,c,restore_reference=False):
   if getattr(self,'timer',None):
    c.window_manager.event_timer_remove(self.timer);self.timer=None
   c.scene.cursor.rotation_mode=self.cursor_mode;c.scene.cursor.matrix=self.cursor_before
   if restore_reference and (self.target=='REFERENCE' or self.ob.sculpt_symref_preview.transform_pivot_linked):
    for key,value in self.saved.items():setattr(self.ob.sculpt_symref_preview,key,value)
   if c.area:c.area.header_text_set(None)
   ref.redraw()
  def invoke(self,c,event):
   if c.object.sculpt_symref_preview.locked and (self.target=='REFERENCE' or c.object.sculpt_symref_preview.transform_pivot_linked):return {'CANCELLED'}
   if self.target=='REFERENCE':
    if c.object.sculpt_symref_preview.locked:return {'CANCELLED'}
    if not ref.ensure_tool_session(c):return {'CANCELLED'}
   elif c.mode!='SCULPT':return {'CANCELLED'}
   self.ob=c.object;self.saved=ref.snapshot(self.ob.sculpt_symref_preview)
   if self.target=='REFERENCE':self.ob.sculpt_symref_preview.mode={'G':'MOVE','R':'ROTATE','S':'SIZE'}[self.mode]
   if self.mode=='S':
    if self.target!='REFERENCE':return {'CANCELLED'}
    self.start_mouse_x=event.mouse_region_x;self.numeric=''
    c.window_manager.modal_handler_add(self)
    c.area.header_text_set('Dimensione piani: numeri · Ctrl scatto 0,1 · Invio conferma · Esc annulla')
    return {'RUNNING_MODAL'}
   self.cursor_before=c.scene.cursor.matrix.copy();self.cursor_mode=c.scene.cursor.rotation_mode
   if self.target=='REFERENCE':
    self.frame=ref.reference_matrix(self.ob,c.evaluated_depsgraph_get())[0]
   else:
    s=self.ob.sculpt_symref_preview
    self.frame=ref.reference_matrix(self.ob,c.evaluated_depsgraph_get())[0] if s.transform_pivot_linked else self.ob.matrix_world@Matrix.Translation(Vector(s.transform_pivot_location))
   c.scene.cursor.matrix=self.frame
   self.timer=c.window_manager.event_timer_add(.03,window=c.window)
   c.window_manager.modal_handler_add(self)
   try:
    operation=bpy.ops.transform.translate if self.mode=='G' else bpy.ops.transform.rotate
    kwargs=dict(cursor_transform=True,orient_type='CURSOR',use_snap_self=True,use_snap_edit=True,use_snap_nonedit=True,release_confirm=self.release_confirm)
    if any(self.constraint_axis):kwargs['constraint_axis']=self.constraint_axis
    if self.mode=='G':kwargs['snap_target']='CENTER'
    result=operation('INVOKE_REGION_WIN',**kwargs)
    if 'RUNNING_MODAL' not in result:
     self.cleanup(c,True);return {'CANCELLED'}
   except Exception as ex:
    self.cleanup(c,True);self.report({'ERROR'},str(ex));return {'CANCELLED'}
   return {'RUNNING_MODAL'}
  def execute(self,c):
   if not self.has_result:return {'CANCELLED'}
   if c.object.sculpt_symref_preview.locked and (self.target=='REFERENCE' or c.object.sculpt_symref_preview.transform_pivot_linked):return {'CANCELLED'}
   if self.target=='REFERENCE' and c.object.sculpt_symref_preview.locked:return {'CANCELLED'}
   self.ob=c.object
   if self.mode=='S':
    if self.target!='REFERENCE':return {'CANCELLED'}
    self.saved=ref.snapshot(self.ob.sculpt_symref_preview)
    self.apply_scale(c,self.result_scale)
    return {'FINISHED'}
   cursor=c.scene.cursor.matrix.copy();rotation_mode=c.scene.cursor.rotation_mode
   try:
    c.scene.cursor.rotation_mode='XYZ';c.scene.cursor.location=self.result_location;c.scene.cursor.rotation_euler=self.result_rotation
    if self.target=='PIVOT':
     s=self.ob.sculpt_symref_preview
     if s.transform_pivot_linked:self.apply_reference(c)
     s.transform_pivot_location=self.ob.matrix_world.inverted_safe()@c.scene.cursor.location
     return bpy.ops.sculpt.set_pivot_position('EXEC_REGION_WIN',True,mode='CURSOR')
    self.apply_reference(c);return {'FINISHED'}
   finally:
    c.scene.cursor.rotation_mode=rotation_mode;c.scene.cursor.matrix=cursor
  def modal(self,c,event):
   if self.mode=='S':return self.modal_scale(c,event)
   if event.type!='TIMER':return {'PASS_THROUGH'}
   try:
    running=any(op.bl_idname in {'TRANSFORM_OT_translate','TRANSFORM_OT_rotate'} for op in c.window.modal_operators)
    self.apply_reference(c)
    if running:return {'PASS_THROUGH'}
    changed=not same_matrix(c.scene.cursor.matrix,self.frame)
    if changed:
     self.result_location=c.scene.cursor.location;self.result_rotation=c.scene.cursor.matrix.to_euler('XYZ');self.has_result=True
    if changed and self.target=='PIVOT':
     c.object.sculpt_symref_preview.transform_pivot_location=c.object.matrix_world.inverted_safe()@c.scene.cursor.location
     if bpy.ops.sculpt.set_pivot_position('EXEC_REGION_WIN',True,mode='CURSOR')!={'FINISHED'}:
      raise RuntimeError('Impossibile posizionare il pivot Sculpt')
    self.cleanup(c,not changed)
    return {'FINISHED'} if changed else {'CANCELLED'}
   except Exception as ex:
    self.cleanup(c,True);self.report({'ERROR'},str(ex));return {'CANCELLED'}
  def cancel(self,c):
   if hasattr(self,'cursor_before'):self.cleanup(c,True)

 class SculptPivotFromCursor(bpy.types.Operator):
  bl_idname='sculpt.snap_pivot_from_cursor_preview'
  bl_label='Pivot al cursore 3D'
  bl_options={'REGISTER','UNDO'}
  @classmethod
  def poll(cls,c):return c.mode=='SCULPT' and c.object and c.area and c.area.type=='VIEW_3D'
  def execute(self,c):return bpy.ops.sculpt.set_pivot_position('EXEC_REGION_WIN',True,mode='CURSOR')

 class SculptPivotUseReference(bpy.types.Operator):
  bl_idname='sculpt.pivot_use_reference_preview'
  bl_label='Usa riferimento come pivot'
  bl_options={'REGISTER'}
  @classmethod
  def poll(cls,c):return c.mode=='SCULPT' and c.object and hasattr(c.object,'sculpt_symref_preview')
  def execute(self,c):
   s=c.object.sculpt_symref_preview
   frame,_=ref.reference_matrix(c.object,c.evaluated_depsgraph_get())
   cursor=c.scene.cursor.matrix.copy()
   try:
    c.scene.cursor.matrix=frame
    result=bpy.ops.sculpt.set_pivot_position('EXEC_REGION_WIN',True,mode='CURSOR')
    if result=={'FINISHED'}:s.transform_pivot_location=c.object.matrix_world.inverted_safe()@frame.translation
    return result
   finally:c.scene.cursor.matrix=cursor

 class VIEW3D_GGT_sculpt_transform_pivot_preview(bpy.types.GizmoGroup):
  bl_idname='VIEW3D_GGT_sculpt_transform_pivot_preview'
  bl_label='Pivot pennelli trasformazione'
  bl_space_type='VIEW_3D';bl_region_type='WINDOW';bl_options={'3D','PERSISTENT'}
  @classmethod
  def poll(cls,c):
   return c.mode=='SCULPT' and c.object and hasattr(c.object,'sculpt_symref_preview') and c.object.sculpt_symref_preview.transform_pivot_enabled
  def setup(self,c):
   self.handles=[]
   colors=((1.,.18,.18),(.3,1.,.3),(.25,.55,1.))
   for axis in range(3):
    gz=self.gizmos.new('GIZMO_GT_arrow_3d');gz.color=colors[axis];gz.alpha=.9
    gz.color_highlight=(1.,1.,1.);gz.alpha_highlight=1.;gz.scale_basis=.8;gz.use_draw_modal=True
    props=gz.target_set_operator('sculpt.reference_native_transform_preview')
    props.mode='G';props.target='PIVOT';props.constraint_axis=tuple(i==axis for i in range(3));props.release_confirm=True
    self.handles.append((axis,gz))
  def draw_prepare(self,c):
   s=c.object.sculpt_symref_preview
   frame=ref.reference_matrix(c.object,c.evaluated_depsgraph_get())[0] if s.transform_pivot_linked else c.object.matrix_world@Matrix.Translation(Vector(s.transform_pivot_location))
   for axis,gz in self.handles:
    direction=frame.to_3x3().col[axis].normalized()
    basis=Vector((0,0,1)).rotation_difference(direction).to_matrix().to_4x4();basis.translation=frame.translation
    gz.matrix_basis=basis

 class SculptSnapPanel(bpy.types.Panel):
  bl_label='Snap e pivot Sculpt'
  bl_idname='VIEW3D_PT_sculpt_snap_pivot_preview'
  bl_space_type='VIEW_3D';bl_region_type='UI';bl_category='Tool'
  @classmethod
  def poll(cls,c):return c.mode=='SCULPT' and c.object and c.object.type=='MESH'
  def draw(self,c):
   layout=self.layout;ts=c.scene.tool_settings
   row=layout.row(align=True);row.prop(ts,'use_snap',text='Snap',icon='SNAP_ON');row.prop(ts,'snap_elements',text='')
   if 'VOLUME' in ts.snap_elements:layout.prop(ts,'use_snap_peel_object',text='Oggetti separati')
   s=c.object.sculpt_symref_preview
   layout.prop(s,'transform_pivot_enabled',toggle=True,icon='ORIENTATION_GIMBAL')
   layout.prop(s,'transform_pivot_linked')
   layout.operator_context='INVOKE_REGION_WIN'
   op=layout.operator('sculpt.reference_native_transform_preview',text='Posiziona pivot con snap',icon='PIVOT_CURSOR');op.target='PIVOT';op.mode='G'
   if s.transform_pivot_linked:layout.operator('sculpt.pivot_use_reference_preview',icon='LINKED')
   layout.operator('sculpt.snap_pivot_from_cursor_preview',icon='PIVOT_CURSOR')
   layout.label(text='G sposta il pivot · Ruota e Scala usano il pivot')
   layout.label(text='X/Y/Z · Ctrl snap · Esc annulla')

 classes=(NativeReferenceTransform,SculptPivotFromCursor,SculptPivotUseReference,VIEW3D_GGT_sculpt_transform_pivot_preview,SculptSnapPanel)
 for cls in classes:bpy.utils.register_class(cls)
 return classes
