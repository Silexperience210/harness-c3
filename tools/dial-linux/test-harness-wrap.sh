#!/usr/bin/env bash
# test-harness-wrap.sh — prouve chaque branche de ~/.hermes/scripts/harness-wrap.
#
# Coût nul : le « moteur » est un faux `claude` (echo + exit 7) dans un dossier privé.
# Isolé : un serveur tmux privé (socket dans un dossier temporaire) ; le serveur du
# daemon et ses sessions ne sont jamais contactés. Environnement vidé (env -i) pour
# que les variables d'agent de l'appelant (CLAUDECODE, HERMES_*) ne faussent rien.

if [ -z "${HW_TEST_CLEAN-}" ]; then
	exec env -i HW_TEST_CLEAN=1 HOME="$HOME" USER="${USER-}" LANG="${LANG:-C.UTF-8}" \
		PATH="$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin" TERM=xterm-256color bash "$0" "$@"
fi

HW_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
WRAP=${HW_WRAP:-$HW_DIR/harness-wrap}
T=$(mktemp -d /tmp/hw-test.XXXXXX)
SOCK=$T/sock
TMUX_BIN=$(command -v tmux)
export HARNESS_WRAP_SOCKET=$SOCK XDG_RUNTIME_DIR=$T/run HW_T=$T
mkdir -p -m 700 "$T/run" "$T/bin" "$T/wrapbin" "$T/notmux" "$T/badtmux" "$T/work"

pass=0 fail=0
ok() { pass=$((pass + 1)); printf '  ok    %s\n' "$*"; }
ko() { fail=$((fail + 1)); printf '  ÉCHEC %s\n' "$*"; }
check() { local what=$1; shift; if "$@"; then ok "$what"; else ko "$what"; fi; }
section() { printf '\n== %s\n' "$*"; }
st() { "$TMUX_BIN" -S "$SOCK" "$@"; }
nsess() { st list-sessions 2>/dev/null | wc -l; }
# Lance une commande dans un vrai pseudo-terminal (stdin et stdout sont des tty) et
# rend son code de sortie ; stdin vide, rien n'est tapé dans le terminal.
pty() {
	python3 -c 'import os, pty, sys; sys.exit(os.waitstatus_to_exitcode(pty.spawn(["/bin/sh", "-c", sys.argv[1]])))' "$1" </dev/null
}

