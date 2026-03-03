#!/usr/bin/env python3
"""
Diretta Web UI — Lightweight configuration interface.

Standalone HTTP server that reads/writes Diretta config files
and restarts the associated systemd service.

Usage:
    python3 diretta_webui.py --profile profiles/diretta_renderer.json [--port 8080]
"""

import argparse
import json
import os
import subprocess
import sys
from html import escape
from http.server import HTTPServer, BaseHTTPRequestHandler
from string import Template
from urllib.parse import parse_qs

from config_parser import ShellVarConfig, CliOptsConfig

# Directory where this script lives
BASE_DIR = os.path.dirname(os.path.abspath(__file__))


def load_profile(path):
    """Load a JSON profile describing the settings for a product."""
    with open(path, 'r') as f:
        return json.load(f)


def get_parser(profile):
    """Return the appropriate config parser based on profile type."""
    config_type = profile.get('config_type', 'shell_vars')
    if config_type == 'cli_opts':
        return CliOptsConfig()
    return ShellVarConfig()


def load_current_settings(profile):
    """Load current settings from the config file.

    Supports mixed config types: groups can override the profile-level
    config_type with their own (e.g. shell_vars group in a cli_opts profile).
    """
    config_path = profile['config_path']
    if not os.path.exists(config_path):
        return {}

    config_type = profile.get('config_type', 'shell_vars')
    if config_type == 'cli_opts':
        # Collect CLI opts settings (groups without config_type override)
        cli_settings_meta = []
        for g in profile.get('groups', []):
            if g.get('config_type') != 'shell_vars':
                cli_settings_meta.extend(g.get('settings', []))

        settings = CliOptsConfig.load(config_path,
                                      var_name=profile.get('config_var', 'OPTS'),
                                      settings_meta=cli_settings_meta)

        # Also load shell_vars groups from the same config file
        shell_data = ShellVarConfig.load(config_path)
        for g in profile.get('groups', []):
            if g.get('config_type') == 'shell_vars':
                for s in g.get('settings', []):
                    key = s['key']
                    if key in shell_data:
                        settings[key] = shell_data[key]

        return settings
    return ShellVarConfig.load(config_path)


def save_settings(profile, settings):
    """Write settings back to the config file.

    For cli_opts profiles with shell_vars groups, CLI opts are saved to the
    config variable and shell vars are saved as separate KEY=VALUE lines.
    """
    config_path = profile['config_path']
    config_type = profile.get('config_type', 'shell_vars')

    if config_type == 'cli_opts':
        # Identify which keys belong to shell_vars groups
        shell_var_keys = set()
        cli_settings_meta = []
        for g in profile.get('groups', []):
            if g.get('config_type') == 'shell_vars':
                for s in g.get('settings', []):
                    shell_var_keys.add(s['key'])
            else:
                cli_settings_meta.extend(g.get('settings', []))

        # Split settings into CLI opts and shell vars
        cli_settings = {k: v for k, v in settings.items()
                        if k not in shell_var_keys}
        shell_settings = {k: v for k, v in settings.items()
                          if k in shell_var_keys}

        # Save CLI opts to the config variable
        CliOptsConfig.save(config_path,
                           var_name=profile.get('config_var', 'OPTS'),
                           settings=cli_settings,
                           settings_meta=cli_settings_meta)

        # Save shell vars as separate KEY=VALUE lines
        if shell_settings:
            ShellVarConfig.save(config_path, shell_settings)
    else:
        ShellVarConfig.save(config_path, settings)


def restart_service(service_name):
    """Restart a systemd service. Returns (success, message)."""
    try:
        result = subprocess.run(
            ['systemctl', 'restart', service_name],
            capture_output=True, text=True, timeout=15
        )
        if result.returncode == 0:
            return True, f'Service {service_name} restarted.'
        return False, f'Restart failed: {result.stderr.strip()}'
    except subprocess.TimeoutExpired:
        return False, 'Restart timed out (15s).'
    except FileNotFoundError:
        return False, 'systemctl not found.'


def render_setting_input(setting, current_value):
    """Render an HTML input element for a single setting."""
    key = setting['key']
    stype = setting.get('type', 'text')
    value = escape(str(current_value)) if current_value else ''

    if stype == 'select':
        options_html = ''
        for opt in setting.get('options', []):
            selected = ' selected' if opt['value'] == current_value else ''
            options_html += (
                f'<option value="{escape(opt["value"])}"'
                f'{selected}>{escape(opt["label"])}</option>\n'
            )
        return f'<select name="{key}">\n{options_html}</select>'

    if stype == 'boolean':
        checked = ' checked' if current_value == 'true' else ''
        return (
            f'<input type="checkbox" name="{key}" value="true"{checked}'
            f' style="width:auto">'
        )

    if stype == 'number':
        attrs = f'type="number" name="{key}" value="{value}"'
        attrs += f' placeholder="{escape(str(setting.get("default", "")))}"'
        if 'min' in setting:
            attrs += f' min="{setting["min"]}"'
        if 'max' in setting:
            attrs += f' max="{setting["max"]}"'
        return f'<input {attrs}>'

    # Default: text input
    placeholder = escape(str(setting.get('default', '')))
    return (
        f'<input type="text" name="{key}" value="{value}"'
        f' placeholder="{placeholder}">'
    )


