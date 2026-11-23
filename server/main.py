import os
import sys

# Run the assistant server
if __name__ == "__main__":
    import uvicorn
    # Dynamically import gemini-code-1795329199931
    import importlib.util
    spec = importlib.util.spec_from_file_location("assistant", os.path.join(os.path.dirname(__file__), "gemini-code-1795329199931.py"))
    assistant = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(assistant)
    uvicorn.run(assistant.app, host="0.0.0.0", port=8000)
