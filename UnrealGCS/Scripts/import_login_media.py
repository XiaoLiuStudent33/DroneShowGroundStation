import unreal
from pathlib import Path
root=Path(unreal.Paths.project_dir()).resolve().parent
folder='/Game/GCS/Login/Media'
unreal.EditorAssetLibrary.make_directory(folder)
for name in ['Saturn','Wing','Tide']:
    task=unreal.AssetImportTask();task.filename=str(root/('Tools/Art/Login/Loops/'+name+'.png'));task.destination_path=folder;task.destination_name='T_'+name;task.automated=True;task.replace_existing=True;task.save=True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex=unreal.load_asset(folder+'/T_'+name)
    if not tex:raise RuntimeError('Missing poster '+name)
    tex.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_UI)
    tex.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    tex.set_editor_property('srgb',True)
    unreal.EditorAssetLibrary.save_loaded_asset(tex)
mat=unreal.load_asset(folder+'/M_LoginLight')
if not mat:mat=unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_LoginLight',folder,unreal.Material,unreal.MaterialFactoryNew())
unreal.MaterialEditingLibrary.delete_all_material_expressions(mat)
mat.set_editor_property('material_domain',unreal.MaterialDomain.MD_UI)
mat.set_editor_property('blend_mode',unreal.BlendMode.BLEND_ADDITIVE)
sample=unreal.MaterialEditingLibrary.create_material_expression(mat,unreal.MaterialExpressionTextureSampleParameter2D,-400,0)
sample.set_editor_property('parameter_name','ShowTexture');sample.set_editor_property('texture',unreal.load_asset(folder+'/T_Saturn'))
sample.set_editor_property('sampler_type',unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
unreal.MaterialEditingLibrary.connect_material_property(sample,'RGB',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
constant=unreal.MaterialEditingLibrary.create_material_expression(mat,unreal.MaterialExpressionConstant,-300,200);constant.set_editor_property('r',1.0)
unreal.MaterialEditingLibrary.connect_material_property(constant,'',unreal.MaterialProperty.MP_OPACITY)
unreal.MaterialEditingLibrary.recompile_material(mat)
unreal.EditorAssetLibrary.save_loaded_asset(mat)
unreal.log('LOGIN_MEDIA_IMPORT_OK')
