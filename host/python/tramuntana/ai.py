"""
tramuntana.ai - register the MCP server and the tramuntana-debug skill with
AI coding agents, and remove them again.

    tramuntana ai install claude|opencode|copilot|all [--user | --project PATH]
    tramuntana ai uninstall claude|opencode|copilot|all [--user | --project PATH]

The server is registered as `<this python> -m tramuntana.mcp_server`, so it
runs from whatever environment the package is installed in (pipx, a venv,
an editable checkout). The skill ships inside the package and is linked
into .claude/skills, where Claude Code, opencode and VS Code Copilot all
look for skills.

    agent      --user (default, every project)          --project PATH (that directory only)
    claude     claude mcp add -> ~/.claude.json          PATH/.mcp.json
    opencode   ~/.config/opencode/opencode.json[c]       PATH/opencode.json[c]
    copilot    ~/.config/Code/User/mcp.json              PATH/.vscode/mcp.json
    skill      ~/.claude/skills/tramuntana-debug         PATH/.claude/skills/tramuntana-debug

`project` below is that PATH, or None for the user-wide install.

opencode and VS Code have no command to remove a server, so their entries
are edited in the JSON file directly. In a file with comments (JSONC) the
entry is removed as text, keeping the comments; install falls back to the
agent's own CLI there (user-wide) or prints the entry to add (project).
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

AGENTS = ("claude", "opencode", "copilot")
NAME = "tramuntana"
SKILL = "tramuntana-debug"


def server_command() -> List[str]:
    """How an agent launches the server: this interpreter, the package's module."""
    return [sys.executable, "-m", "tramuntana.mcp_server"]


def skill_source() -> Path:
    return Path(__file__).resolve().parent / "skills" / SKILL


def _say(text: str):
    print(text, flush=True)


def _config_home() -> Path:
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")


def _where(project: Optional[Path]) -> str:
    return f"project {project}" if project else "user"


# --- JSON config files --------------------------------------------------

def _load_json(path: Path) -> Optional[dict]:
    """The file's contents ({} if missing or empty), or None if it isn't
    plain JSON (comments, trailing commas) and must not be rewritten."""
    if not path.exists():
        return {}
    text = path.read_text()
    if not text.strip():
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return None


def _save_json(path: Path, cfg: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2) + "\n")


def _json_set(path: Path, section: str, entry: dict, seed: Optional[dict] = None) -> bool:
    """Adds or replaces the tramuntana entry under `section`. False if the
    file can't be rewritten safely."""
    cfg = _load_json(path)
    if cfg is None:
        return False
    if not cfg and seed:
        cfg.update(seed)
    cfg.setdefault(section, {})[NAME] = entry
    _save_json(path, cfg)
    return True


def _skip(text: str, i: int) -> int:
    """Index of the next character that isn't whitespace or a comment."""
    while i < len(text):
        if text[i].isspace():
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = len(text) if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = len(text) if j < 0 else j + 2
        else:
            break
    return i


def _string_end(text: str, i: int) -> int:
    """Index just after the string literal that opens at text[i]."""
    i += 1
    while i < len(text) and text[i] != '"':
        i += 2 if text[i] == "\\" else 1
    return i + 1


def _jsonc_member(text: str):
    """(start, end) of the `"tramuntana": { ... }` member in a JSONC text,
    found outside strings and comments, or None."""
    key = f'"{NAME}"'
    i = 0
    while True:
        i = _skip(text, i)
        if i >= len(text):
            return None
        if text[i] != '"':
            i += 1
            continue
        end = _string_end(text, i)
        if text[i:end] == key:
            colon = _skip(text, end)
            brace = _skip(text, colon + 1) if colon < len(text) and text[colon] == ":" else -1
            if brace >= 0 and brace < len(text) and text[brace] == "{":
                depth, j = 0, brace
                while j < len(text):
                    j = _skip(text, j)
                    if j >= len(text):
                        return None
                    if text[j] == '"':
                        j = _string_end(text, j)
                        continue
                    depth += {"{": 1, "}": -1}.get(text[j], 0)
                    j += 1
                    if depth == 0:
                        return i, j
                return None
        i = end


def _jsonc_drop(text: str) -> Optional[str]:
    """The JSONC text without the tramuntana member (comments kept), or None
    if it has none. Takes the comma that separated it from a neighbour."""
    span = _jsonc_member(text)
    if span is None:
        return None
    start, end = span
    nxt = _skip(text, end)
    if nxt < len(text) and text[nxt] == ",":
        end = nxt + 1                      # "tramuntana": {...}, "next": ...
    else:
        prev = len(text[:start].rstrip())
        if prev > 0 and text[prev - 1] == ",":
            start = prev - 1               # "prev": ..., "tramuntana": {...}
    while start > 0 and text[start - 1] in " \t":
        start -= 1
    if start > 0 and text[start - 1] == "\n" and end < len(text) and text[end] == "\n":
        end += 1
    return text[:start] + text[end:]


