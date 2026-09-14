import unreal
from pathlib import Path
for name in ['Saturn','StarSea','Moon']:
    task=unreal.AssetImportTask()
    task.filename=str(Path(unreal.Paths.project_dir()).resolve().parent / ('Tools/Art/Login/'+name+'Static.png'))
    task.destination_path='/Game/GCS/Login'
    task.destination_name='T_'+name+'Static'
    task.automated=True
    task.replace_existing=True
    task.save=True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture=unreal.load_asset('/Game/GCS/Login/T_'+name+'Static')
    if not texture: raise RuntimeError('Missing '+name)
    texture.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)
