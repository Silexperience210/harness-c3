#!/bin/bash
# Veilleur no_agent : le CLI Harness se met à jour tout seul et efface nos correctifs Hermes.
# Silencieux si tout est en place. Sinon : réapplique, redémarre le daemon (comme le fait déjà
# sa propre mise à jour), et dit ce qu'il a fait. Motif amont changé ⇒ une ligne d'alerte.
OUT=$(/usr/bin/python3 /home/silex/.harness/repatch-hermes.py 2>&1)
RC=$?
if echo "$OUT" | grep -q '🔧'; then
    systemctl --user restart harness.service
    sleep 3
    echo "$OUT (version $(/home/silex/.local/bin/harness --version 2>/dev/null)) — daemon relancé : $(systemctl --user is-active harness.service)"
elif [ -n "$OUT" ]; then
    echo "$OUT"
fi
[ $RC -eq 1 ] && exit 1
exit 0
