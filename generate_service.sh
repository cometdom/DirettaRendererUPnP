#!/usr/bin/env bash

SERVICE_NAME="diretta-renderer"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

WORK_DIR="/root/diretta/DirettaRendererUPnP"
EXEC_BIN="${WORK_DIR}/bin/DirettaRendererUPnP"

TARGET_INDEX="${TARGET_INDEX:-1}"
UPNP_PORT="${UPNP_PORT:-4005}"
BUFFER_SECS="${BUFFER_SECS:-2.0}"

if [[ $EUID -ne 0 ]]; then
    echo "Please run this script as root (use sudo)."
    exit 1
fi

if [[ ! -x "${EXEC_BIN}" ]]; then
    echo "Executable not found: ${EXEC_BIN}"
    echo "Please build the project first in ${WORK_DIR}."
    exit 1
fi

cat > "${SERVICE_FILE}" <<EOF
[Unit]
Description=Diretta UPnP Renderer
After=network.target

[Service]
Type=simple
WorkingDirectory=${WORK_DIR}
ExecStart=${EXEC_BIN} --target ${TARGET_INDEX} --port ${UPNP_PORT} --buffer ${BUFFER_SECS}
Restart=on-failure
User=root

[Install]
WantedBy=multi-user.target
EOF

echo "systemd service generated: ${SERVICE_FILE}"
echo "Run the following commands to reload and start the service:"
echo "  sudo systemctl daemon-reload"
echo "  sudo systemctl enable ${SERVICE_NAME}"
echo "  sudo systemctl start ${SERVICE_NAME}"
