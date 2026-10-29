#!/usr/bin/env bash
# 在云主机（Ubuntu，apt+systemd 发行版即可）上运行，把 dashboard/ 部署成开机自启的 systemd 服务。
#
# 本脚本只负责「服务器上的安装」，不负责把代码从开发机传上来。完整流程两步：
#
#   第 1 步 · 把 dashboard/ 弄到服务器上（二选一）
#     scp:  scp -r dashboard/ <user>@<host>:~/          # 开发机上执行
#     git:  ssh <user>@<host> 'git clone <repo> && cd <repo>'
#
#   第 2 步 · 在服务器上安装（本脚本）
#     cd ~/dashboard && sudo bash deploy/deploy.sh
#
# 注意：.env 不会被传上来（被 .gitignore 忽略，本脚本也不复制它）。这是有意为之——
# 服务器上的 DEEPSEEK_API_KEY 由本脚本创建的 /etc/socc-dashboard.env 提供，需手动填。
#
# 幂等：重复运行只会更新代码 + 重启服务，不会重复安装，也不会覆盖已填好的 key。
set -euo pipefail

APP_DIR=/opt/socc-dashboard
SERVICE_NAME=socc-dashboard
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行: sudo bash deploy/deploy.sh" >&2
    exit 1
fi

apt-get update -y
apt-get install -y python3 python3-venv python3-pip

mkdir -p "$APP_DIR" "$APP_DIR/data" "$APP_DIR/templates" "$APP_DIR/static"
cp "$SRC_DIR/server.py" "$SRC_DIR/requirements.txt" "$APP_DIR/"
cp "$SRC_DIR/templates/index.html" "$APP_DIR/templates/"
cp -R "$SRC_DIR/static/." "$APP_DIR/static/"

if [ ! -d "$APP_DIR/venv" ]; then
    python3 -m venv "$APP_DIR/venv"
fi
"$APP_DIR/venv/bin/pip" install -q --upgrade pip
"$APP_DIR/venv/bin/pip" install -q -r "$APP_DIR/requirements.txt"

# 密钥文件：只在首次创建，之后重复部署不会覆盖已填好的 key
ENV_FILE=/etc/socc-dashboard.env
if [ ! -f "$ENV_FILE" ]; then
    cat > "$ENV_FILE" <<'EOF'
# socc-dashboard 环境变量。改完执行: systemctl restart socc-dashboard
# DeepSeek API key；留空则 AI 健康报告功能关闭，其余功能照常
DEEPSEEK_API_KEY=
EOF
    chmod 600 "$ENV_FILE"
    NEED_KEY=1
fi

cp "$SRC_DIR/deploy/socc-dashboard.service" /etc/systemd/system/"$SERVICE_NAME".service
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
systemctl restart "$SERVICE_NAME"

echo
echo "部署完成。"
echo "  查看状态: systemctl status $SERVICE_NAME"
echo "  查看日志: journalctl -u $SERVICE_NAME -f"
echo "  别忘了在阿里云控制台安全组里放行 TCP 5000 入方向"
if [ "${NEED_KEY:-0}" = "1" ]; then
    echo
    echo "  [AI 报告] 已创建 $ENV_FILE (权限 600)。"
    echo "            填入 DEEPSEEK_API_KEY 后执行: systemctl restart $SERVICE_NAME"
fi
