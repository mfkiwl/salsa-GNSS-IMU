from plotResults import plotResults
import subprocess

# subprocess.call(("cmake", "..", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-GNinja"), cwd="../build")
# subprocess.call(("ninja", "mocapSim"), cwd="../build")
# subprocess.call(("./mocapSim"), cwd="../build")

plotResults("/tmp/Salsa/mocapSimulation/", saveFig=True)