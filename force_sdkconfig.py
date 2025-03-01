Import("env")
import os
import shutil

def before_build(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    sdkconfig_path = os.path.join(build_dir, "config", "sdkconfig")
    defaults_path = os.path.join(env.subst("$PROJECT_DIR"), "sdkconfig.defaults")
    if os.path.exists(sdkconfig_path):
        os.remove(sdkconfig_path)
    if os.path.exists(defaults_path):
        shutil.copy(defaults_path, os.path.join(build_dir, "config", "sdkconfig.defaults"))
    print("Forced sdkconfig regeneration with 4MB flash")

env.AddPreAction("buildprog", before_build)