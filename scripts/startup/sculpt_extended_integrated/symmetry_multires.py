"""Multires detail transfer in the shared Sculpt reference frame."""
import bpy
from bpy.props import FloatProperty, BoolProperty

def install(reference_module):
    class SCULPT_OT_reference_multires_detail(bpy.types.Operator):
        bl_idname='sculpt.symmetrize_reference_multires_detail'
        bl_label='Symmetrize Multires in Reference Frame'
        bl_description='Mask-aware transfer of the active layer detail or Multires base without changing topology'
        bl_options={'REGISTER','UNDO'}
        distance:FloatProperty(name='Base Correspondence Distance',default=.001,min=.0000001,subtype='DISTANCE')
        respect_mask:BoolProperty(name='Respect Mask',default=True)
        base_surface:BoolProperty(name='Multires Base',default=False,options={'HIDDEN'})
        @classmethod
        def poll(cls,c):return c.mode=='SCULPT' and c.object and c.object.type=='MESH' and any(m.type=='MULTIRES' for m in c.object.modifiers)
        def invoke(self,c,event):return c.window_manager.invoke_props_dialog(self)
        def draw(self,c):
            self.layout.prop(self,'distance');self.layout.prop(self,'respect_mask')
            self.layout.label(text='Native base and surface; layer detail preserved' if self.base_surface else 'Subdivided detail only; base and L0 unchanged',icon='INFO')
            if self.base_surface:self.layout.label(text='Preview: requires layers without L0 displacement')
        def execute(self,c):
            ob=c.object;s=ob.sculpt_symref_preview
            linear=ob.matrix_world.to_3x3();lengths=[linear.col[i].length for i in range(3)]
            if min(lengths)<1e-8 or max(lengths)-min(lengths)>max(lengths)*1e-5 or linear.determinant()<=0 or any(abs(linear.col[i].dot(linear.col[j]))>lengths[i]*lengths[j]*1e-5 for i,j in ((0,1),(0,2),(1,2))):
                self.report({'ERROR'},'Preview: apply non-uniform or negative scale before continuing');return {'CANCELLED'}
            frame,valid=reference_module.reference_matrix(ob,c.evaluated_depsgraph_get())
            if not valid:self.report({'ERROR'},'Invalid reference');return {'CANCELLED'}
            axis='XYZ'.index(s.edit_direction[-1]);origin=ob.matrix_world.inverted()@frame.translation
            normal=(linear.transposed()@frame.to_3x3().col[axis]).normalized()
            try:
                return bpy.ops.sculpt.multires_detail_symmetrize_preview(
                    plane_origin=tuple(origin),plane_normal=tuple(normal),positive_source=not s.edit_direction.startswith('-'),
                    distance=self.distance/lengths[0],respect_mask=self.respect_mask,base_surface=self.base_surface)
            except RuntimeError as ex:self.report({'ERROR'},str(ex));return {'CANCELLED'}
    bpy.utils.register_class(SCULPT_OT_reference_multires_detail)
    return SCULPT_OT_reference_multires_detail