cleanup() { st kill-server 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

# Faux moteur : laisse des preuves dans $HW_T, puis exit 7.
cat >"$T/bin/claude" <<'EOF'
#!/usr/bin/env bash
echo "FAKE-OUT args=[$*]"
echo "FAKE-ERR" >&2
if [ -n "${TMUX-}" ]; then
	tmux display-message -p -t "$TMUX_PANE" '#{session_name}' >"$HW_T/where"
	tmux list-sessions -F '#{session_name}' >"$HW_T/sessions-during"
	tmux show-options -v -t "$TMUX_PANE" detach-on-destroy >"$HW_T/dod" 2>/dev/null
else
	echo "(hors tmux)" >"$HW_T/where"
fi
printf '%s\n' "$PWD" >"$HW_T/pwd"
printf '%s\n' "${HW_PROBE-}" >"$HW_T/probe"
[ -n "${FAKE_SLEEP-}" ] && sleep "$FAKE_SLEEP"
[ -n "${FAKE_CLIENTS-}" ] && tmux list-clients -F '#{client_session}' >"$HW_T/clients-during"
exit 7
EOF
chmod +x "$T/bin/claude"
ln -s "$WRAP" "$T/wrapbin/claude"  # mode « interception par le PATH »
# Le faux claude passe devant le vrai partout : aucun test ne peut atteindre le vrai.
export PATH=$T/bin:$PATH
BASE_PATH=$PATH
WRAP_PATH=$T/wrapbin:$PATH
reset_evidence() { rm -f "$T/where" "$T/sessions-during" "$T/pwd" "$T/probe" "$T/clients-during" "$T/dod"; }

# ---------------------------------------------------------------------------
section "1. Non interactif : sortie et code strictement identiques au binaire nu"
cd "$T/work" || exit 1
PATH=$BASE_PATH claude -c "a b" --flag </dev/null >"$T/nu.out" 2>"$T/nu.err"; rc_nu=$?
PATH=$WRAP_PATH claude -c "a b" --flag </dev/null >"$T/w.out" 2>"$T/w.err"; rc_w=$?
"$WRAP" claude -c "a b" --flag </dev/null >"$T/d.out" 2>"$T/d.err"; rc_d=$?
echo "  binaire nu : rc=$rc_nu stdout=$(cat "$T/nu.out")"
echo "  via PATH   : rc=$rc_w stdout=$(cat "$T/w.out")"
echo "  appel direct (harness-wrap claude …) : rc=$rc_d stdout=$(cat "$T/d.out")"
check "code de sortie 7 identique (stdin </dev/null, stdout fichier)" [ "$rc_nu" = 7 ] && [ "$rc_w" = 7 ] && [ "$rc_d" = 7 ]
check "stdout identique octet pour octet (PATH et appel direct)" cmp -s "$T/nu.out" "$T/w.out" && cmp -s "$T/nu.out" "$T/d.out"
check "stderr identique octet pour octet" cmp -s "$T/nu.err" "$T/w.err"
check "aucune session tmux créée" [ "$(nsess)" = 0 ]

PATH=$BASE_PATH claude -p x </dev/null 2>/dev/null | cat >"$T/nu2.out"; rc_nu=${PIPESTATUS[0]}
PATH=$WRAP_PATH claude -p x </dev/null 2>/dev/null | cat >"$T/w2.out"; rc_w=${PIPESTATUS[0]}
check "stdout tube : identique, rc $rc_nu = $rc_w" [ "$rc_w" = 7 ] && cmp -s "$T/nu2.out" "$T/w2.out"

pty "PATH=$WRAP_PATH claude out-redirige >$T/w3.out 2>$T/w3.err"; rc=$?
check "stdin tty mais stdout redirigé → pass-through, rc=$rc" [ "$rc" = 7 ] && grep -q 'out-redirige' "$T/w3.out" && [ "$(nsess)" = 0 ]

reset_evidence
out=$(pty "PATH=$WRAP_PATH claude -p 'question' --output-format json"); rc=$?
check "tty + -p → pass-through (rc=$rc, hors tmux, 0 session)" [ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where" && [ "$(nsess)" = 0 ]
reset_evidence
pty "PATH=$WRAP_PATH claude -cp 'q'" >/dev/null; rc=$?
check "tty + -cp groupé → pass-through (rc=$rc)" [ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where"
for args in "--output-format=json" "--input-format stream-json" "--no-session-persistence" "--print" "--version" "mcp list" "update"; do
	reset_evidence
	pty "PATH=$WRAP_PATH claude $args" >/dev/null; rc=$?
	check "tty + « $args » → pass-through (rc=$rc)" [ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where" && [ "$(nsess)" = 0 ]
done

section "1b. hermes : seules la session interactive et « chat » sont enveloppées"
ln -s "$T/bin/claude" "$T/bin/hermes"; ln -s "$WRAP" "$T/wrapbin/hermes"
for args in "gateway status" "cron list" "-z 'q'" "chat -q x --oneshot" "chat -Q -q x" "--version"; do
	reset_evidence
	pty "PATH=$WRAP_PATH hermes $args" >/dev/null; rc=$?
	check "tty + hermes $args → pass-through (rc=$rc)" [ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where" && [ "$(nsess)" = 0 ]
done
for args in "" "chat" "-m modele chat" "--continue"; do
	reset_evidence
	pty "cd $T/work && PATH=$WRAP_PATH hermes $args" >/dev/null; rc=$?
	check "tty + hermes ${args:-(sans argument)} → session $(cat "$T/where" 2>/dev/null) (rc=$rc)" [ "$rc" = 7 ] && grep -q '^harness-hermes-work' "$T/where"
done

# ---------------------------------------------------------------------------
section "2. HARNESS_WRAP=0 et contexte d'agent → pass-through"
reset_evidence
out=$(pty "HARNESS_WRAP=0 PATH=$WRAP_PATH claude salut"); rc=$?
check "HARNESS_WRAP=0 : rc=$rc, hors tmux, 0 session, aucune ligne du lanceur" \
	[ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where" && [ "$(nsess)" = 0 ] && [ "$(grep -c harness-wrap <<<"$out")" = 0 ]
for v in CLAUDECODE=1 HERMES_SESSION_KEY=abc HERMES_AGENT=true; do
	reset_evidence
	pty "$v PATH=$WRAP_PATH claude salut" >/dev/null; rc=$?
	check "$v (appel par un agent) → pass-through (rc=$rc)" [ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where" && [ "$(nsess)" = 0 ]
done

# ---------------------------------------------------------------------------
section "3. Hors tmux, interactif → session harness-* créée puis attachée"
reset_evidence
out=$(pty "cd $T/work && dir=/nulle-part name=piege i=999 HW_PROBE=sonde-42 FAKE_SLEEP=1 FAKE_CLIENTS=1 PATH=$WRAP_PATH claude bonjour 'deux mots'"); rc=$?
printf '%s\n' "$out" | tr -d '\r' | grep -a 'harness-wrap' | sed 's/^/  stderr> /'
where=$(cat "$T/where" 2>/dev/null)
echo "  le moteur tournait dans : $where ; clients pendant : $(cat "$T/clients-during" 2>/dev/null)"
check "le moteur a tourné dans une session harness-*" [ "${where#harness-claude-work}" != "$where" ]
check "le terminal était attaché à cette session" grep -qx "$where" "$T/clients-during"
check "detach-on-destroy=on posé (attach rend la main à la fin)" grep -qx on "$T/dod"
check "arguments avec espaces transmis intacts" grep -aq "args=\[bonjour deux mots\]" <<<"$out"
check "répertoire courant conservé ($(cat "$T/pwd"))" [ "$(cat "$T/pwd")" = "$T/work" ]
check "environnement de l'appelant transmis (HW_PROBE=$(cat "$T/probe"), même avec dir/name/i exportés)" [ "$(cat "$T/probe")" = sonde-42 ]
check "code de sortie 7 rendu au terminal (rc=$rc)" [ "$rc" = 7 ]
check "une ligne d'explication sur stderr" [ "$(printf '%s\n' "$out" | grep -ac 'lancé dans la session tmux')" = 1 ]
check "session terminée avec le moteur (0 session restante)" [ "$(nsess)" = 0 ]
check "aucun fichier d'état oublié" [ -z "$(ls "$T/run" | grep harness-wrap\\.)" ]

# ---------------------------------------------------------------------------
section "4. Déjà dans une session harness-* → aucune imbrication"
reset_evidence
st new-session -d -s harness-deja -x 80 -y 24 \
	"cd $T/work; HW_PROBE=imbr PATH=$WRAP_PATH claude dedans >$T/deja.out 2>$T/deja.err; echo \$? >$T/deja.rc; sleep 1"
sleep 1.5
echo "  sessions vues par le moteur : $(tr '\n' ' ' <"$T/sessions-during" 2>/dev/null)"
check "le moteur a tourné dans harness-deja même" grep -qx harness-deja "$T/where"
check "1 seule session sur le serveur pendant l'exécution (avant = après)" [ "$(wc -l <"$T/sessions-during")" = 1 ]
check "rc=7 et stderr muet" [ "$(cat "$T/deja.rc")" = 7 ] && [ "$(cat "$T/deja.err")" = FAKE-ERR ]
st kill-session -t =harness-deja 2>/dev/null

# ---------------------------------------------------------------------------
section "5. Dans un tmux non harness- → session visible créée, bascule, retour"
reset_evidence
st new-session -d -s dev-travail -x 80 -y 24 "bash --norc"
pty "tmux -S $SOCK attach -t =dev-travail" >/dev/null &
pty_pid=$!
sleep 0.5
before=$(st list-sessions -F '#{session_name}' | tr '\n' ' ')
st send-keys -t dev-travail: "cd $T/work; FAKE_SLEEP=1 FAKE_CLIENTS=1 PATH=$WRAP_PATH claude ici 2>$T/sw.err; echo \$? >$T/sw.rc" Enter
for _ in $(seq 50); do [ -s "$T/sw.rc" ] && break; sleep 0.2; done
sleep 0.3
echo "  sessions avant : $before"
echo "  le moteur tournait dans : $(cat "$T/where" 2>/dev/null) ; client pendant : $(cat "$T/clients-during" 2>/dev/null)"
sed 's/^/  stderr> /' "$T/sw.err"
check "le moteur a tourné dans une session harness-*" grep -q '^harness-claude-work' "$T/where"
check "le client a été basculé dessus" grep -q '^harness-claude-work' "$T/clients-during"
check "detach-on-destroy=off posé (retour à la dernière session active)" grep -qx off "$T/dod"
check "l'utilisateur est informé (1 ligne nommant la session)" [ "$(grep -c 'nouvelle session « harness-' "$T/sw.err")" = 1 ]
check "rc=7 rendu au shell d'origine" [ "$(cat "$T/sw.rc" 2>/dev/null)" = 7 ]
echo "  client après la fin : $(st list-clients -F '#{client_session}')"
check "client revenu sur dev-travail à la fin" [ "$(st list-clients -F '#{client_session}')" = dev-travail ]
check "dev-travail intacte, pas renommée ; aucune session laissée" [ "$(st list-sessions -F '#{session_name}')" = dev-travail ]
st kill-session -t =dev-travail 2>/dev/null; wait "$pty_pid" 2>/dev/null

# ---------------------------------------------------------------------------
section "6. tmux absent du PATH → pass-through + un seul avertissement, rc préservé"
for t in readlink id grep head cat mktemp env date tr cut stty sleep rm mv chmod mkdir bash sh python3; do ln -s "$(command -v $t)" "$T/notmux/$t"; done
reset_evidence
out1=$(pty "PATH=$T/wrapbin:$T/bin:$T/notmux claude sans-tmux"); rc1=$?
out2=$(pty "PATH=$T/wrapbin:$T/bin:$T/notmux claude sans-tmux"); rc2=$?
printf '%s\n' "$out1" | tr -d '\r' | sed 's/^/  1er> /'
check "rc=7 préservé (1er : $rc1, 2e : $rc2)" [ "$rc1" = 7 ] && [ "$rc2" = 7 ]
check "la commande a tourné normalement" grep -aq 'args=\[sans-tmux\]' <<<"$out1" && grep -q 'hors tmux' "$T/where"
check "avertissement au 1er lancement" grep -aq 'tmux introuvable' <<<"$out1"
check "pas répété au 2e" bash -c '! grep -aq "tmux introuvable" <<<"$1"' _ "$out2"

section "6b. tmux cassé (renvoie une erreur) → pass-through, jamais bloqué"
printf '#!/bin/sh\necho "tmux cassé" >&2\nexit 1\n' >"$T/badtmux/tmux"; chmod +x "$T/badtmux/tmux"
reset_evidence
out=$(timeout 20 bash -c "$(declare -f pty); pty 'PATH=$T/wrapbin:$T/bin:$T/badtmux:$PATH claude casse'"); rc=$?
printf '%s\n' "$out" | tr -d '\r' | grep -a harness-wrap | sed 's/^/  stderr> /'
check "rc=7, commande lancée hors tmux" [ "$rc" = 7 ] && grep -q 'hors tmux' "$T/where"

# ---------------------------------------------------------------------------
section "7. Nom de session déjà pris → suffixe horodaté, rien d'écrasé"
reset_evidence
st new-session -d -s harness-claude-work -x 80 -y 24 "sleep 300"
pid_avant=$(st display-message -p -t '=harness-claude-work:' '#{pane_pid}')
out=$(pty "cd $T/work && FAKE_SLEEP=1 PATH=$WRAP_PATH claude occupe"); rc=$?
where=$(cat "$T/where" 2>/dev/null)
echo "  session prise : harness-claude-work ; moteur lancé dans : $where"
check "suffixe horodaté (harness-claude-work-AAAAMMJJ-HHMMSS)" grep -qxE 'harness-claude-work-[0-9]{8}-[0-9]{6}' <<<"$where"
check "session existante intacte (même processus $pid_avant)" [ -n "$pid_avant" ] && [ "$(st display-message -p -t '=harness-claude-work:' '#{pane_pid}')" = "$pid_avant" ]
check "rc=7" [ "$rc" = 7 ]
st kill-session -t =harness-claude-work

# ---------------------------------------------------------------------------
section "8. Session tuée de l'extérieur → le lanceur rend la main"
reset_evidence
( sleep 1.5; st kill-session -t =harness-claude-work ) &
t0=$(date +%s)
out=$(timeout 20 bash -c "$(declare -f pty); pty 'cd $T/work && FAKE_SLEEP=60 PATH=$WRAP_PATH claude long'"); rc=$?
check "retour en $(($(date +%s) - t0)) s avec rc=$rc et un message" [ "$rc" = 129 ] && grep -aq 'fermée de l' <<<"$out"

# ---------------------------------------------------------------------------
section "9. Détection des moteurs"
"$WRAP" --engines | sed 's/^/  /'
check "table extraite de cli.js (≥ 14 moteurs)" [ "$(cut -d' ' -f2 "$HW_DIR/harness-wrap.engines" | sort -u | wc -l)" -ge 14 ]
check "commande inconnue → 127 comme bash" bash -c "'$WRAP' commande-qui-nexiste-pas </dev/null 2>/dev/null; [ \$? = 127 ]"

printf '\n== Bilan : %d ok, %d échec(s) ; sessions restantes sur le serveur de test : %s\n' "$pass" "$fail" "$(nsess)"
[ "$fail" = 0 ]
