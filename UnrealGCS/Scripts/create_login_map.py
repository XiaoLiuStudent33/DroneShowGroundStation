"""Run once in UE Editor after compiling the login module; preserves existing maps."""
import unreal

path = "/Game/Maps/L_GCS_Login"
if not unreal.EditorAssetLibrary.does_asset_exist(path):
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.new_level(path):
        raise RuntimeError("Could not create login map")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.GameModeBase)
    if not subsystem.save_current_level():
        raise RuntimeError("Could not save login map")
unreal.log("DroneGCS login map is ready: " + path)
