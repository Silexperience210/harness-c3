#!/usr/bin/env python3
"""Réapplique au CLI Harness installé les correctifs Hermes que l'amont n'a pas encore.

Le CLI se met à jour tout seul (toutes les 60 s) et chaque mise à jour efface ces correctifs.
Ce script est idempotent : il ne touche au fichier que s'il manque un correctif, sauvegarde
l'original à côté, vérifie la syntaxe avec `node --check`, et revient en arrière sinon.

Les noms minifiés changent d'une version à l'autre : tout est repéré par des motifs
(regex) sur la structure, jamais par un nom de fonction.

  1. #383   — `harness new hermes` : n'envoyer permissionMode que si --mode/--plan est tapé.
              FUSIONNÉ EN AMONT : repris par le mainteneur dans #412 (commit e144a11, co-auteur
              Silexperience210). Le CLI installé 0.3.26 contient déjà `modeGiven` : ce correctif
              ne s'applique donc plus. La fonction reste comme garde-fou (si l'amont régresse,
              l'ancienne forme réapparaît et il est réappliqué).
  2. lock   — questions `clarify` de Hermes : pied « Enter to lock » + marqueur `▸`.
              PAS ENCORE EN AMONT — proposé par la PR #410 (rebasée, MERGEABLE).
  3. prompt — Hermes accepte un premier message (`hermes chat -q …`, reste interactif sur un TTY) :
              débloque `harness new hermes --prompt …` ET le fork « handoff » depuis le cadran.
              PAS ENCORE EN AMONT — même PR #410.

Sortie : rien si tout est déjà en place ; une ligne par correctif appliqué ; code 2 si un motif
n'est plus reconnu (l'amont a changé le code : à regarder à la main).

  --check   dit ce qui manque, sans rien écrire.
"""
import os, re, shutil, subprocess, sys

CLI = os.path.expanduser("~/.harness/cli/cli.js")


def p383(s):
    if "modeGiven" in s:
        return s, None
    m = re.search(r'(let t=\[\],r=null,i=null,s=null,)o="auto",(c=null,l=null,u=!1,d=a\(\(p,h\)=>)', s)
    if not m:
        return s, "383: parseNewArgs introuvable"
    start = m.start()
    end = s.find('"parseNewArgs")', start)
    if end < 0:
        return s, "383: fin de parseNewArgs introuvable"
    body = s[start:end]
    body2 = body.replace('o="auto",c=null', 'o="auto",mg=!1,c=null', 1)
    body2 = body2.replace('h==="--plan")o="plan";', 'h==="--plan")o="plan",mg=!0;', 1)
    body2 = body2.replace('h==="--mode")o=d(h,p),p++;', 'h==="--mode")o=d(h,p),mg=!0,p++;', 1)
    body2 = body2.replace('mode:o,prompt:c', 'mode:o,modeGiven:mg,prompt:c', 1)
    if body2.count("mg") - body.count("mg") != 4:
        return s, "383: parseNewArgs a changé de forme"
    s = s[:start] + body2 + s[end:]
    old = 'bypassPermission:!i&&(n.mode==="auto"||n.mode==="full"),...i?{}:{permissionMode:n.mode}'
    new = ('bypassPermission:!i&&n.modeGiven!==!1&&(n.mode==="auto"||n.mode==="full"),'
           '...i||n.modeGiven===!1?{}:{permissionMode:n.mode}')
    if s.count(old) != 1:
        return s, "383: newAgentPayload a changé de forme"
    return s.replace(old, new), "383 (harness new hermes)"


def plock(s):
    changed = False
    old = r"enter to (select|confirm|submit)|enter\s+(submit|confirm|toggle)"
    if "enter to (select|confirm|submit|lock)" not in s:
        if s.count(old) != 1:
            return s, "lock: regex du pied introuvable"
        s = s.replace(old, r"enter to (select|confirm|submit|lock)|enter\s+(submit|confirm|toggle)")
        changed = True
    # CLI 0.3.64 : le marqueur `▸` n'existe plus dans le texte du pied. L'extraction
    # n'a donc plus d'objet — ne plus alerter (code 2) à chaque tour pour rien.
    if "▸" in s and 'replace(/^▸\\s*/,"")' not in s:
        m = re.search(r'/\^\[─━-\]\{6,\}\$/\.test\((\w)\)\)break;(\w)=\1;break\}', s)
        if not m:
            return s, "lock: extraction de la question introuvable"
        v, p = m.group(1), m.group(2)
        s = s[:m.start()] + f'/^[─━-]{{6,}}$/.test({v}))break;{p}={v}.replace(/^▸\\s*/,"");break}}' + s[m.end():]
        changed = True
    return s, ("lock (questions Hermes)" if changed else None)


def pprompt(s):
    if 'hermes:["chat","-q"]' in s:
        return s, None
    m = re.search(r'(=\{opencode:\["--prompt"\],claude:\[\],codex:\[\],cursor:null,pi:null,)hermes:null,', s)
    if not m:
        return s, "prompt: table des premiers messages introuvable"
    return s[:m.start()] + m.group(1) + 'hermes:["chat","-q"],' + s[m.end():], "prompt (fork + --prompt Hermes)"


def main():
    check = "--check" in sys.argv
    src = open(CLI, encoding="utf-8").read()
    s, done, errors = src, [], []
    for fn in (p383, plock, pprompt):
        s, msg = fn(s)
        if msg and ":" in msg and not msg.startswith(("383 (", "lock (", "prompt (")):
            errors.append(msg)
        elif msg:
            done.append(msg)
    if check:
        print("manque : " + ", ".join(done) if done else "tout est en place")
        for e in errors:
            print("⚠️ " + e)
        return 2 if errors else 0
    if done:
        bak = CLI + ".avant-repatch"
        shutil.copy2(CLI, bak)
        with open(CLI, "w", encoding="utf-8") as f:
            f.write(s)
        r = subprocess.run(["node", "--check", CLI], capture_output=True, text=True)
        if r.returncode != 0:
            shutil.copy2(bak, CLI)
            print("❌ correctifs Harness : syntaxe cassée, original remis — " + r.stderr.strip()[:200])
            return 1
        print("🔧 Harness : correctifs réappliqués — " + ", ".join(done))
    for e in errors:
        print("⚠️ Harness : " + e + " (l'amont a changé ce code, à regarder)")
    return 2 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
