# Render entry point — uvicorn app:app se start hoga
import importlib.util, os, sys

spec = importlib.util.spec_from_file_location(
    "server",
    os.path.join(os.path.dirname(__file__), "gemini-code-1795329199931.py")
)
_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(_mod)
app = _mod.app
