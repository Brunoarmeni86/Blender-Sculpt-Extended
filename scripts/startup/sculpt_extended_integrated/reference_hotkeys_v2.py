import bpy,math
from mathutils import Vector,Quaternion
from bpy_extras import view3d_utils
from bpy.props import EnumProperty

def install(ref):
 class ReferenceTransform(bpy.types.Operator):
  bl_idname='sculpt.reference_transform_preview'
  bl_label='Trasforma riferimento'
  bl_options={'REGISTER','UNDO','BLOCKING'}
  mode:EnumProperty(items=[('G','Sposta',''),('R','Ruota',''),('S','Dimensione piani','')])
  @classmethod
  def poll(cls,c):return ref.tool_selected(c) and not c.object.sculpt_symref_preview.locked
  def invoke(self,c,event):
   if not ref.ensure_tool_session(c):return {'CANCELLED'}
   self.saved=ref.snapshot(c.object.sculpt_symref_preview)
   self.frame=ref.reference_matrix(c.object,c.evaluated_depsgraph_get())[0]
   self.start=Vector((event.mouse_region_x,event.mouse_region_y));self.axis=None;self.numeric=''
   c.object.sculpt_symref_preview.mode={'G':'MOVE','R':'ROTATE','S':'SIZE'}[self.mode]
   c.window_manager.modal_handler_add(self)
   c.area.header_text_set('Riferimento: X/Y/Z vincolo · numeri · Invio conferma · Esc annulla')
   return {'RUNNING_MODAL'}
  def finish(self,c,cancel=False):
   if cancel:
    for key,value in self.saved.items():setattr(c.object.sculpt_symref_preview,key,value)
   c.area.header_text_set(None);ref.redraw()
   return {'CANCELLED'} if cancel else {'FINISHED'}
  def modal(self,c,event):
   if event.type in {'ESC','RIGHTMOUSE'} and event.value=='PRESS':return self.finish(c,True)
   if event.type in {'RET','NUMPAD_ENTER','LEFTMOUSE'} and event.value=='PRESS':return self.finish(c)
   if event.value=='PRESS':
    if event.type in {'X','Y','Z'}:self.axis=None if self.axis==event.type else event.type
    elif event.type=='BACK_SPACE':self.numeric=self.numeric[:-1]
    elif event.unicode and event.unicode in '0123456789.-':self.numeric+=event.unicode
   if event.type!='MOUSEMOVE' and event.value!='PRESS':return {'RUNNING_MODAL'}
   delta=Vector((event.mouse_region_x,event.mouse_region_y))-self.start
   try:value=float(self.numeric) if self.numeric else None
   except ValueError:return {'RUNNING_MODAL'}
   s=c.object.sculpt_symref_preview
   axis=self.frame.to_3x3().col['XYZ'.index(self.axis)].normalized() if self.axis else c.region_data.view_rotation@Vector((0,0,1))
   if self.mode=='G':
    a=view3d_utils.region_2d_to_location_3d(c.region,c.region_data,self.start,self.frame.translation)
    b=view3d_utils.region_2d_to_location_3d(c.region,c.region_data,self.start+delta,self.frame.translation)
    movement=b-a
    if self.axis:movement=axis*(value if value is not None else movement.dot(axis))
    elif value is not None:movement=(c.region_data.view_rotation@Vector((1,0,0)))*value
    ref.set_world_location(c.object,self.frame.translation+movement)
   elif self.mode=='R':
    angle=math.radians(value) if value is not None else delta.x*.01
    rotation=Quaternion(axis,angle)@self.frame.to_quaternion()
    s.rotation=(c.object.matrix_world.to_quaternion().inverted()@rotation).to_euler('XYZ')
   else:
    factor=value if value is not None else max(.001,1+delta.x*.01)
    s.display_size=max(.01,self.saved['display_size']*factor)
   ref.redraw();return {'RUNNING_MODAL'}
 bpy.utils.register_class(ReferenceTransform)
 return ReferenceTransform
