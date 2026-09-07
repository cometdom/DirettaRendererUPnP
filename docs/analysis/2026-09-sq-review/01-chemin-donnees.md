# Audit du chemin de données audio — DirettaRendererUPnP 2.5.15

*Audit réalisé le 2026-09-07 (agent d'analyse, rapport brut archivé sans retouche).*

Périmètre : `src/AudioEngine.{h,cpp}`, `src/DirettaRingBuffer.h`, `src/DirettaSync.{h,cpp}`, `src/DirettaRenderer.cpp`, `src/ProtocolInfoBuilder.h`, `src/memcpyfast_audio.h`, `src/FastMemcpy_Audio.h`, en-têtes SDK 149/150 (`Format.hpp`, `Sync.hpp`, `diretta_stream.h`). Lecture seule, aucun fichier modifié.

Conventions : **[V]** = vérifié dans le code de l'arbre ; **[F]** = comportement FFmpeg connu, *non vérifiable* ici (aucune source FFmpeg locale) ; **[S]** = suspecté / hypothèse.

Point préliminaire important [V] : aucun traitement de gain n'existe dans le chemin audio. `SetVolume`/`SetMute` UPnP ne touchent que l'état `m_volume`/`m_mute` de `UPnPDevice.cpp:820-873`, jamais les échantillons. Aucun rééchantillonnage n'est jamais demandé : `AudioEngine::process()` passe `outputRate = m_currentTrackInfo.sampleRate` et `outputBits = m_currentTrackInfo.bitDepth` à `readSamples()` (`AudioEngine.cpp:2221-2237`), donc `swr` est toujours instancié avec `in_rate == out_rate` (`AudioEngine.cpp:1738-1748`) et ne crée pas de resampler [F]. Aucun dither n'est configuré (dither swresample par défaut = none [F]).

---

## 1. Chemin de données par format source

Étapes : **E1** HTTP→demux (paquet) · **E2** décodage (frame, `sample_fmt`) · **E3** sortie `readSamples()` (conteneur remis au moteur) · **E4** négociation sink (`configureSinkPCM/DSD`) · **E5** `sendAudio()` → méthode `push*` du ring · **E6** conteneur dans le ring = octets remis au SDK dans `getNewStream()` (`pop` brut, `DirettaSync.cpp:1915`, aucune transformation après le ring [V]).

| Source | E1→E2 (FFmpeg) | E3 `readSamples` | E4 sink (défaut) | E5 push | E6 vers SDK | Bit-transparent ? |
|---|---|---|---|---|---|---|
| FLAC 16/44.1 | flac → `S16` packé [F] | swr S16→S16 identité (bypass interdit car `isCompressed`, `AudioEngine.cpp:1825`) | 24 bits (32 jamais tenté si source<32, `DirettaSync.cpp:1055`; 24 tenté avant 16, `:1067`) | `push16To24` : `[00, lo, hi]` (`DirettaRingBuffer.h:729-738`) | S24 packé 3 o, 16 bits utiles + LSB=0 | **Oui** (padding zéro, sans dither) |
| FLAC 24/96, 24/192 | flac → `S32` packé, justifié à gauche (`<<8`) [F] | swr S32→S32 identité | 24 bits | hint `MsbAligned` (`AudioEngine.cpp:743-747`) → `push24BitPacked` mode shifted = octets 1..3 (`DirettaRingBuffer.h:638-680`) | S24 packé 3 o = 24 bits d'origine | **Oui** |
| FLAC 20 bits / MQA | idem, `bits_per_raw_sample=20` → « Invalid bit depth » → 24 (`AudioEngine.cpp:695-699`) | idem | 24 | idem (20 bits dans les MSB, 4 LSB = 0) | S24 packé | **Oui** (représentation standard 20-en-24 ; MQA-16 et MQA-24 conservent leurs LSB) |
| FLAC 32 int | `S32` (shift 0) [F] | swr identité | **32** tenté (source=32), sinon 24 | 32 : `push()` copie directe (`DirettaSync.cpp:1600-1606`) ; 24 : pack MSB = troncature 8 LSB sans dither | S32 ou S24 | Oui en 32 ; **Non** si sink 24 (inévitable) |
| WAV 16 (`pcm_s16le`) | paquet brut | **raw packet bypass** memcpy (`AudioEngine.cpp:1415-1436`, activé `:1704-1719`) | 24 | `push16To24` | S24 packé | **Oui** |
| WAV 24 (`pcm_s24le`) | décodeur → `S32` `<<8` [F] | bypass frame memcpy (`:1528-1547`) ; raw bypass impossible (`block_align`=6≠8) | 24 | hint MSB (`:737-741`) → pack shifted | S24 packé | **Oui** |
| WAV 32 int (`pcm_s32le`) | brut | raw packet bypass | 32 | `push()` direct | S32 | **Oui** (seul chemin « zéro conversion » de bout en bout) |
| WAV float32 | `pcm_f32le` non listé comme non-compressé (`:446-453`) → swr FLT→S32 (arrondi+clip) [F] | — | 32 (lossless prop) | direct | S32 | Non (conversion float→int, inhérente) |
| AIFF 16/24 (`pcm_s16be/s24be`) | décodeur fait le byte-swap → `S16`/`S32` [F] | bypass frame (raw bypass exclu car BE, `:1710-1712`) | 24 | comme WAV | S24 | **Oui** |
| ALAC 16/24 | alac → `S16P`/`S32P` **planaire**, 24 bits `<<8` [F] | swr planaire→packé (valeurs inchangées) | 24 | hint MSB (`:743-747`) | S24 | **Oui** |
| MP3/AAC/Vorbis/Opus | `FLTP` [F] | bitDepth 32 → **plafonné 24** (`:712-721`) ; swr FLTP→S32 (lrint, clip, sans dither) | 24 | hint MSB (`:762-767`) → pack shifted = troncature des 8 LSB du résultat 32 bits | S24 | N/A (lossy) — conversion sans dither |
| DSD64 DSF natif | demuxer dsf, **codec jamais ouvert**, paquets bruts `[bloc L][bloc R]` (`:1188-1238`) | assemblage planaire `[L…][R…]` (`:1264-1271`), ordre de bits LSB-first conservé (`:1284-1286`) | `FMT_DSD1|SIZ_32|LSB|BIG` en premier (`DirettaSync.cpp:1109-1127`) → DSF = `Passthrough` | `pushDSDPlanarOptimized` : entrelacement mots de 4 o L/R (`DirettaRingBuffer.h:919-1010`) | DSD 32-bit words, bits intacts | **Oui** |
| DSD64 DFF natif | parseur maison (`:839-984`), désentrelacement octet à octet (`:1133-1145`) | planaire MSB-first | même sink LSB\|BIG → `BitReverseOnly` (`:1117-1122`) | inversion de bits via LUT/SIMD | DSD LSB-first | **Oui** (changement de représentation, réversible) |
| DSD → DoP (`--dop`) | idem natif | idem | `configureSinkPCM(rate/16, 24)` (`DirettaSync.cpp:741-744`) | `pushDSDToDoP` : `[DSD N+1, DSD N, 0x05/0xFA]` (`DirettaRingBuffer.h:557-575`) ; **pas d'inversion de bits sauf `--dop-msb`** (`DirettaSync.cpp:1547`) | S24 packé DoP | Ordre d'octets DoP 1.1 correct [V] ; **ordre de bits douteux pour DSF** (voir F6) |
| DoP pré-encodé par le serveur (L24/WAV 24 avec marqueurs) | comme WAV 24 | bypass | 24 | pack MSB | S24 | **Oui** (les marqueurs survivent tant que le chemin 24→24 est transparent) |

Remarques transversales [V] :
- Toutes les sorties FFmpeg sont little-endian natives ; les sources BE (AIFF, `audio/L16`) sont converties par le décodeur, et le raw-packet-bypass est explicitement restreint à LE (`AudioEngine.cpp:1710-1712`).
- `AV_SAMPLE_FMT_S24` n'existe pas dans FFmpeg : le 24 bits arrive **toujours** en S32 justifié à gauche [F]. Le code de `AudioEngine.cpp:734-768` ne produit d'ailleurs **jamais** `S24Alignment::LsbAligned` (grep : seule référence dans le ternaire de `DirettaRenderer.cpp:455`). Le mode LSB du ring n'est donc atteignable que par la détection/le timeout, c'est-à-dire uniquement par erreur.
- Le silence PCM est `0x00` (`DirettaSync.cpp:118, 1262`), DSD `0x69` (`:1357`), DoP `0x00` (`:1716-1727`).
- La taille popée est toujours multiple de la trame (PCM : `framesBase*bytesPerFrame` ou `framesPerBuffer*bytesPerFrame` + dérive d'une trame, `:1276-1303`, `:1736-1751` ; DSD : multiple de `4*channels`, `:1367-1375`).

---

## 2. Constats

### F1 — Mode de packing S24 : défaut LSB erroné + hint perdu à la reprise après pause
**Gravité : bug latent (bruit blanc pleine échelle), risque de robustesse élevé sur x86.**

Preuves [V] :
- `DirettaRingBuffer.h:367-391` : sans hint, `detectS24PackMode` n'inspecte que `b0`/`b3` des 64 premiers échantillons-canaux ; en cas de silence (`Deferred`), après `DEFERRED_TIMEOUT_SAMPLES = 48000` (`:1537`) le mode est **verrouillé `LsbAligned`** (`:380`) ; le fallback immédiat est aussi LSB (`:390`).
  - `numSamples = inputSize/4` compte des échantillons-canaux : le timeout vaut ~0,54 s en stéréo 44,1 k, ~0,125 s à 192 k (le commentaire « ~1 second at 48kHz » est faux d'un facteur canaux).
  - « Silence » au sens de la détection = 64 échantillons consécutifs dont l'octet haut est 0, donc tout passage positif < −48 dBFS ou zéro numérique : débuts de pistes, fondus, silences entre mouvements.
- `DirettaRingBuffer.h:179-189` : `clear()` remet `m_s24PackMode` et `m_s24Hint` à `Unknown`.
- `DirettaSync.cpp:1480` : `resumePlayback()` appelle `m_ringBuffer.clear()`.
- `DirettaRenderer.cpp:676-679` : la reprise après pause appelle `resumePlayback()` puis `m_audioEngine->play()` ; le callback ne re-pose le hint que si `needsOpen` (`:418`, `:444-461`), or `m_playing` est déjà vrai (`DirettaSync.cpp:1485`) → **aucun hint après une reprise**.
- Conséquence : si la reprise tombe sur un passage silencieux > ~0,5 s, verrouillage LSB, puis `convert24BitPacked_AVX2` (`:594-636`) extrait les octets 0..2 d'un mot MSB-aligné = `[00, lo, mid]` : l'octet de signe/amplitude est jeté → bruit blanc pleine échelle jusqu'au prochain `open()`.
- Sur ARM64 le problème est masqué par `#if defined(__aarch64__) effectiveMode = MsbAligned` (`:395-397`), dont le commentaire (« FFmpeg on ARM produces MSB-aligned … x86 produces LSB ») est faux [F] : les décodeurs FFmpeg sont indépendants de la plate-forme ; c'est la même justification à gauche partout.

Robustesse du hint côté sources [V] : les quatre branches de `AudioEngine.cpp:734-768` couvrent `PCM_S24LE/BE` (WAV/AIFF), FLAC/ALAC (dont MQA, 20 bits), tout codec dont `sample_fmt` est S32/S32P (WavPack, TTA, TrueHD…), et tout codec lossy (FLTP via swr→S32). Seuls des cas exotiques restent `Unknown` (ex. `pcm_u8` → U8 : bitDepth 8 « invalide » → 24 ; ici un verrouillage LSB donne du **silence total**, pas du bruit). Le hint est donc fiable pour les *sources* ; c'est son *état* dans le ring qui est fragile.

Changement recommandé (minimal) :
1. `DirettaRingBuffer.h:380` et `:390` → `MsbAligned` (c'est le seul alignement que ce projet peut recevoir).
2. `AudioEngine.cpp:735` : poser `s24Alignment = MsbAligned` inconditionnellement quand `realBitDepth == 24` (garder le log par branche à titre informatif).
3. `DirettaRingBuffer.h:179-189` : ne plus effacer `m_s24Hint` dans `clear()` ; restaurer `m_s24PackMode = m_s24Hint` quand le hint ≠ `Unknown`. Déplacer la remise à zéro complète dans `resize()` (`:149-156`), qui est le seul point suivi d'un `setS24PackModeHint()`.
4. Supprimer le `#if __aarch64__` (`:395-397`) une fois 1-3 en place, pour que x86 et ARM exécutent le même code.

### F2 — Combinaisons (entrée 4 o, sink 2 o) non converties : copie directe d'octets incohérents
**Gravité : bug latent (garbage audio), déclenché par des sinks sans 24 bits.**

Preuves [V] :
- `DirettaSync.cpp:1052-1087` : pour `inputBits < 32`, l'ordre d'essai est 24 → 16, **jamais 32**. Une source 24 bits sur un sink « 32 mais pas 24 » (ou « 16 seulement ») tombe en `acceptedBits = 16`.
- `DirettaSync.cpp:762-765` : `direttaBps = 2`, `inputBps = 4` ; `configureRingPCM` (`:1234-1236`) ne lève aucun flag pour (4→2) → branche « PCM direct copy » (`:1600-1606`) avec `bytesPerFrame = 2*ch` sur un tampon S32.
- `DirettaRenderer.cpp:491-506` : le renderer compte en trames de 4 o (`samplesConsumed = sent / (4*ch)`) alors que le ring a consommé en 2 o → les échantillons sont réinterprétés en 16 bits (moitié LSB / moitié MSB de chaque mot).
- Même chose pour une source 32 bits sur sink 16 bits.

Changement recommandé : après l'échec du 24 bits, pour `inputBits == 24` tenter `FMT_PCM_SIGNED_32` (le conteneur S32 justifié à gauche est déjà exact → `push()` direct, sans perte) ; puis dans `open()` refuser explicitement toute combinaison où `inputBps > direttaBps` sans chemin de conversion (`return false` + log) plutôt que de pousser des octets mal typés. Si l'on veut vraiment servir un sink 16 bits, ajouter une conversion 24/32→16 (troncature, idéalement avec dither TPDF) — mais c'est une décision de qualité, pas un bug fix.

### F3 — `std::runtime_error` non rattrapée sur le thread audio + capacités UPnP codées en dur
**Gravité : bug latent (crash) / robustesse.**

Preuves [V] :
- `DirettaSync.cpp:1087` et `:1219` : `throw std::runtime_error(...)` quand aucun format n'est accepté.
- `open()` est appelé depuis le callback audio (`DirettaRenderer.cpp:445`), exécuté par le thread `audioThreadFunc` (`:913-…`) ; aucun `try` sur ce chemin (grep : les seuls `catch` de `DirettaRenderer.cpp` sont aux lignes 62, 165, 830, hors thread audio). Une exception dans un `std::thread` → `std::terminate`.
- `UPnPDevice.cpp:56-57` : la liste `ProtocolInfo` est `getHoloAudioCapabilities()` (`ProtocolInfoBuilder.h:50-80`), jamais dérivée de `getSinkInfo()`. Un point de contrôle enverra donc du DSD à un target PCM-only, ce qui déclenche exactement le `throw` de `configureSinkDSD`.

Changement recommandé : remplacer les deux `throw` par un `return false` remonté à `open()` ; à l'initialisation, filtrer les entrées DSD/DoP de `ProtocolInfo` selon `getSinkInfo().checkSinkSupportDSD()` (`Sync.hpp:242`).

### F4 — Push DSD partiel : plans L/R décalés et reste silencieusement perdu
**Gravité : bug latent (uniquement si le ring est presque plein).**

Preuves [V] :
- `DirettaRingBuffer.h:480-487` : `maxBytes = min(inputSize, STAGING_SIZE, free)` puis `bytesPerChannel = maxBytes / numChannels`.
- `DirettaRingBuffer.h:921-927` (et homologues `:1018-1024`, `:1122-1128`, `:1231-1237`) : `srcR = src + bytesPerChannel` **calculé sur la taille tronquée**, alors que le plan R commence réellement à `src + inputSize/numChannels` → en cas de troncature, une partie du plan L est lue comme R.
- `DirettaRenderer.cpp:469-486` : la boucle DSD ne réémet que si `sent == 0` ; un `sent` partiel abandonne le reste.
- Déclenchement : `free < inputSize` (≤ 32 768 o par appel, `DirettaSync.h:266-286`). Le throttle à 50 % (`DirettaRenderer.cpp:958`) rend le cas improbable, d'où « latent ».

Changement recommandé (une ligne) : dans `pushDSDPlanarOptimized`, `if (free < inputSize) return 0;` (push atomique ; la boucle appelante attend déjà sur 0). Alternativement, passer le stride de plan (`inputSize/numChannels`) aux `convertDSD_*`.

### F5 — Push PCM partiel non aligné sur la trame → désalignement permanent du flux
**Gravité : bug latent (même condition de déclenchement que F4).**

Preuves [V] :
- `DirettaRingBuffer.h:309-313` : `push()` écrit `len = free` octets, sans arrondi à la trame. `free = size-1-avail` (`:170-177`) : quand le ring est plein, `free ≡ -1 (mod bytesPerFrame)`, donc **tout** push partiel à ring plein est fractionnaire.
- `DirettaRenderer.cpp:504-506` : `samplesConsumed = sent / bytesPerSample` (division entière) mais `audioData += sent` → sur-lecture jusqu'à `bytesPerFrame-1` octets au-delà du tampon et injection de ces octets dans le ring → décalage du flux (échange de canaux / bruit) jusqu'au prochain `clear()`.
- Les chemins `push16To32/16To24/24BitPacked` clampent en échantillons-canaux (`free/4`, `free/3`) et non en trames : même effet en bord de ring plein (échantillon L sans R renvoyé comme « consommé » par arrondi inférieur).

Changement recommandé : mémoriser `inputFrameBytes` dans le ring à `configureRingPCM` et arrondir `len`/`numSamples` à un multiple de trame dans chaque `push*` ; ou rendre les pushes atomiques (`return 0` si `free < totalBytes`) comme pour F4.

### F6 — Ordre de bits DoP indépendant du format source (DSF vs DFF)
**Gravité : risque de robustesse (qualité) — à confirmer à l'écoute/mesure.**

Preuves [V] :
- `DirettaSync.cpp:1547` : `dopBitReverse = g_dopMsb` (drapeau utilisateur, défaut `false`, `main.cpp:76`), sans regarder `m_currentFormat.dsdFormat`.
- `AudioEngine.cpp:1284-1286` : les octets DSF restent LSB-first ; le chemin natif, lui, inverse bien pour DSF→sink MSB / DFF→sink LSB (`DirettaSync.cpp:1117, 1138`).
- La spec DoP 1.1 place le bit DSD le plus ancien en MSB (convention DFF/ALSA `DSD_U8`) [S — connaissance de la spec, non vérifiable dans l'arbre]. Un DSF poussé sans inversion transporte donc des octets inversés bit à bit.
- Pourquoi cela « fonctionne » quand même [S] : inverser les bits à l'intérieur de chaque octet est une permutation par blocs de 8 qui conserve la somme de chaque bloc ; après filtrage passe-bas du DAC (bande audio ≪ fs/8 = 352,8 kHz), la musique est quasi intacte, mais le bruit de mise en forme HF est replié → plancher de bruit / souffle plus élevé. Cohérent avec le fait que `--dop-msb` ait été ajouté « try if --dop produces noise » (`main.cpp:385-386`).

Changement recommandé : `dopBitReverse = (dsdFormat == DSF) ^ g_dopMsb` (inversion par défaut pour DSF, aucune pour DFF, `--dop-msb` devenant un inverseur). Test : même DSF en `--dop` et `--dop-msb`, comparer le plancher de bruit ; contre-test avec un DFF.

### F7 — 16 bits toujours élargi en 24 bits packé (choix de format, transparent)
**Gravité : cosmétique / question A-B.** `configureSinkPCM` (`DirettaSync.cpp:1067-1085`) tente 24 avant 16 pour toute source < 32 ; le 16 bits natif n'est utilisé que si le sink refuse 24 et 32. Le padding `[00, lo, hi]` (`DirettaRingBuffer.h:729-738`) est exact. Voir §3-B.

### F8 — FLAC exclu à tort du bypass ; swresample instancié pour une identité ; double copie
**Gravité : cosmétique (charge inutile).** Voir §4.

### F9 — Fin de piste : décodeur non vidangé, FIFO ignoré après EOF
**Gravité : cosmétique (perte de quelques ms, codecs à latence seulement).**
- `AudioEngine.cpp:1374-1376` : `AVERROR_EOF` → `m_eof = true` sans `avcodec_send_packet(ctx, NULL)` + boucle `receive_frame` → les dernières trames retenues par les décodeurs à délai (MP3/AAC) sont perdues [F] ; FLAC/ALAC/PCM n'ont pas de délai.
- `AudioEngine.cpp:1295` : retour immédiat sur `m_eof` avant le drainage du FIFO (`:1327-1343`). Par construction le FIFO n'est non vide qu'après un retour « plein » (`totalSamplesRead == numSamples`), donc il est drainé à l'appel suivant *avant* l'EOF ; aucune perte constatée [V], mais l'ordre est fragile.
- `DirettaRenderer.cpp:575-589` : en fin de playlist, `stopPlayback` après un drain à 1 % du ring → jusqu'à ~1 % du ring (5–30 ms selon la taille arrondie en puissance de 2) jeté. Gapless non affecté (pas de `clear()`).

### F10 — Silence et largeur d'échantillon codés en dur au lieu de l'API SDK
**Gravité : robustesse.** `Format.hpp:157-160, 179-180` exposent `getWid()`, `getBits()`, `getMuteByte()`. Le code suppose `FMT_PCM_SIGNED_24` = 3 octets (`DirettaSync.cpp:762`) — validé sur le terrain (correctif TEAC UD-701N) mais jamais vérifié à l'exécution — et hard-code `0x00`/`0x69`. Note : `0x69` inversé bit à bit vaut `0x96` ; les deux sont des motifs DC-free acceptables, mais `getMuteByte()` donnerait le motif canonique du sink. Recommandation : après `configureSink*`, `assert(m_pendingSinkFormat.getWid() == direttaBps)` et `resize(ring, fmt.getMuteByte())`.

### F11 — DSD multicanal non géré de façon cohérente
**Gravité : bug latent (hors stéréo).** `AudioEngine.cpp:1226` (`blockSize = packetSize/2`), `:1139-1145` (DFF : deux premiers canaux seulement) alors que `trackInfo.channels = N` sert à `sendAudio` (`DirettaSync.cpp:1569`) et à `configureRingDSD`. Refuser `channels != 2` en DSD serait plus sûr.

### F12 — Sémantique SDK `FMT_DSD_SIZ_32` / BIG-LITTLE non vérifiable
**Gravité : à confirmer.** `Format.hpp:39` commente « LSB 1 2 3 4 (LE)  MSB 4 3 2 1 (LE) ». Le code interprète BIG = ordre mémoire = ordre temporel (pas de swap) et LITTLE = swap (`DirettaSync.cpp:1118, 1160`), ce qui correspond à la convention ALSA `DSD_U32_BE/LE`. Le premier choix `LSB|BIG` + `Passthrough` (DSF) est cohérent avec « LSB 1 2 3 4 ». Le cas `MSB|BIG` + `Passthrough` (`:1129-1148`, sinks MSB-only) pourrait nécessiter « 4 3 2 1 » selon la lecture du commentaire — non tranchable sans la doc SDK. Le repli `FMT_DSD1` seul (`:1193-1217`) sans `SIZ_32` continue d'entrelacer par mots de 4 o.

### F13 — Commentaires/documentation périmés (cosmétique)
`AudioEngine.h:43-46` (« sample-based detection takes priority » — inversé depuis v2.0.0) ; `DirettaRingBuffer.h:341-348` (« FLAC, ALAC, PCM_S24 are always LSB » — faux) ; `:393-397` et `CHANGELOG.md:756-761` (ARM ≠ x86 — faux [F]) ; `AudioEngine.cpp:1814-1815` (« FLAC decode to planar » — faux [F], seul ALAC est planaire) ; `:1537` unités du timeout.

---

## 3. Choix de format à tester en A/B sur la cible

| # | Question | Comportement actuel | Bouton |
|---|---|---|---|
| A | **24 bits packé (S24_3LE, 6 o/trame) vs conteneur S32 (8 o/trame)** — audio bit-identique, diffèrent : format côté sink, octets/trame → `cycleTime` et trames/MTU (`DirettaSync.cpp:1276-1303`, `DirettaSync.h:303-308`), travail hôte (pack SIMD vs `memcpy_audio`). | Packé 24 dès que le sink l'accepte ; S32 seulement pour sources 32 bits. | Condition `inputBits >= 32` `DirettaSync.cpp:1055` (tenter 32 aussi pour 24) — attention aux DACs qui annoncent 32 sans le faire (historique TEAC, `CHANGELOG.md:292`). |
| B | **16 bits : natif 16 vs 16→24 (zéro-pad) vs 16→32** | 16→24 systématique si sink 24 ; 16→32 seulement si sink 32-sans-24 ; natif 16 seulement si ni 24 ni 32. | Ordre des essais `DirettaSync.cpp:1067-1085`. Les trois conversions sont exactes (`DirettaRingBuffer.h:688-738`). |
| C | **Chemin « direct copy » vs conversion** pour du 24 bits : le seul moyen de faire passer du 24 bits par `push()` (memcpy FastMemcpy AVX, `:309-335`) est l'option A. | 24 bits → toujours `push24BitPacked`. | Idem A. |
| D | **Bypass décodeur vs swr identité** (FLAC) — mêmes octets, charge/latence différentes. | FLAC → swr ; WAV/AIFF → bypass ; WAV 16/32 → raw packet bypass. | Garde `isCompressed` `AudioEngine.cpp:1825`. |
| E | **Encadrement de silence** : durée de silence numérique avant le premier échantillon (prefill 80/100/500/1000 ms `DirettaSync.h:210-214` ; stabilisation 100/200 ms `:219-223` ; `POST_ONLINE_SILENCE_BUFFERS=20`), queue de silence 20/50 buffers (`DirettaSync.cpp:883, 1438`). | Voir valeurs. | Constantes `DirettaBuffer`, options `--*-prefill`, `dacStabilizationMs`. |
| F | **Silence DoP** : `0x00` PCM (le DAC quitte/re-verrouille le mode DSD) vs trames DoP marquées `0x69`. | `0x00` (choix documenté `DirettaSync.cpp:1716-1724`, hiss observé avec l'autre). | `fillSilence` `:1725-1727`. |
| G | **DSD : sink `LSB\|BIG` vs `MSB\|BIG` pour un DFF** — bits identiques après inversion, différence de CPU/timing hôte uniquement. | LSB\|BIG d'abord → DFF inversé sur l'hôte. | Ordre `DirettaSync.cpp:1108-1148`. |
| H | **DoP `--dop` vs `--dop-msb` par type de source** — test de *correction* (F6), pas de préférence. | Aucune inversion par défaut. | `g_dopMsb`. |
| I | **Prise en compte du ProtocolInfo** : annoncer `audio/L16` pour tous les débits (`ProtocolInfoBuilder.h:160-163`) peut amener certains serveurs à transcoder du 24 bits en L16 [S]. Retirer L16 aux débits > 48 k et comparer ce que le serveur envoie. | Liste Holo codée en dur. | `getHoloAudioCapabilities()`. |

---

## 4. Travail inutile dans l'étage de décodage [V]

1. **swresample pour une identité** : pour FLAC 16 (S16→S16) et FLAC 24/32 (S32→S32), `initResampler` alloue et initialise un `SwrContext` (`AudioEngine.cpp:1738-1760`) uniquement parce que `canBypass` rejette tout `isCompressed` (`:1823-1828`) sur la croyance erronée que FLAC est planaire [F]. Les tests de format déjà présents (`:1852-1872`) suffisent à exclure ALAC (planaire) et les flottants ; la détection de désaccord à l'exécution (`:1507-1526`) reste une sécurité. Supprimer la garde `isCompressed`.
2. **Double copie dans le chemin swr** : `swr_convert` → `m_resampleBuffer` (`:1580-1586`) puis `memcpy_audio` → `outputPtr` (`:1594`). Quand `samplesNeeded >= totalOutSamples`, convertir directement dans `outputPtr`.
3. **Allocation de 256 Ko `m_resampleBuffer`** (`:1789-1794`) + FIFO `av_audio_fifo` (`:1772-1776`) même pour les identités ci-dessus (disparaît avec 1).
4. **`swr_get_delay` / `av_rescale_rnd` par trame** (`:1551-1562`) : sans rééchantillonnage, `delay` vaut 0 [F] ; coût négligeable, mais inutile après 1.
5. **DSD/DFF : trois copies** (lecture entrelacée dans `buffer+totalBytesNeeded`, désentrelacement scalaire octet par octet `:1133-1145`, puis `memcpy_audio` vers le début du même buffer `:1269-1270`). Le DSF en fait deux (`:1235-1238` puis `:1269-1270`). Un désentrelacement direct dans la zone de sortie éviterait la dernière.
6. **`std::cout` inconditionnel** dans le chemin « No resampling - direct copy » (`:1633-1634`), pratiquement inatteignable (swr nul sans bypass) mais à passer en `DEBUG_LOG`.
7. Rien à signaler côté allocations par paquet : `m_packet`/`m_frame` réutilisés (`:1346-1351`), `AudioBuffer` ne croît qu'à la demande, `m_streamData.resize()` (`DirettaSync.cpp:1755-1757`) ne réalloue plus après la première croissance (la capacité est conservée quand la taille oscille avec la correction de dérive).
8. `FastMemcpy_Audio.h` / `memcpyfast_audio.h` : copies AVX2 avec chargements/stockages non alignés et gestion de queue par recouvrement ; aucune transformation d'octets, garde anti-recouvrement en debug (`memcpyfast_audio.h:150-158`). Rien de suspect pour la transparence.

---

## Synthèse

Le chemin est **bit-transparent par construction** pour tous les formats lossless usuels (FLAC/WAV/AIFF/ALAC 16-24-32, DSF/DFF natif), sans rééchantillonnage, sans gain, sans dither, avec un seul point de décision de conteneur (`configureSinkPCM`) qui favorise systématiquement le 24 bits packé. Les vrais risques sont d'*état* et de *bord* : (F1) le mode S24 revient à un défaut LSB qui n'est jamais correct dans ce projet, et le hint est perdu à la reprise après pause sur x86 ; (F2/F3) des sinks sans 24 bits ou sans DSD conduisent à des octets mal typés ou à un `terminate` ; (F4/F5) les pushes partiels à ring plein corrompent l'alignement (latents grâce au throttle 50 %) ; (F6) l'ordre de bits DoP mérite un test DSF vs DFF. Les correctifs proposés pour F1, F4 et F5 tiennent en quelques lignes chacun.
