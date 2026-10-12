#!/usr/bin/env bash
# 把编译日志里的错误行做成一条 Actions 注解。注解不登录也能通过 API 读到，日志不行。
log="$1"
lines=$(grep -E -A4 'error[: ]|Error|undefined (reference|symbol)|FAILED' "$log" | head -n 80)
[ -z "$lines" ] && lines=$(tail -n 40 "$log")
message=$(printf '%s' "$lines" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | sed ':a;N;$!ba;s/\n/%0A/g')
echo "::error title=编译失败::${message}"
