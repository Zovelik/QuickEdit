# QuickEdit : architecture

Logiciel de montage vidéo simple à utiliser et puissant, rendu entièrement en
Vulkan. Ce document décrit l'architecture visée et l'état actuel. Les choix
marqués *(à confirmer)* attendent une décision.

## Vue d'ensemble

```
             ┌──────────────── UI (ImGui docking, thread principal) ───────────────┐
             │ Médias │ Visionneuse │ Inspecteur │ Timeline │ Mixeur audio │ Export │
             └───────────────┬─────────────────────────────────────────────────────┘
                             │ éditions (snapshots → undo/redo)
                     ┌───────▼────────┐
                     │  core::Project │  modèle pur, sans GPU ni UI, testé
                     └───────┬────────┘
            ┌────────────────┼──────────────────────┐
   ┌────────▼───────┐ ┌──────▼────────┐    ┌────────▼────────┐
   │ media::Decoder │ │ render::      │    │ audio::Engine   │
   │ FFmpeg demux + │ │ Compositor    │    │ mixage float32, │
   │ hwaccel Vulkan │─▶ compute Vulkan│    │ VST3, sortie SDL│
   └────────────────┘ └──────┬────────┘    └────────┬────────┘
                             │ VkImage RGBA16F               │ horloge maître
                     ┌───────▼────────┐                      │
                     │ media::Encoder │◀─────────────────────┘
                     │ Vulkan Video + │
                     │ mux FFmpeg     │
                     └────────────────┘
```

Un seul `VkDevice` est partagé par tous les modules : l'UI, le compositing,
FFmpeg (décodage) et l'encodeur. Les images décodées restent sur le GPU du
début à la fin (zéro copie).

## Modules

### `core/` (fait)

- `Time.h` : le temps est un `int64` en ticks de 1/705 600 000 s. Cette unité
  divise exactement toutes les cadences courantes (24 à 120 i/s) et toutes les
  fréquences audio (44,1 à 192 kHz). Les cadences NTSC passent par `Rational`.