def render_groups_html(profile, current_settings):
    """Render all setting groups as HTML."""
    html = ''
    for group in profile.get('groups', []):
        collapsed = group.get('collapsed', False)
        body_class = ' collapsed' if collapsed else ''
        toggle_class = ' open' if not collapsed else ''

        settings_html = ''
        for s in group.get('settings', []):
            key = s['key']
            current = current_settings.get(key, '')
            desc = s.get('description', '')
            desc_html = (
                f'<div class="description">{escape(desc)}</div>'
                if desc else ''
            )
            input_html = render_setting_input(s, current)
            settings_html += (
                f'<div class="setting">\n'
                f'  <label>{escape(s["label"])}</label>\n'
                f'  {desc_html}\n'
                f'  {input_html}\n'
                f'</div>\n'
            )

        html += (
            f'<div class="group">\n'
            f'  <div class="group-header">\n'
            f'    <h2>{escape(group["name"])}</h2>\n'
            f'    <span class="toggle{toggle_class}">&#9654;</span>\n'
            f'  </div>\n'
            f'  <div class="group-body{body_class}">\n'
            f'    {settings_html}\n'
            f'  </div>\n'
            f'</div>\n'
        )
    return html


def render_page(profile, current_settings, flash=None):
    """Render the complete HTML page."""
    template_path = os.path.join(BASE_DIR, 'templates', 'index.html')
    with open(template_path, 'r') as f:
        template = Template(f.read())

    flash_html = ''
    if flash:
        css_class = 'success' if flash[0] else 'error'
        flash_html = (
            f'<div class="flash {css_class}">{escape(flash[1])}</div>'
        )

    groups_html = render_groups_html(profile, current_settings)

    return template.safe_substitute(
        product_name=escape(profile.get('product_name', 'Diretta')),
        flash_html=flash_html,
        groups_html=groups_html,
    )


class ConfigHandler(BaseHTTPRequestHandler):
    """HTTP request handler for the configuration web UI."""

    profile = None  # Set by main()

    def log_message(self, format, *args):
        """Override to use simple logging."""
        print(f'[webui] {args[0]}')

    def _send_html(self, html, status=200):
        self.send_response(status)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(html.encode('utf-8'))))
        self.end_headers()
        self.wfile.write(html.encode('utf-8'))

    def _send_redirect(self, location):
        self.send_response(303)
        self.send_header('Location', location)
        self.end_headers()

    def do_GET(self):
        if self.path.startswith('/static/'):
            self._serve_static()
            return

        if self.path == '/favicon.ico':
            self.send_response(204)
            self.end_headers()
            return

        # Main page (with optional flash from query string)
        flash = None
        if '?' in self.path:
            qs = parse_qs(self.path.split('?', 1)[1])
            if 'ok' in qs:
                flash = (True, qs['ok'][0])
            elif 'err' in qs:
                flash = (False, qs['err'][0])

        current = load_current_settings(self.profile)
        html = render_page(self.profile, current, flash)
        self._send_html(html)

    def do_POST(self):
        # Read form data
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length).decode('utf-8')
        form = parse_qs(body, keep_blank_values=True)

        if self.path == '/save':
            self._handle_save(form)
        elif self.path == '/restart':
            self._handle_restart()
        else:
            self._send_redirect('/')

    def _handle_save(self, form):
        """Save config and restart service."""
        # Build settings dict from form data
        settings = {}
        for group in self.profile.get('groups', []):
            for s in group.get('settings', []):
                key = s['key']
                if key in form:
                    settings[key] = form[key][0]
                else:
                    settings[key] = ''

        try:
            save_settings(self.profile, settings)
        except Exception as e:
            self._send_redirect(f'/?err=Save failed: {e}')
            return

        service = self.profile.get('service_name', '')
        if service:
            ok, msg = restart_service(service)
            if ok:
                self._send_redirect(f'/?ok=Settings saved. {msg}')
            else:
                self._send_redirect(f'/?err=Settings saved but restart failed: {msg}')
        else:
            self._send_redirect('/?ok=Settings saved.')

    def _handle_restart(self):
        """Restart service only (no config change)."""
        service = self.profile.get('service_name', '')
        if not service:
            self._send_redirect('/?err=No service configured.')
            return

        ok, msg = restart_service(service)
        if ok:
            self._send_redirect(f'/?ok={msg}')
        else:
            self._send_redirect(f'/?err={msg}')

    def _serve_static(self):
        """Serve static files (CSS)."""
        # Sanitize path to prevent directory traversal
        rel_path = self.path[len('/static/'):]
        if '..' in rel_path or rel_path.startswith('/'):
            self.send_response(403)
            self.end_headers()
            return

        file_path = os.path.join(BASE_DIR, 'static', rel_path)
        if not os.path.isfile(file_path):
            self.send_response(404)
            self.end_headers()
            return

        content_types = {
            '.css': 'text/css',
            '.js': 'application/javascript',
            '.png': 'image/png',
            '.ico': 'image/x-icon',
        }
        ext = os.path.splitext(file_path)[1]
        content_type = content_types.get(ext, 'application/octet-stream')

        with open(file_path, 'rb') as f:
            data = f.read()

        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    parser = argparse.ArgumentParser(description='Diretta Web Configuration UI')
    parser.add_argument('--profile', required=True,
                        help='Path to JSON profile (e.g. profiles/diretta_renderer.json)')
    parser.add_argument('--port', type=int, default=8080,
                        help='HTTP server port (default: 8080)')
    parser.add_argument('--bind', default='0.0.0.0',
                        help='Bind address (default: 0.0.0.0)')
    args = parser.parse_args()

    profile = load_profile(args.profile)
    ConfigHandler.profile = profile

    server = HTTPServer((args.bind, args.port), ConfigHandler)
    product = profile.get('product_name', 'Diretta')
    print(f'[webui] {product} configuration UI')
    print(f'[webui] Listening on http://{args.bind}:{args.port}')
    print(f'[webui] Config file: {profile.get("config_path", "N/A")}')

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\n[webui] Shutting down.')
        server.server_close()


if __name__ == '__main__':
    main()