def _json_del(path: Path, section: str) -> bool:
    """Removes the tramuntana entry; False if it wasn't there. A JSONC file
    is edited as text, so its comments survive."""
    cfg = _load_json(path)
    if cfg is None:
        text = _jsonc_drop(path.read_text())
        if text is None:
            return False
        path.write_text(text)
        return True
    if NAME not in cfg.get(section, {}):
        return False
    del cfg[section][NAME]
    if not cfg[section]:
        del cfg[section]
    _save_json(path, cfg)
    return True


def _json_has(path: Path, section: str) -> bool:
    cfg = _load_json(path)
    if cfg is None:
        return _jsonc_member(path.read_text()) is not None
    return NAME in cfg.get(section, {})


def _manual(path: Path, section: str, entry: dict):
    _say(f"    {path} has comments (JSONC), not touching it; add this under \"{section}\" by hand:")
    for line in json.dumps({NAME: entry}, indent=2).splitlines():
        _say("      " + line)


# --- agents -------------------------------------------------------------

def _claude(args: List[str], project: Optional[Path], quiet: bool = True) -> subprocess.CompletedProcess:
    """Runs the claude CLI. Project scope means .mcp.json in the directory
    it runs from, so it runs from the project."""
    scope = ["--scope", "project" if project else "user"] if args[0] in ("add", "remove") else []
    cmd = ["claude", "mcp", args[0], *args[1:2], *scope, *args[2:]]
    return subprocess.run(cmd, cwd=project, capture_output=quiet, text=True, stdin=subprocess.DEVNULL)


def _claude_registered(project: Optional[Path]) -> bool:
    r = _claude(["get", NAME], project)
    if r.returncode != 0:
        return False
    return ("Project config" if project else "User config") in r.stdout


def _opencode_config(project: Optional[Path]) -> Path:
    d = project if project else _config_home() / "opencode"
    jsonc = d / "opencode.jsonc"
    return jsonc if jsonc.exists() else d / "opencode.json"


def _copilot_config(project: Optional[Path]) -> Path:
    if project:
        return project / ".vscode" / "mcp.json"
    return _config_home() / "Code" / "User" / "mcp.json"


def _opencode_entry() -> dict:
    return {"type": "local", "command": server_command()}


def _copilot_entry() -> dict:
    cmd = server_command()
    return {"type": "stdio", "command": cmd[0], "args": cmd[1:]}


def _cli_for(agent: str, project: Optional[Path]) -> Optional[str]:
    """The CLI an agent needs for this install, or None if none is needed:
    project installs of opencode and Copilot only write a JSON file."""
    if agent == "claude":
        return "claude"
    if project:
        return None
    return "opencode" if agent == "opencode" else "code"


def install_agent(agent: str, project: Optional[Path]):
    if agent == "claude":
        _say(f"==> Claude Code: registering the {NAME} MCP server ({_where(project)})")
        if _claude_registered(project):
            _claude(["remove", NAME], project)
            _say("    replacing the existing entry")
        r = _claude(["add", NAME, "--", *server_command()], project, quiet=False)
        if r.returncode != 0:
            raise RuntimeError("'claude mcp add' failed")
        _say("    verify with: claude mcp list")

    elif agent == "opencode":
        cfg = _opencode_config(project)
        _say(f"==> opencode: registering the {NAME} MCP server ({_where(project)}: {cfg})")
        if _json_set(cfg, "mcp", _opencode_entry(), seed={"$schema": "https://opencode.ai/config.json"}):
            _say("    written")
        elif not project and not _json_has(cfg, "mcp"):
            # 'opencode mcp add' writes the global config and keeps its comments.
            subprocess.run(["opencode", "mcp", "add", NAME, "--"] + server_command(),
                           stdin=subprocess.DEVNULL)
        else:
            _manual(cfg, "mcp", _opencode_entry())
        _say("    verify with: opencode mcp list")

    elif agent == "copilot":
        cfg = _copilot_config(project)
        _say(f"==> GitHub Copilot (VS Code): registering the {NAME} MCP server ({_where(project)}: {cfg})")
        if _json_set(cfg, "servers", _copilot_entry()):
            _say("    written")
        elif not project and not _json_has(cfg, "servers"):
            entry = dict(_copilot_entry(), name=NAME)
            subprocess.run(["code", "--add-mcp", json.dumps(entry)], stdin=subprocess.DEVNULL)
        else:
            _manual(cfg, "servers", _copilot_entry())
        _say("    in VS Code: Copilot Chat in Agent mode -> tools, or the 'MCP: List Servers' command")


