#!/usr/bin/env bash
# 把编译日志里的错误行做成一条 Actions 注解。注解不登录也能通过 API 读到，日志不行。
log="$1"
# 链接命令行很长，会把注解撑满，所以每行只留前 300 个字符。
lines=$(grep -E -A4 'error[: ]|Error|undefined (reference|symbol)|FAILED' "$log" | cut -c1-300 | head -n 80)
[ -z "$lines" ] && lines=$(tail -n 40 "$log")
message=$(printf '%s' "$lines" | sed -e 's/%/%25/g' -e 's/\r/%0D/g' | sed ':a;N;$!ba;s/\n/%0A/g')
echo "::error title=编译失败::${message}"
