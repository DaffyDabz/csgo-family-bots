#!/bin/bash
# One-command control for the CS:GO legacy server. Run as root in WSL:
#   /opt/csgo/csgo-ctl.sh start|stop|restart|status|logs [n]|cmd <rcon text>|assemble
cd /opt/csgo || exit 1
PORT=27025
ensure_pw() {
  [ -s /opt/csgo/rcon_password ] && return
  python3 -c 'import secrets; print(secrets.token_urlsafe(18))' > /opt/csgo/rcon_password
  chmod 600 /opt/csgo/rcon_password
}
case "${1:-}" in
  start)   ensure_pw; docker compose up -d --build ;;
  stop)    docker compose stop ;;
  restart) ensure_pw; docker compose stop && docker compose up -d ;;
  assemble) docker compose stop; bash /opt/csgo/assemble.sh && ensure_pw && docker compose up -d --build ;;
  # status speaks the DEV panel's dialect: "csgo: running (pid N)",
  # "port 27025/udp: open", "players: n/max" (humans only, from rcon status;
  # no players line while it is still loading = unknown, so it is never slept then).
  status)  if [ "$(docker inspect -f '{{.State.Running}}' csgo 2>/dev/null)" = true ]; then
             echo "csgo: running (pid $(docker inspect -f '{{.State.Pid}}' csgo))"
             if ss -Hlun "sport = :$PORT" | grep -q .; then echo "port $PORT/udp: open"
             else echo "port $PORT/udp: closed (loading or down)"; fi
             python3 /opt/csgo/rcon.py --players 2>/dev/null
           else
             echo "csgo: stopped"
           fi ;;
  logs)    docker logs --tail "${2:-80}" csgo 2>&1 ;;
  cmd)     shift; python3 /opt/csgo/rcon.py "$@"; echo ;;
  *) echo "usage: $0 start|stop|restart|status|logs [n]|cmd <rcon text>|assemble"; exit 2 ;;
esac
