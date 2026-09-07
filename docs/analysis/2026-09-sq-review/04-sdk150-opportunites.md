# SDK Diretta Host 150 vs DirettaRendererUPnP 2.5.15 — ce que le SDK offre, ce que DRUP utilise, ce qui reste à prendre

*Audit réalisé le 2026-09-07 (agent d'analyse, rapport brut archivé sans retouche). Convention : **[DOC]** = cité textuellement des headers / Doxygen / échantillon officiel ; **[INF]** = inférence, à vérifier auprès de Yu Harada ou par test.*

## 0. Nature des sources (important pour lire la suite)

- Le Doxygen 150 (`<SDK 150>\doc`, Doxygen 1.13.2) est **généré uniquement à partir des commentaires `///` des headers** : la page `index.html` est vide (« HosrDiretta Documentation »), et chaque « Detailed Description » reprend mot pour mot le `@brief` du header. Il n'existe donc **aucune sémantique documentée au-delà des lignes de commentaire des `.hpp`**. Le diff `149/doc` vs `150/doc` ne contient que les pages régénérées pour les 5 changements de header (Sync, SyncBuffer, ProfileMaker, Clock/ClockDiff) et les index.
- `memo_host.txt` est identique (diff vide) : licence + `DIRETTA:Sync buffer pull type / DIRETTA:SyncBuffer buffer push type`.
- La seule « recommandation d'usage » de l'auteur qui existe localement est l'échantillon `<SDK 147>\SinHost\SinHost.cpp` (SDK 147). Il n'y a pas de SinHost dans les archives 149/150 comparées.
- `src/sync/DirettaSync.{cpp,h}` dans DRUP est une copie ancienne **non compilée** (le `Makefile` l.482-486 ne liste que `src/DirettaSync.cpp`). Tout ce qui suit vise `src/DirettaSync.cpp` / `.h`.

## 1. Diff API + sémantique 149 → 150

Diff brut complet (`diff -r 149/Host 150/Host`) : seuls 5 fichiers changent. THRED_MODE, MSMODE, `open()`, `setSink()`, `Profile::ModeType`, `Find`, `Format`, `Stream`, `Connection` sont **strictement identiques** entre 149 et 150.

### 1.1 `Release.hpp`
`ReleaseNo = 149` → `150`. (DRUP ne journalise pas `DIRETTA::ReleaseNo` dans `DirettaSync.*`.)

### 1.2 `Sync.hpp`

| 149 | 150 |
|---|---|
| `bool connect(int);` (aucun commentaire) | `/// @param CPU number occupied by the send thread(default -1 not set CPU occupied)` `/// @param Rapid Start (defalt play mode)` `bool connect(int=-1,bool=false);` (l.147-150) |
| — | `void statusUpdate(REQ_STATE);` **privé, non virtuel** (l.292), à côté du `virtual void statusUpdate();` protégé (l.259) inchangé |

- **`REQ_STATE`** [DOC] est un enum **privé** (l.284-290) : `DISCONNECT, CONNECT_REQ, CONNECT, DISCONNECT_REQ`. Les prédicats publics le lisent : `is_connect()` = `CONNECT_REQ||CONNECT||DISCONNECT_REQ`, `is_online()` = `CONNECT`, `is_disconnect()` = `DISCONNECT_REQ||DISCONNECT` (l.169-173). Le nouvel overload `statusUpdate(REQ_STATE)` étant privé, **DRUP ne peut ni l'appeler ni le surcharger** : le seul point d'accroche reste `virtual void statusUpdate()`. [INF] c'est vraisemblablement le refactor interne (état → notification → `cv`) qui a rendu fatal le stub vide de DRUP (la base doit être appelée pour réveiller `connectWait()`, cf. Yu Harada cité dans `DirettaSync.h` l.527-535).
- **Rapid Start** [DOC] : unique phrase, « Rapid Start (defalt play mode) ». Rien d'autre nulle part (grep « rapid » sur tout le doc 150 : 2 occurrences, ce paramètre dans `Sync::connect` et `SyncBuffer::connect`). [INF] `true` = démarrage rapide vs « play mode » par défaut — probablement saute une phase de stabilisation/pré-remplissage côté target avant `play()`. La note mémoire indique que `connect(-1,true)` a été testé le 2026-08-30 sans effet sur les 50 s (qui étaient dues au stub). **Sémantique réelle inconnue → question à poser à Yu.**
- **`connect(int cpu)`** [DOC] : le sens du paramètre est désormais écrit : numéro de CPU « occupé » par le thread d'envoi, `-1` = pas d'affinité. Conséquence pour DRUP : voir §2 (DRUP passe `0`).

### 1.3 `SyncBuffer.hpp` (classe *push* ; DRUP dérive de `Sync`, donc non concerné directement)
- `setupBuffer(FS, depth, noMute=false, FormatConfigure, bool firstCallbackImmediately=false)` retourne maintenant `bool` (« setup status »). [DOC] « FS frame count not byte size) », « depth (total size = FS*depth) », « Do not generate mute when depleted », « Call the first callback immediately after the connection is established (need FS>=2) ». **FS** = nombre de frames par `Stream` du tampon ; [INF] « need FS>=2 » signifie qu'il faut au moins 2 frames par stream pour qu'un callback immédiat ait un sens.
- `setStream(Stream&)` → `bool` : « false is Connection exit ».
- `writeStreamStart(bool& err)` documenté : « Retrieval error (Connection exit) ».
- `addStream` documenté : « Adding a stream—it is added regardless of setupBuffer ».
- `checkStreamStart()const` **supprimé** ; membre `std::atomic_bool notifyStreamFlg` **supprimé** ; `notifyStreamDone(Stream&,bool)` 2ᵉ paramètre passe de « End of playback » à « allways false » (le flag de fin n'est plus signalé par ce canal).
- `connect(bool callback, int cpu=-1, bool rapid=false)` — mêmes ajouts que `Sync::connect`.

### 1.4 `Profile.hpp`
`bool configTransferSizeFix(size_t bytes, bool noRemainder=false);` [DOC] « Transmission Size Specification Mode. @param Packet Data Size: Bytes @param NO remainder ». [INF] avec `true`, la taille de cycle est arrondie pour que le découpage en paquets ne laisse pas de paquet « reste » plus petit — donc tous les paquets d'un cycle ont la même taille. C'est le mode le plus proche d'un « paquet de taille constante à cadence constante ».

Rappel du reste de `Profile.hpp` (inchangé mais utile, [DOC]) : `Profile{ tergetSycleTime; VarSendSize (« if 0 is fix-cycle(variable-size) »); SendSizeMax; TypicalFrame; sendMultCount (« frames per cycle = sendMultCount × TypicalFrame »); randomMin; Mode ∈ {VARIABLE, FIX, RANDOM, TRIANGOLO} }`. `TRIANGOLO` n'est documenté nulle part ; `SinHost.cpp` l.174 le traite comme un cas spécial d'affichage de fréquence. `ProfileMaker::setForceFragment(bool)` : « Override packet fragmentation specification in TargetProfile ».

### 1.5 `ACQUA/Clock.hpp`
Ajout de `class ClockDiff : public Clock { reset(); reset(Clock); Clock update(); }` — chronomètre delta (`update()` retourne `now - précédent` et réarme). Rien de plus. À noter (149 et 150) : `extern "C" unsigned long long (*AcquaClockNow)(void);` — la source d'horloge du SDK est un **pointeur de fonction C global, substituable par l'hôte** ; `Clock` est en picosecondes.

## 2. Cartographie de l'usage SDK dans `DirettaSync` (2.5.15)

Fichier : `le clone v2.5.15\src\DirettaSync.cpp` (`.h` pour les overrides).

| Appel SDK | Où | Valeurs | Remarque vs doc / échantillon |
|---|---|---|---|
| `Find::Setting{Loopback=false, ProductID=0, Name, MyID=0x44525400}`, `find.open()`, `findOutput()` | 244-325, 367-435 | — | conforme à SinHost |
| `Find::measSendMTU(addr, mtu)` | 353 | fallback 1500 | conforme à SinHost l.110 |
| `Sync::open(THRED_MODE(threadMode), infoCycle, 0, "DirettaRenderer", 0x44525400, cpuMain, cpuOther, 0, MSMODE_AUTO)` | 207-210 (`openSDK`) | `threadMode` défaut **1** (+16 si cpuAudio) ; `infoCycle` 100 ms ; `rngOther=0` | SinHost utilise `THRED_MODE(5)` = CRITICAL\|NOSLEEP4CORE, 100 ms, MSMODE_AUTO |
| `inquirySupportFormat(addr)` | 231 (après open), 802 (après setSink, si full connect) | | « Retrieve the supported formats for Sink » ; le 2ᵉ appel est justifié par un commentaire « may be required » — [INF] inoffensif |
| `checkSinkSupport(fmt)` | 1057-1194 | essais 32→24→16 / DSD LSB\|BIG… | OK (nécessite `inquirySupportFormat` préalable) |
| `setSink(addr, cycleTime, false, m_effectiveMTU)` | 785 | **2ᵉ arg = cycleTime** (100 µs–50 ms calculé MTU/débit) | **Écart** : [DOC] le 2ᵉ paramètre est « Sink buffer time (if zeoro use default sink buffer time) », pas le cycle. SinHost passe `Clock::MilliSeconds(100)` (« target buffer request »). DRUP demande donc au target un tampon égal à la période d'un paquet (ex. ≈ 8,5 ms à 44,1 k/16, ≈ 1 ms à 352,8 k/32). [INF] le target borne sûrement par `Info.latencyBuffer/latencyMax`, mais la demande est incohérente avec l'intention. 3ᵉ arg `false` = « Disable Sink's playback rejection (NOP) » désactivé (SinHost pull passe `true`). |
| `setSinkConfigure(m_pendingSinkFormat)` | 797 | | **Corrigé en 2.5.15** : après `setSink` (règle Yu Harada) |
| `applyTransferMode` : `configTransferFixAuto / VarAuto / VarMax / Random` sur `Sync` (SelfProfile) ou via `getProfileMaker(limit)` + `setConfigTransfer(pm)` (TargetProfile si `targetProfileLimitTime>0`) | 2082-2152 | AUTO → VAR_AUTO si DSD ou ≤48 k/16 bit, sinon VAR_MAX ; random min 333 µs, count 1 | **Écart 1** : `configTransferFixAuto/VarAuto` sont commentés dans `Sync.hpp` l.127-130 « Called when Target Profile (Auto) is active » — DRUP les utilise précisément dans le chemin **Self**Profile. **Écart 2** : `configTransferAuto(min, target, max)` (l.110-114, celui qu'utilise SinHost : `200 µs, 0, 100 ms`) n'est jamais appelé. **Écart 3** : `inquiryParameter(addr,u32,u32)` « Retrieve the TargetProfile for the Sink » (l.187-188) n'est jamais appelé, alors que le chemin TargetProfile (`getProfileMaker`) en dépend logiquement ([INF] explique peut-être son statut « experimental »). |
| `connectPrepare()` | 809 | défaut `true` = « Automatically adjust Trget delay » | OK |
| `connect(0)` | 820 | **CPU 0** | Avec la doc 150, `0` = « CPU number occupied by the send thread » → épingle le thread d'envoi SDK sur le cœur 0 (celui des IRQ en général), alors que DRUP épingle son propre worker sur `cpuAudio` (1975-1992). SinHost 147 faisait aussi `connect(0)` (l.180), mais à l'époque sans doc. [INF] à remplacer par `connect(sdkCpuMain)` ou `connect(-1)`. Le test du 30/08 « `connect(-1)` sans effet » portait sur les 50 s, pas sur le placement du thread. |
| `connectWait()` | 828 | | dépend du `statusUpdate()` de base → corrigé 2.5.15 |
| `play()` / `stop()` | 543, 842, 1414, 1447, 1464, 1483 | | OK |
| `disconnect(true)` | 598, 646, 830, 897, 960 | | OK |
| `DIRETTA::Sync::close()` | 178, 604, 652, 900, 940, 966 | | fermeture complète à chaque changement de format et à chaque `close()` |
| `is_online()` | 406, 1502, 2068 | | OK |
| `getSinkInfo()` | 438-467, 1099-1102 | PCM/DSD/MS bits | `latencyBuffer/latencyMax/latencyHw/maxSize/minMTU/reqMTU/maxMTU` **jamais lus ni journalisés** |
| Overrides | `.h` 523-535 | `getNewStream` (1686-1948), `getNewStreamCmp(){return true;}`, `startSyncWorker` (1950-2002 : thread propre, SCHED_FIFO `g_rtPriority`, affinité `cpuAudio`, boucle `syncWorker()` + sleep 100 µs), `statusUpdate(){Sync::statusUpdate();}` | `getNewStream` respecte « pointer must remain valid until … this function is called again » via `m_streamData` persistant ; ne retourne jamais `false` (« false indicates termination ») — l'arrêt passe par `stop()/disconnect()`. |
| Non utilisés | — | `is_MSmode()`, `getCycleTime/getMinCycleTime/getCycleSize/getCyclePackets/getMode`, `getLatency()`, `changeWorkMode()`, `currentWorkMode()`, `inquiryParameter()`, `mtuTest/mtuCheck`, `setHostInvertPhase`, `configTransferAuto`, `configTransferFix/Var` (non-Auto), `ProfileMaker::configTransferSizeFix/setForceFragment`, `Sync(udp, raw)`, `Find::Setting::NopBreak/LimitVersion`, `SysLogDiretta` (branché en 2.5.15 via `DIRETTA_SDK_SYSLOG_DEBUG`, `main.cpp` 451-452) | |

Contradiction documentaire côté DRUP : `docs/CONFIGURATION.md` l.113 liste `SOCKETNOBLOCK = 8` alors que le SDK l'a **commenté** (`//SOCKETNOBLOCK=8`, `Sync.hpp` l.30) ; [INF] le bit 8 est un no-op. Même fichier l.135 « 2620 µs base » ne correspond plus au calcul actuel (`DirettaCycleCalculator`, `.h` 293-313 : `(MTU-3)/débit`, borné 100 µs–50 ms).

Remarque sur `logSinkCapabilities` (457) : « the actual negotiated MS mode (MSmodeSet) is private » — vrai pour le membre, mais **`is_MSmode()` est public** (l.176-177, [DOC] « If `is_online` is false MSMODE_NONE is returned ») ; DRUP devine au lieu de lire.

## 3. Opportunités concrètes (150, et 149 déjà inexploité)

### 3.1 Nouveautés 150 proprement dites
1. **`connect(cpu, rapidStart)`** — [DOC] cité §1.2. Usage : `connect(sdkCpuMain /* -1 si non configuré */, m_config.rapidStart)`. Bénéfice : cohérence de l'affinité (ne plus forcer le cœur 0) ; « Rapid Start » = inconnu, à mesurer (temps `connect→online`, comportement au 1ᵉʳ paquet). Risque : Rapid Start non documenté ; à exposer comme option off par défaut. Code : `m_config.rapidStart` + 1 ligne l.820.
2. **`configTransferSizeFix(bytes, noRemainder=true)`** (ProfileMaker) — [DOC] §1.4. Usage : nouveau `--transfer-mode sizefix` avec `--cycle-size <bytes>` (ex. `bytesPerBuffer` déjà calculé, aligné frame) ; nécessite le chemin ProfileMaker (`getProfileMaker(limit)` → `pm.configTransferSizeFix(n, true)` → `setConfigTransfer(pm)`). Bénéfice attendu [INF] : paquets tous de même taille → période d'émission la plus régulière possible, ce qui est exactement l'objectif « packet-timing regularity ». Risque : dépend du TargetProfile (voir 3.3.3) ; retour `bool` à contrôler.
3. **`ClockDiff`** — usage diagnostic : dans `getNewStream`, `static ACQUA::ClockDiff d; auto dt = d.update();` et accumuler min/max/écart-type des intervalles d'appel (à sortir dans `dumpStats()`). Coût nul, aucun risque, donne enfin une mesure de gigue côté hôte.
4. **`statusUpdate()`** — déjà réglé ; garder l'appel de base (règle Yu). L'overload `REQ_STATE` est inaccessible.

### 3.2 THRED_MODE inexploités (149 = 150) — tous [DOC] cités ci-dessous
- `NOSHORTSLEEP=2` « Do not enter Sleep mode for a short period of time (busy loop) » ; `NOSLEEP4CORE=4` « If fewer than four cores are available, do not perform a busy loop » (garde-fou du précédent ; SinHost = 1|4) ; `NOSLEEPFORCE=2048` « force busy loop » ; `IDLEONE=512` « Run Idle every time (busy loop) » ; `IDLEALL=1024` « Always run Idle (busy loop) ». [INF] famille « busy-wait » : sur un hôte isolé (RPi5 / x86 tune avec cœur dédié), c'est le levier le plus direct sur la régularité d'émission ; à combiner avec `OCCUPIED` et un cœur isolé.
- `FEEDBACKOFFSET=32` « Move the transmission feedback to the moving average », champ 3 bits (`FEEDBACKMASK=0x7*32`, valeurs 32..224), `NOFASTFEEDBACK=256` (sans commentaire). [INF] lissage de la boucle d'asservissement hôte↔target (feedback de remplissage) — impacte la régularité des corrections de cadence. À tester par valeurs 32/64/128/224.
- `LIMITRESEND=4096` « Limit the amount of retransmission buffer allocated » ; `NOJUMBOFRAME=8192` « Do not use jumbo frame(MTU) » ; `NOFIREWALL=16384` « Do not send packets to disable the firewall » ; `NORAWSOCKET=32768` « Do not use raw socket(no use DDS mode3) ».
- **`changeWorkMode(THRED_MODE)`** (l.89) : change le mode **à chaud**, sans `close/open`. Permet un A/B pendant la lecture (ex. commande SIGUSR2 ou endpoint Web UI). `currentWorkMode(cpuMain,cpuOther,rng)` pour journaliser l'état réel.
- `rngOther` d'`open()` : « CPU number of the other thread random range (when occupied) » — DRUP passe 0 ; permet de répartir les threads « autres » du SDK sur une plage de cœurs.

### 3.3 Cycle / profil / tampon
1. **Tampon target (`setSink` 2ᵉ arg)** : passer `0` (défaut du sink) ou une valeur dédiée `--sink-buffer-ms` (SinHost : 100 ms) au lieu du cycle. Bénéfice [INF] : marge de latence côté target découplée de la cadence paquets ; potentiellement moins de corrections. Risque : latence audible plus élevée, borne `Info.latencyMax`. Code : l.785 + champ config.
2. **`configTransferAuto(minSyncTime, targetCycle, maxCycle)`** [DOC] « Minimum Sync System Time / Target Cycle Time (Set zero as the default setting / Maximum Cycle Time (Recovery procedure when the system is busy » — mode utilisé par l'auteur (`200 µs, 0, 100 ms`) et absent de DRUP. Ajouter `--transfer-mode auto-sdk`.
3. **TargetProfile complet** : appeler `inquiryParameter(addr, ?, ?)` avant `getProfileMaker()` (les deux `uint32_t` ne sont pas documentés — [INF] probablement `NoBaseMin`/`FrameMax` cf. `ProfileMaker::setParameter` et `Sync::getNoneBaseMin()/getFrameMax()` ; à confirmer). C'est le préalable pour que `configTransferFixAuto/VarAuto` (« Called when Target Profile (Auto) is active ») aient le sens prévu.
4. **Journaliser la négociation réelle** : après `applyTransferMode` et après `connectWait`, sortir `getCycleTime()`, `getMinCycleTime()`, `getCycleSize()`, `getCyclePackets()`, `getMode()`, `getLatency()`, `is_MSmode()`, et `Info.{latencyBuffer,latencyMax,latencyHw,maxSize,reqMTU,maxMTU}` (exactement ce que SinHost l.172-178 affiche). Permet de vérifier que `m_bytesPerBuffer` (1267-1303) coïncide avec `getCycleSize()` — sinon le SDK refragmente/regroupe et l'« alignement 1 ms » de DRUP est illusoire. Risque nul.
5. **`Info.reqMTU / maxSize`** pour dimensionner `bytesPerBuffer` à la place de l'heuristique `OVERHEAD=3` (« Tested by Hoorna »). [INF].
6. **DDS mode 3 / raw socket** : `Sync(udp, raw)` + `FormatConfigure::setDDS(true)` + bit `FormatID::DDS` ; désactivable par `NORAWSOCKET`. Rien n'indique que le DDC-0/Red le supporte ; [INF] à explorer seulement avec Yu.
7. **`AcquaClockNow`** substituable : [INF] permettrait une horloge TSC/`CLOCK_MONOTONIC_RAW` ; risqué (toute la temporisation du SDK en dépend), à ne pas toucher sans accord de l'auteur.

### 3.4 Silence / repos
Rien de nouveau côté SDK : `Sync` (pull) ne génère pas de mute ; c'est `SyncBuffer` qui a « Do not generate mute when depleted ». DRUP gère déjà son propre silence (prefill, stabilisation, rebuffering). La seule interaction : `getNewStream` retournant `false` = terminaison, jamais utilisé.

## 4. Boutons à exposer pour tests A/B (avec plages)

| Bouton | État DRUP | Plage documentée | Plage suggérée A/B |
|---|---|---|---|
| `--thread-mode` (bitmask) | existe, défaut 1 | aucune ; SinHost = 5 | 1 / 3 / 5 / 17 / 21 (5+16) / +2048 / +512 / +1024 ; FEEDBACK 32·k (k=1..7) ; +256 ; +4096 ; +8192 si MTU>1500 |
| `changeWorkMode` à chaud | absent | — | même bitmask, sans reconnexion |
| `--cycle-time` | existe (auto = (MTU−3)/débit, borné 100 µs–50 ms ; CLI avertit hors 333–10000 µs) | aucune ; SinHost : min 200 µs, max 100 ms | auto / 1 / 2 / 5 / 10 ms |
| `--transfer-mode` | auto/varmax/varauto/fixauto/random | — | + `auto-sdk` (`configTransferAuto`), + `sizefix` (`configTransferSizeFix(n,true)`), + `fix`/`var` non-Auto avec `--fragments <n>` |
| `--sink-buffer-ms` (nouveau) | = cycle (bug de sens) | « if zero use default » ; SinHost 100 ms | 0 (défaut) / 20 / 50 / 100 / 200 ms, ≤ `Info.latencyMax` |
| `--info-cycle` | existe, 100 ms | aucune ; SinHost 100 ms | 50 / 100 / 200 / 500 ms [INF] |
| `--target-profile-limit` | existe (µs, 0 = Self) | « limitCycle Minimum period time (maximum transmission frequency) » | 200 µs–2 ms, **après** `inquiryParameter` |
| `--rapid-start` (nouveau) | absent | « Rapid Start (defalt play mode) » | off / on |
| `--connect-cpu` (nouveau) ou réutiliser `cpuAudio` | forcé à 0 | « default -1 not set » | −1 / cœur isolé |
| `--ms-mode` | AUTO fixe | NONE / MS1 (« 1or0 ») / MS2 (« must 2 ») / MS3 (« 3or1 ») / AUTO (« 3 1 0 ») | AUTO / NONE / MS1 / MS3 |
| `--cycle-min-time` | existe (random) | — | inchangé |

## 5. Résumé des points à corriger / à demander à Yu Harada

1. `setSink` 2ᵉ argument : DRUP passe le cycle, la doc dit « Sink buffer time » (`DirettaSync.cpp` l.785).
2. `connect(0)` épingle le thread d'envoi SDK sur le cœur 0 selon la doc 150 (l.820).
3. `configTransferFixAuto/VarAuto` utilisés hors TargetProfile, `inquiryParameter` jamais appelé, `configTransferAuto` (celui de l'auteur) jamais utilisé (l.2082-2152).
4. `is_MSmode()` public ignoré ; aucune trace de `getCycleTime/getCycleSize/getLatency` dans les logs.
5. `CONFIGURATION.md` : `SOCKETNOBLOCK=8` obsolète (commenté dans le SDK), « 2620 µs base » obsolète.
6. Questions pour l'auteur : sémantique exacte de « Rapid Start » ; signification des deux `uint32_t` d'`inquiryParameter` ; effet de `FEEDBACKOFFSET`/`NOFASTFEEDBACK` ; ce que fait `TRIANGOLO` ; si `connect(cpu)` s'applique au thread créé par `startSyncWorker()` surchargé ou à un thread interne.

Fichiers de référence : `<SDK 150>\Host\{Sync,SyncBuffer,Profile,Release}.hpp`, `<SDK 150>\Host\ACQUA\Clock.hpp`, `<SDK 150>\doc\classDIRETTA_1_1{Sync,SyncBuffer,ProfileMaker}.html`, `<SDK 147>\SinHost\SinHost.cpp`, `le clone v2.5.15\src\DirettaSync.{cpp,h}`, `le clone v2.5.15\CHANGELOG.md` (2.5.15, 2.1.0, 1.3.x), `le clone v2.5.15\docs\CONFIGURATION.md` l.100-140.