- `Project` : médias, séquence, pistes vidéo et audio, clips (début, durée,
  point d'entrée source, groupe de liaison vidéo/audio, opacité, gain).
  Opérations : import, ajout sans chevauchement, découpe, suppression avec les
  clips liés.
- `History` : undo/redo par snapshots JSON du projet entier, qui partagent
  leurs parties inchangées. Simple et sans risque d'oubli ; à remplacer par des
  commandes si les projets deviennent gros.
- `Json` : lecteur/écrivain JSON minimal (entiers 64 bits exacts), sans
  dépendance.

### Format de projet `.qep` (fait)

JSON lisible et versionné (`"version": 1`, refus des versions plus récentes,
migration des plus anciennes). Les chemins des médias sont relatifs au projet
quand ils sont dans son dossier. Écriture via un fichier temporaire puis
renommage, pour ne jamais laisser un projet tronqué. Les caches (vignettes,
formes d'onde, proxys) iront dans un dossier `<projet>.cache/` à côté.

### `app/` (fait, à enrichir)

Base commune : `GUIBase` (SDL3, Vulkan, swapchain, ImGui,
fenêtre sans bordure), `vkutils/`, polices et icônes. Spécifique à QuickEdit :

- `GUI` : panneaux Médias, Visionneuse, Inspecteur, Timeline, menus, barre
  d'outils, raccourcis, dialogues, lecture.
- `TimelineView` : règle, en-têtes de pistes (masquer, muet, solo, verrou),
  clips ; sélection, déplacement entre pistes, trim des bords, magnétisme
  (tête de lecture et bords des clips), zoom à la souris, glisser-déposer
  depuis les médias.

### `media/` : décodage *(fait pour la visionneuse)*

État actuel : `GUIBase` crée le device avec une file de décodage vidéo quand
le GPU en a une, et `HwDevice` le confie à FFmpeg (mêmes files, mêmes
verrous). `VideoPlayer` décode sur son propre thread (`VideoReader` : seek,
8 frames d'avance en lecture) ; les frames Vulkan Video restent des `VkImage`
et `VideoFrameRenderer` les convertit en RGBA par compute shader
(`shader/yuv_to_rgb.comp`, vues par plan, synchronisation par les sémaphores
timeline des `AVVkFrame`). Les autres formats (images, ProRes...) sont
décodés et convertis par le CPU en multi-thread, puis envoyés au GPU. En Debug,
`quickedit.log` à côté de l'exe reçoit les messages SDL, Vulkan et FFmpeg ; la
couche de validation signale VUID-VkImageCreateInfo-pNext-06811 sur les images
créées par FFmpeg lui-même (drapeaux mutables), sans effet sur le décodage.

Cible :

- Démultiplexage et sondage avec libavformat (durée, flux, cadence, couleur).
- Décodage avec libavcodec en hwaccel Vulkan : on crée l'`AVVulkanDeviceContext`
  sur **notre** `VkDevice` et nos files (graphique, compute, décodage vidéo),
  donc les `AVFrame` sont des `VkImage` utilisables directement. Repli logiciel
  (ProRes, DNxHR, formats exotiques) avec upload dans un `VkImage`.
- Un `DecoderPool` par média et par position de lecture : lecture anticipée sur
  un thread, cache de quelques frames, seek par image clé puis décodage jusqu'à
  la frame voulue.
- Vignettes et formes d'onde générées en tâche de fond.

Alternative écartée pour l'instant : notre propre décodeur Vulkan Video. Il
faudrait écrire les parseurs H.264/H.265/AV1, ce qui représente des mois de
travail pour un gain marginal sur ce que FFmpeg fait déjà avec Vulkan.

### `render/` : compositing Vulkan *(fait, espace de travail 8 bits pour l'instant)*

État actuel : un décodeur (`VideoPlayer`) et un `VideoFrameRenderer` par
piste vidéo ; `Compositor` dessine les calques du bas vers le haut en rendu
dynamique dans une image RGBA8 de la taille de la séquence, chaque calque
placé par la transformation animée de son clip (`core::Transform`, images
clés avec courbes). Les textes (`FontLibrary`, `TextLayout`) sont rendus
depuis un atlas de glyphes en champ de distance signé (stb_truetype fourni
avec ImGui) : une instance par glyphe, ombre puis contour et remplissage dans
le shader, animations lettre par lettre (machine à écrire, fondu, montée,
pop) appliquées par glyphe. Les polices du système sont recensées en tâche de
fond en ne lisant que leur table `name`.

Cible :

- Espace de travail linéaire, `RGBA16F`. Conversion YUV→RGB par compute shader
  (matrices BT.601/709/2020, plage limitée/pleine), ou `VkSamplerYcbcrConversion`.
- Graphe par frame : pour chaque piste vidéo de bas en haut, clip → effets →
  transformation → fusion (opacité, modes de fusion) sur l'accumulateur.
- Effets en compute shaders, sur le modèle des pipelines
  (`IPipeline`, shaders compilés en headers SPIR-V).
- La visionneuse affiche la sortie via un descripteur ImGui ; l'export utilise
  le même graphe à la résolution de la séquence.

### `audio/` : moteur audio *(mixage et sortie faits, plugins à faire)*

État actuel : `MediaManager` décode chaque audio en entier en float stéréo à
la fréquence de la séquence (une tâche par média sur le pool de threads,
décodeur FFmpeg multi-thread), puis calcule une forme d'onde multi-résolution.
`buildMixGraph()` aplatit les pistes audibles en un graphe immuable échangé
atomiquement ; `AudioEngine` le mixe dans le callback SDL3 (fondus de 3 ms aux
bords des clips contre les clics) et sert d'horloge à la lecture. Le décodage
complet en mémoire suffit pour des voix off de quelques dizaines de minutes ; il
sera remplacé par un décodage en flux avec cache disque pour les longs médias.

Cible :

- Thread temps réel, blocs float32 entrelacés à la fréquence de la séquence ;
  rééchantillonnage des sources (libswresample).
- Chaîne par piste : gain du clip, effets de piste (plugins), panoramique,
  mute/solo ; bus maître avec limiteur.
- Plugins **VST3** (SDK sous licence MIT depuis la 3.8) dans un thread de
  traitement, éditeurs dans des fenêtres natives. **CLAP** possible ensuite
  *(à confirmer)*.
- Sortie avec SDL3 audio. Pendant la lecture, l'horloge audio est l'horloge
  maître et la vidéo se cale dessus (aujourd'hui : horloge système).

### Export : encodage *(fait)*

État actuel : `Exporter` (thread dédié) refait la composition de chaque image
comme la visionneuse, avec ses propres lecteurs (décodage synchrone), son
compositeur hors écran et sa bibliothèque de polices, sur la seconde file de
la famille graphique. `Nv12Converter` écrit l'image en NV12 (BT.709, plage
limitée) directement dans une frame Vulkan de l'encodeur (`h264_vulkan`,
`hevc_vulkan`) ; sans encodage Vulkan, l'image est relue et convertie par
swscale pour NVENC, AMF, Quick Sync ou Media Foundation. `av1_vulkan` (FFmpeg
8.1) produit un flux corrompu sur NVIDIA : il passe après les encodeurs AV1
dédiés. Son : mixage identique à la lecture, AAC 192 kbit/s. En ligne de
commande : `QuickEdit projet.qep --export film.mp4 [--codec h264|h265|av1]`.

Cible initiale :

- Encodeurs Vulkan Video via FFmpeg (`h264_vulkan`, `hevc_vulkan`,
  `av1_vulkan`) alimentés directement par les `VkImage` du compositing,
  conversion RGB→NV12/P010 en compute. Repli NVENC/AMF/QSV ou logiciel.
- Multiplexage MP4/MOV/MKV avec libavformat, audio AAC/Opus/FLAC.
- File d'export en tâche de fond avec progression.

## Threads

| Thread            | Rôle                                                   |
|-------------------|--------------------------------------------------------|
| Principal         | UI, éditions du modèle, soumission graphique           |
| Décodage (n)      | lecture anticipée par média actif                      |
| Audio temps réel  | mixage et plugins, aucune allocation ni verrou         |
| Tâches de fond    | sondage, vignettes, formes d'onde, export              |

Le modèle n'est modifié que par le thread principal ; les autres threads
travaillent sur une copie immuable de la séquence publiée à chaque édition.

## Cas d'usage visé

Voix off enregistrée, nettoyée vite (couper les passages ratés), traitée par
des VST ; bande son dessous ; vidéo, captures d'écran et textes par-dessus.
Le texte doit être aussi facile à modifier que dans PowerPoint, les arrêts sur
image et les apparitions animées (zoom progressif puis maintien) doivent se
faire en un geste. Windows et Linux.

Conséquences sur le modèle :

- **Clips générés** en plus des clips de média : texte, visualisation audio,
  couleur unie. Un clip texte porte un document riche (paragraphes, runs avec
  police, taille, graisse, couleur, contour, ombre) édité directement dans la
  visionneuse.
- **Animations** : chaque propriété (position, échelle, rotation, opacité,
  gain) peut avoir des images clés avec courbes d'accélération. Des
  préréglages ("Apparition zoom", "Fondu", "Machine à écrire" pour le texte)
  créent ces images clés en un clic.
- **Transitions** entre deux clips adjacents (fondu, glissement, zoom) et
  transitions d'entrée/sortie sur un clip seul.
- **Arrêt sur image** : commande qui coupe le clip à la tête de lecture et
  insère un clip "frame figée" d'une durée choisie, en décalant la suite.
- **Montage de la voix** : forme d'onde dans les clips, suppression avec
  décalage (ripple delete), sélection d'une plage à retirer d'un geste.
- **Effets audio** par piste (VST3, puis CLAP), avec bus maître.

## Feuille de route

1. ~~Base qui compile : fenêtre, panneaux, timeline éditable, projet JSON,
   undo/redo, tests du modèle.~~
2. ~~FFmpeg : sondage des médias et décodage audio en tâche de fond, formes
   d'onde, décodage vidéo Vulkan Video et affichage dans la visionneuse.~~
   Reste : vignettes dans la timeline.
3. ~~Audio : mixage, sortie SDL3, lecture calée sur l'horloge audio, ripple
   delete pour la voix.~~
4. ~~Lecture temps réel : compositing de plusieurs pistes, images fixes.~~
5. ~~Clips texte (rendu de texte riche sur GPU), édition directe dans la
   visionneuse (`TextEditor`, `core/TextEdit`).~~
6. ~~Images clés, préréglages d'animation, arrêt sur image.~~ Reste :
   poignées d'échelle et de rotation dans la visionneuse, éditeur de courbes.
   Transitions faites (fondu enchaîné, fondu au noir, poussée, volet, zoom,
   centrées sur la coupe, sans marge de média : `core::layersAt`).
7. ~~VST3 sur les pistes audio~~ (`audio/Vst3Plugin`, `app/EffectRack` :
   chaîne d'effets par piste, éditeurs dans des fenêtres SDL, états en base64
   dans le projet, nouvelles instances pour l'export). Reste : éditeurs sous
   Linux (boucle X11), visualisation audio en spectre.
8. ~~Export H.264/H.265/AV1 via Vulkan Video.~~ (AV1 par NVENC/AMF/QSV pour
   l'instant, voir plus haut.)
9. Vérification et paquet Linux.
10. ~~Enregistrement~~ : voix off (`audio/Recorder`, WAV 24 bits), écran,
    fenêtre ou caméra (`capture/ScreenRecorder` : filtre `gfxcapture` de
    FFmpeg, images GPU jusqu'à NVENC/AMF, son de l'ordinateur par WASAPI,
    caméras par DirectShow), progression en direct sur la timeline.
11. ~~Effets vidéo GPU par clip et par piste~~ (`core/VideoEffects`, passes
    `shader/effect_*.frag` de `Compositor::cmdApplyEffects`), VST par clip,
    recadrage / coins / bordure / ombre (`core::Frame`, `shader/layer.frag`).
12. ~~Shaders de l'utilisateur~~ (`core/Shaders`, `render/ShaderCompiler` avec
    shaderc) : générateurs (`ClipKind::Generator`) et transitions
    (`TransitionType::Shader`), dans `Documents/QuickEdit/Shaders`.
13. ~~Éditeur de courbes~~ (`app/KeyframeEditor`), vignettes
    (`media/Thumbnails`), dossiers de médias, séquences multiples et
    imbriquées (`ClipKind::Sequence`, `Compositor::cmdRenderLevel`),
    sous-titres (`TrackType::Subtitle`, `core/Subtitles` : SRT, incrustation).
    Génération automatique des sous-titres (`app/SubtitleGenerator`,
    whisper.cpp sur GPU par Vulkan ou sur CPU, modèles téléchargés depuis
    l'application dans `Documents/QuickEdit/Models`).

## Compilation

Windows, Visual Studio 2022 ou plus récent (CMake + Ninja), SDK Vulkan 1.4.

Dépendances téléchargées, ignorées par git :

- FFmpeg 8.1 LGPL partagé de https://github.com/BtbN/FFmpeg-Builds
  (`ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip`) extrait dans
  `3rdparty/ffmpeg/win64` (sous Linux : paquets de développement FFmpeg).
- SDK VST3 3.8.1 (MIT) :
  `git clone --depth 1 --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk.git 3rdparty/vst3sdk`
  puis `git submodule update --init --depth 1 base pluginterfaces public.sdk` dans ce dossier.
- whisper.cpp 1.8.7 (MIT), facultatif (sous-titres automatiques) :
  `git clone --depth 1 --branch v1.8.7 https://github.com/ggml-org/whisper.cpp.git 3rdparty/whisper.cpp`.

```
git submodule update --init --recursive
cmake -G Ninja -S . -B out/build/x64-Debug -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/x64-Debug
ctest --test-dir out/build/x64-Debug
```

Ou ouvrir le dossier dans Visual Studio (`CMakeSettings.json`).