def uninstall_agent(agent: str, project: Optional[Path]):
    if agent == "claude":
        _say(f"==> Claude Code: removing the {NAME} MCP server ({_where(project)})")
        r = _claude(["remove", NAME], project)
        _say("    removed" if r.returncode == 0 else "    not registered")

    elif agent in ("opencode", "copilot"):
        label = "opencode" if agent == "opencode" else "GitHub Copilot (VS Code)"
        cfg = _opencode_config(project) if agent == "opencode" else _copilot_config(project)
        section = "mcp" if agent == "opencode" else "servers"
        _say(f"==> {label}: removing the {NAME} MCP server ({_where(project)}: {cfg})")
        _say("    removed" if _json_del(cfg, section) else "    not registered")


def registered(agent: str, project: Optional[Path]) -> bool:
    if agent == "claude":
        return shutil.which("claude") is not None and _claude_registered(project)
    if agent == "opencode":
        return _json_has(_opencode_config(project), "mcp")
    return _json_has(_copilot_config(project), "servers")


# --- skill --------------------------------------------------------------

def _skill_dest(project: Optional[Path]) -> Path:
    return (project or Path.home()) / ".claude" / "skills" / SKILL


def install_skill(project: Optional[Path]):
    src, dst = skill_source(), _skill_dest(project)
    _say(f"==> Skill: {SKILL} -> {dst}")
    if dst.is_symlink():
        dst.unlink()
        dst.symlink_to(src, target_is_directory=True)
        _say("    link updated")
    elif dst.exists():
        _say(f"    {dst} exists and is not a link, leaving it alone")
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            dst.symlink_to(src, target_is_directory=True)
            _say("    linked")
        except OSError:
            shutil.copytree(src, dst)      # no symlinks (Windows without developer mode)
            _say("    copied (re-run after upgrading the package)")


def uninstall_skill(project: Optional[Path]):
    src, dst = skill_source(), _skill_dest(project)
    _say(f"==> Skill: removing {dst}")
    if dst.is_symlink():
        dst.unlink()
        _say("    removed")
    elif dst.is_dir() and (dst / "SKILL.md").exists() and \
            (dst / "SKILL.md").read_bytes() == (src / "SKILL.md").read_bytes():
        shutil.rmtree(dst)                 # the copy install_skill made
        _say("    removed")
    elif dst.exists():
        _say(f"    {dst} is not ours (or was edited), leaving it alone")
    else:
        _say("    not installed")


# --- entry points -------------------------------------------------------

def _targets(target: str, project: Optional[Path]) -> List[str]:
    agents = list(AGENTS) if target == "all" else [target]
    out = []
    for agent in agents:
        cli = _cli_for(agent, project)
        if cli and shutil.which(cli) is None:
            if target == "all":
                _say(f"==> '{cli}' not found in PATH, skipping {agent}")
                continue
            raise RuntimeError(f"'{cli}' not found in PATH")
        out.append(agent)
    return out


def _project_dir(project) -> Optional[Path]:
    if project is None:
        return None
    path = Path(project).expanduser().resolve()
    if not path.is_dir():
        raise RuntimeError(f"{path} is not a directory")
    return path


def install(target: str, project=None) -> int:
    """Registers the server with `target` ("claude", "opencode", "copilot"
    or "all") and installs the skill; user-wide, or only in the directory
    `project`."""
    try:
        import mcp  # noqa: F401
    except ImportError:
        _say('warning: the MCP SDK is not installed here; agents won\'t be able to start the server '
             'until you install the extra: pip install "tramuntana[mcp]"')
    try:
        project = _project_dir(project)
        for agent in _targets(target, project):
            install_agent(agent, project)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    install_skill(project)
    _say("==> Done.")
    return 0


def uninstall(target: str, project=None) -> int:
    """Undoes install() for the same target and location."""
    try:
        project = _project_dir(project)
        for agent in _targets(target, project):
            uninstall_agent(agent, project)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    # The skill is shared by every agent: keep it while one still uses the server.
    others = [a for a in AGENTS if a != target and registered(a, project)] if target != "all" else []
    if others:
        _say(f"==> Skill: kept, still used by {', '.join(others)}")
    else:
        uninstall_skill(project)
    _say("==> Done.")
    return 0
