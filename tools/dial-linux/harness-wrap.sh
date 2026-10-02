# harness-wrap.sh — à sourcer depuis ~/.bashrc (ou ~/.zshrc). Ne fait rien d'autre
# que définir des fonctions ; aucune commande n'est lancée au chargement, hormis la
# régénération (rapide, bornée) de la liste des moteurs quand le daemon a été mis à jour.
#
# Quoi fait quoi :
#   ~/.hermes/scripts/harness-wrap          le lanceur ; décide pass-through ou session tmux harness-*
#   ~/.hermes/scripts/harness-wrap.engines  table « exécutable moteur » extraite de ~/.harness/cli/cli.js
#   ce fichier                              une fonction par moteur INSTALLÉ (claude, hermes…) qui
#                                           appelle le lanceur, + la fonction générique harness_wrap
#
#   harness_wrap <commande> [args]   lance n'importe quelle commande via le lanceur
#   command claude …  /  \claude …   contourne la fonction pour un appel
#   HARNESS_WRAP=0 claude …          le lanceur passe la main sans rien faire
#
# hermes-agent n'a pas de fonction : c'est un point d'entrée d'automatisation.

# Le dossier est deduit de CE fichier (clone du depot ou installation), avec repli
# sur l'emplacement d'installation pour ne pas casser un ~/.bashrc existant.
_hw_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd -P) || _hw_dir="$HOME/.hermes/scripts"
[ -x "$_hw_dir/harness-wrap" ] || _hw_dir="$HOME/.hermes/scripts"
: "${HARNESS_WRAP_BIN:=$_hw_dir/harness-wrap}"

if [ -x "$HARNESS_WRAP_BIN" ]; then
	harness_wrap() { "$HARNESS_WRAP_BIN" "$@"; }

	_hw_cache="${HARNESS_WRAP_ENGINES:-$_hw_dir/harness-wrap.engines}"
	if [ ! -s "$_hw_cache" ] || [ "$HOME/.harness/cli/cli.js" -nt "$_hw_cache" ]; then
		"$HARNESS_WRAP_BIN" --refresh-engines >/dev/null 2>&1
	fi
	if [ -s "$_hw_cache" ]; then
		_hw_list=$(cat "$_hw_cache")
	else  # secours : noms d'exécutables connus du daemon au 02/10/2026
		_hw_list='claude
codex
agent
cursor-agent
opencode
pi
hermes
cmd
command-code
commandcode
devin
muse
amp
kilo
kilocode
grok
agy
copilot'
	fi
	# while/here-doc : boucle dans la coquille courante, découpage identique en bash et zsh.
	while read -r _hw_n _hw_id; do
		case $_hw_n in hermes-agent|*[!a-z0-9-]*|'') continue ;; esac
		if [ -n "${ZSH_VERSION-}" ]; then _hw_p=$(whence -p "$_hw_n" 2>/dev/null); else _hw_p=$(type -P "$_hw_n" 2>/dev/null); fi
		[ -n "$_hw_p" ] || continue
		# « function nom { } » : insensible à un alias du même nom, valable en bash et zsh.
		eval "function $_hw_n { \"\$HARNESS_WRAP_BIN\" $_hw_n \"\$@\"; }"
	done <<EOF
$_hw_list
EOF
	unset _hw_cache _hw_list _hw_n _hw_id _hw_p
fi
