import unreal


SOURCE_IMAGE = r"C:\Users\13092\Documents\无人机地面站\UnrealGCS\SourceData\Map\T_GCS_VenueMap_31.932471N_118.638531E_2km.jpg"
TEXTURE_PATH = "/Game/Map/Textures"
MATERIAL_PATH = "/Game/Map/Materials"
TEXTURE_NAME = "T_GCS_VenueMap"
MATERIAL_NAME = "M_GCS_2DMap"
INSTANCE_NAME = "MI_GCS_2DMap"
GEOREFERENCE_PATH = "/Game/Map/Georeference"
GEOREFERENCE_NAME = "BP_GCS_Georeference"
MAP_LATITUDE = 31.932471
MAP_LONGITUDE = 118.638531
MAP_HEIGHT_METERS = 0.0


asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
editor_assets = unreal.EditorAssetLibrary


def import_texture():
    texture_asset_path = f"{TEXTURE_PATH}/{TEXTURE_NAME}"
    if editor_assets.does_asset_exist(texture_asset_path):
        texture = editor_assets.load_asset(texture_asset_path)
    else:
        task = unreal.AssetImportTask()
        task.filename = SOURCE_IMAGE
        task.destination_path = TEXTURE_PATH
        task.destination_name = TEXTURE_NAME
        task.automated = True
        task.replace_existing = True
        task.save = True
        asset_tools.import_asset_tasks([task])
        texture = editor_assets.load_asset(texture_asset_path)

    if not texture:
        raise RuntimeError("Failed to import venue map texture")

    texture.set_editor_property("srgb", True)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
    texture.set_editor_property("filter", unreal.TextureFilter.TF_TRILINEAR)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
    texture.modify()
    editor_assets.save_loaded_asset(texture)
    return texture


def create_material(texture):
    material_asset_path = f"{MATERIAL_PATH}/{MATERIAL_NAME}"
    material = (
        editor_assets.load_asset(material_asset_path)
        if editor_assets.does_asset_exist(material_asset_path)
        else None
    )
    if not material:
        material = asset_tools.create_asset(
            MATERIAL_NAME,
            MATERIAL_PATH,
            unreal.Material,
            unreal.MaterialFactoryNew(),
        )

    if not material:
        raise RuntimeError("Failed to create 2D map material")

    unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
    texture_parameter = unreal.MaterialEditingLibrary.create_material_expression(
        material,
        unreal.MaterialExpressionTextureSampleParameter2D,
        -360,
        0,
    )
    texture_parameter.set_editor_property("parameter_name", "MapTexture")
    texture_parameter.set_editor_property("texture", texture)

    unreal.MaterialEditingLibrary.connect_material_property(
        texture_parameter,
        "RGB",
        unreal.MaterialProperty.MP_EMISSIVE_COLOR,
    )
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", False)
    unreal.MaterialEditingLibrary.recompile_material(material)
    material.modify()
    editor_assets.save_loaded_asset(material)
    return material


def create_material_instance(material, texture):
    instance_asset_path = f"{MATERIAL_PATH}/{INSTANCE_NAME}"
    instance = (
        editor_assets.load_asset(instance_asset_path)
        if editor_assets.does_asset_exist(instance_asset_path)
        else None
    )
    if not instance:
        factory = unreal.MaterialInstanceConstantFactoryNew()
        instance = asset_tools.create_asset(
            INSTANCE_NAME,
            MATERIAL_PATH,
            unreal.MaterialInstanceConstant,
            factory,
        )

    if not instance:
        raise RuntimeError("Failed to create 2D map material instance")

    instance.set_editor_property("parent", material)
    unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
        instance,
        "MapTexture",
        texture,
    )
    instance.modify()
    editor_assets.save_loaded_asset(instance)
    return instance


def create_georeference_blueprint():
    asset_path = f"{GEOREFERENCE_PATH}/{GEOREFERENCE_NAME}"
    blueprint = (
        editor_assets.load_asset(asset_path)
        if editor_assets.does_asset_exist(asset_path)
        else None
    )
    if not blueprint:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", unreal.CesiumGeoreference)
        blueprint = asset_tools.create_asset(
            GEOREFERENCE_NAME,
            GEOREFERENCE_PATH,
            unreal.Blueprint,
            factory,
        )

    if not blueprint:
        raise RuntimeError("Failed to create Cesium georeference Blueprint")

    generated_class = blueprint.generated_class()
    default_object = unreal.get_default_object(generated_class)
    default_object.set_editor_property("origin_latitude", MAP_LATITUDE)
    default_object.set_editor_property("origin_longitude", MAP_LONGITUDE)
    default_object.set_editor_property("origin_height", MAP_HEIGHT_METERS)
    default_object.modify()
    blueprint.modify()
    editor_assets.save_loaded_asset(blueprint)
    return blueprint


texture = import_texture()
material = create_material(texture)
instance = create_material_instance(material, texture)
georeference = create_georeference_blueprint()
unreal.EditorAssetLibrary.save_directory("/Game/Map", only_if_is_dirty=False, recursive=True)
unreal.log(
    "GCS_MAP_ASSETS_READY "
    f"texture={texture.get_path_name()} "
    f"material={material.get_path_name()} "
    f"instance={instance.get_path_name()} "
    f"georeference={georeference.get_path_name()}"
)
