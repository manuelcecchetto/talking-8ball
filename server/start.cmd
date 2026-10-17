@echo off
rem Starts the talking 8-ball server. First run: creates server\.venv and installs the requirements.
cd /d "%~dp0"
if not exist .venv\Scripts\python.exe (
  uv venv --python 3.12 .venv || python -m venv .venv
  uv pip install --python .venv\Scripts\python.exe -r requirements.txt || .venv\Scripts\python -m pip install -r requirements.txt
)
.venv\Scripts\python.exe -I server.py %*
