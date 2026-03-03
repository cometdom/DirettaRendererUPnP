"""
Config parsers for Diretta web UI.

Two formats supported:
  - ShellVarConfig: individual KEY=VALUE lines (DirettaRendererUPnP)
  - CliOptsConfig: single variable with CLI args (slim2diretta)
"""

import os
import re
import shlex


class ShellVarConfig:
    """Parse and write shell-style config files with KEY=VALUE lines.

    Preserves comments and file structure on save.
    """

    @staticmethod
    def load(path):
        """Load config file and return dict of key -> value.

        Handles quoted and unquoted values:
            TARGET=1          -> {"TARGET": "1"}
            VERBOSE="--verbose" -> {"VERBOSE": "--verbose"}
            VERBOSE=""        -> {"VERBOSE": ""}
        """
        settings = {}
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                m = re.match(r'^([A-Z_][A-Z0-9_]*)=(.*)$', line)
                if m:
                    key = m.group(1)
                    val = m.group(2).strip()
                    # Strip surrounding quotes
                    if len(val) >= 2 and val[0] == val[-1] and val[0] in ('"', "'"):
                        val = val[1:-1]
                    settings[key] = val
        return settings

    @staticmethod
    def save(path, settings):
        """Rewrite config file, updating existing keys and preserving structure.

        - Lines with existing keys are updated in place
        - Comments and blank lines are preserved
        - Keys not present in the file are appended at the end
        """
        if os.path.exists(path):
            with open(path, 'r') as f:
                lines = f.readlines()
        else:
            lines = []

        written_keys = set()
        new_lines = []

        for line in lines:
            stripped = line.strip()
            # Match active or commented-out assignment
            m = re.match(r'^#?\s*([A-Z_][A-Z0-9_]*)=', stripped)
            if m:
                key = m.group(1)
                if key in settings:
                    val = settings[key]
                    # Quote values that are empty or contain special chars
                    if val == '' or re.search(r'[\s#\-]', val):
                        new_lines.append(f'{key}="{val}"\n')
                    else:
                        new_lines.append(f'{key}={val}\n')
                    written_keys.add(key)
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)

        # Append any new keys not already in the file
        for key, val in settings.items():
            if key not in written_keys:
                if val == '' or re.search(r'[\s#\-]', val):
                    new_lines.append(f'{key}="{val}"\n')
                else:
                    new_lines.append(f'{key}={val}\n')

        with open(path, 'w') as f:
            f.writelines(new_lines)


class CliOptsConfig:
    """Parse and write config files with a single CLI options variable.

    Example: SLIM2DIRETTA_OPTS="-s 192.168.1.100 -n Room --thread-mode 3"
    """

    # Map short flags to long setting keys (for slim2diretta)
    SHORT_TO_LONG = {
        '-s': 'server',
        '-n': 'name',
        '-v': 'verbose',
    }

    # Flags that take no argument (boolean)
    BOOLEAN_FLAGS = {'-v', '--verbose', '--no-gapless'}

    @staticmethod
    def load(path, var_name='SLIM2DIRETTA_OPTS', settings_meta=None):
        """Load config and parse CLI opts into a dict.

        Args:
            path: Config file path
            var_name: Variable name to extract
            settings_meta: List of setting dicts from profile (used to know
                           which keys are flags vs key-value pairs)

        Returns:
            Dict of setting_key -> value. Boolean flags have value "true".
        """
        # Build sets from metadata
        flag_keys = set(CliOptsConfig.BOOLEAN_FLAGS)
        if settings_meta:
            for s in settings_meta:
                if s.get('type') == 'boolean':
                    cli_key = s.get('cli_arg', f'--{s["key"]}')
                    flag_keys.add(cli_key)

        raw_opts = ''
        with open(path, 'r') as f:
            for line in f:
                stripped = line.strip()
                if stripped.startswith('#'):
                    continue
                m = re.match(rf'^{re.escape(var_name)}=(.*)$', stripped)
                if m:
                    val = m.group(1).strip()
                    if len(val) >= 2 and val[0] == val[-1] and val[0] in ('"', "'"):
                        val = val[1:-1]
                    raw_opts = val
                    break

        if not raw_opts:
            return {}

        settings = {}
        try:
            tokens = shlex.split(raw_opts)
        except ValueError:
            tokens = raw_opts.split()

        i = 0
        while i < len(tokens):
            token = tokens[i]
            if token.startswith('-'):
                # Normalize key
                if token in CliOptsConfig.SHORT_TO_LONG:
                    key = CliOptsConfig.SHORT_TO_LONG[token]
                elif token.startswith('--'):
                    key = token[2:]  # strip --
                else:
                    key = token

                if token in flag_keys:
                    settings[key] = 'true'
                    i += 1
                elif i + 1 < len(tokens) and not tokens[i + 1].startswith('-'):
                    # Consume all non-flag tokens as the value
                    # (handles multi-word values like -n Living Room)
                    value_parts = []
                    i += 1
                    while i < len(tokens) and not tokens[i].startswith('-'):
                        value_parts.append(tokens[i])
                        i += 1
                    settings[key] = ' '.join(value_parts)
                else:
                    settings[key] = 'true'
                    i += 1
            else:
                i += 1

        return settings

    @staticmethod
    def save(path, var_name, settings, settings_meta=None):
        """Rebuild CLI opts string and write to config file.

        Args:
            path: Config file path
            var_name: Variable name to write
            settings: Dict of key -> value
            settings_meta: Profile settings list for CLI arg mapping
        """
        # Build the CLI options string
        cli_map = {}  # key -> cli_arg
        flag_keys = set()
        if settings_meta:
            for s in settings_meta:
                cli_arg = s.get('cli_arg', f'--{s["key"]}')
                cli_map[s['key']] = cli_arg
                if s.get('type') == 'boolean':
                    flag_keys.add(s['key'])

        # Reverse SHORT_TO_LONG for output (prefer long form)
        parts = []
        for key, val in settings.items():
            if not val and val != '0':
                continue
            cli_arg = cli_map.get(key, f'--{key}')
            if key in flag_keys:
                if val == 'true':
                    parts.append(cli_arg)
            else:
                # Quote value if it contains spaces
                if ' ' in val:
                    parts.append(f'{cli_arg} "{val}"')
                else:
                    parts.append(f'{cli_arg} {val}')

        opts_str = ' '.join(parts)

        # Rewrite the file (create if missing)
        if os.path.exists(path):
            with open(path, 'r') as f:
                lines = f.readlines()
        else:
            lines = []

        found = False
        new_lines = []
        for line in lines:
            stripped = line.strip()
            m = re.match(rf'^#?\s*{re.escape(var_name)}=', stripped)
            if m and not found:
                new_lines.append(f'{var_name}="{opts_str}"\n')
                found = True
            elif m and found and not stripped.startswith('#'):
                continue  # Skip duplicate uncommented lines
            else:
                new_lines.append(line)

        if not found:
            new_lines.append(f'{var_name}="{opts_str}"\n')

        with open(path, 'w') as f:
            f.writelines(new_lines)
