Import("env")
import os

def before_build(source, target, env):
    sdkconfig_path = os.path.join(env.subst("$BUILD_DIR"), "config", "sdkconfig")
    if os.path.exists(sdkconfig_path):
        os.remove(sdkconfig_path)
    print("Forced sdkconfig regeneration")

env.AddPreAction("buildprog", before_build)