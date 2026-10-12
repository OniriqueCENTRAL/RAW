# RAW, plugin synthé (version 0.3 : moteur, interface et effets)

## Contenu

- Le son : 2 oscillateurs wavetable identiques (position, level, tune, unison 1 à 5 voix,
  detune, pan, ADSR, filtre passe-bas résonant, LFO vers le cutoff), mix add / fm / ring,
  jeu de la vie qui module pos a, pos b ou le cutoff, 16 voix, notes CLAP et MIDI.
- L'interface RAW dans la fenêtre du plugin : oscillateurs, grille de vie et scope alimentés
  par le moteur, menu unique, 500 presets, couleur, tunnel ASCII et ondes à chaque note
  (y compris les notes jouées par le DAW), clavier cliquable.
- Les gestes faits dans l'interface sont signalés au DAW : l'automation s'enregistre.
- Les 10 effets dans le panneau fx, à droite : hyper/dimension, distortion, flanger, phaser,
  chorus, delay, compressor, eq, filter, reverb. Clique un effet pour voir ses réglages,
  `< on >` pour l'allumer. Un `*` devant le nom signale un effet allumé.
- Les 500 presets règlent aussi les effets.
- Les réglages sont sauvegardés dans le projet du DAW.

## Licence

Le dossier `License/` est le module de licence ONIRIQUE, le même dans tous les plugins (voir
`License/README.md`) : sans licence, bruit blanc à l'export et par salves à l'écoute ; la pastille de
l'en-tête ouvre la page d'activation.

## Formats produits

| Fichier       | Windows | Mac | DAW concernés |
|---------------|---------|-----|---------------|
| RAW.vst3      | oui     | oui | Ableton Live, FL Studio, Cubase, Studio One, Reaper, Bitwig |
| RAW.component | non     | oui | Logic Pro, GarageBand (format AU) |
| RAW.clap      | oui     | oui | DAW compatibles CLAP, dont Bitwig et Reaper |

Pro Tools n'est pas couvert : il exige le format AAX, réservé aux développeurs inscrits chez Avid.
Dans le doute sur la compatibilité CLAP d'un DAW, utiliser le VST3.

## Aperçu de l'interface sans DAW

Ouvrir `src/ui/raw_ui.html` dans un navigateur : l'interface tourne avec des visuels simulés
(pas de son). C'est la même page que celle intégrée au plugin.

## Construire sur Windows

1. Installer Visual Studio 2022 (charge de travail "Développement Desktop en C++"),
   CMake 3.21 ou plus, et Git.
2. Dans un terminal, dans ce dossier :

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

3. Les fichiers sont dans `build/out` (sinon chercher `RAW.vst3` dans `build`). Copier :
   - `RAW.vst3` dans `C:\Program Files\Common Files\VST3`
   - `RAW.clap` dans `C:\Program Files\Common Files\CLAP`

L'interface utilise WebView2, déjà présent sur Windows 10 et 11 à jour. Sur une machine
ancienne, installer le "WebView2 Runtime" de Microsoft (gratuit).

## Construire sur Mac

1. Installer Xcode (ou `xcode-select --install`), CMake 3.21 ou plus, et Git.
2. Dans un terminal, dans ce dossier :

```
cmake -B build -G Xcode
cmake --build build --config Release
```

3. Copier depuis `build/out` :
   - `RAW.clap` dans `~/Library/Audio/Plug-Ins/CLAP`
   - `RAW.vst3` dans `~/Library/Audio/Plug-Ins/VST3`
   - `RAW.component` dans `~/Library/Audio/Plug-Ins/Components`
4. Signer localement (sinon macOS peut bloquer le plugin) :

```
codesign --force --deep -s - ~/Library/Audio/Plug-Ins/Components/RAW.component
codesign --force --deep -s - ~/Library/Audio/Plug-Ins/VST3/RAW.vst3
codesign --force --deep -s - ~/Library/Audio/Plug-Ins/CLAP/RAW.clap
```

5. Vérifier l'AU pour Logic :

```
killall -9 AudioComponentRegistrar
auval -v aumu Raw1 RaWs
```

## Vérifier le plugin compilé

- `clap-validator` (free-audio) pour RAW.clap
- `pluginval` (Tracktion) pour RAW.vst3 et RAW.component
- `auval` pour Logic (voir plus haut)

## Si la construction échoue

- Le point le plus fragile est `make_clapfirst_plugins` (clap-wrapper) : ses options peuvent
  changer d'une version à l'autre. Choisir une version publiée et la fixer :
  `cmake -B build -DRAW_CLAP_WRAPPER_TAG=<version>` puis comparer les options avec son README.
- Si le téléchargement de WebView2 échoue, choisir une autre version du paquet NuGet
  `Microsoft.Web.WebView2` : `-DRAW_WEBVIEW2_VERSION=<version>`.
- Plan B sans clap-wrapper, pour au moins avoir le .clap (interface comprise) :

```
cmake -B build -DRAW_CLAP_ONLY=ON
cmake --build build --config Release
```

Envoyer le message d'erreur exact pour correction.

## Étapes suivantes

- Finitions : sauvegarde de la couleur et du preset choisi dans le projet, presets utilisateur,
  ordre des effets modifiable.
