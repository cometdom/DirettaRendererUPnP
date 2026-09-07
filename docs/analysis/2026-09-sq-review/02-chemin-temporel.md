# Audit du chemin temporel — DirettaRendererUPnP 2.5.15 → SDK Diretta Host 150

*Audit réalisé le 2026-09-07 (agent d'analyse, rapport brut archivé sans retouche).*

Périmètre : lecture intégrale de `src/DirettaSync.{h,cpp}`, `src/DirettaRingBuffer.h`, `src/main.cpp`, `src/memcpyfast_audio.h`, `src/FastMemcpy_Audio.h`, `src/LogLevel.h`, `src/TimestampedLogger.h`, les parties producteur de `src/DirettaRenderer.cpp` et `src/AudioEngine.cpp`, les deux tuners, `systemd/*`, `Makefile`, les deux rapports d'optimisation de janvier, et les en-têtes SDK (`Sync.hpp`, `SyncBuffer.hpp`, `Profile.hpp`, `Stream.hpp`, `Connection.hpp`, `ACQUA/{Clock,ThreadPriority,Ethernet,Socket}.hpp`) + la doc Doxygen.

**Limite méthodologique importante** : la lib statique `libDirettaHost_x64-linux-15v3.a` n'est pas disponible sur cette machine (tarball 150_4 en `.zst`, pas de `zstd`/`nm`/`objdump` ici). Tout ce qui se passe *à l'intérieur* de `syncWorker()` (attente, envoi, feedback) est donc déduit des en-têtes et marqué **[SUSPECTÉ]**. Tout ce qui est marqué **[VÉRIFIÉ]** est lu dans le code source ou les en-têtes.

---

## 1. Chronologie d'un cycle du thread worker SDK

### 1.1 Qui est ce thread et comment il tourne

- Le thread n'est **pas** celui du SDK (`Sync::thread_sync_node`, `Sync.hpp:280-281`) : DRUP surcharge `startSyncWorker()` et crée son propre `std::thread` qui boucle `while (m_running) { if (!syncWorker()) sleep_for(100µs); }` (`DirettaSync.cpp:1967-1999`). [VÉRIFIÉ]
- Avant la boucle : `pthread_setschedparam(SCHED_FIFO, g_rtPriority=80)` (`:1970`, `main.cpp:77,301-307`) puis `pthread_setaffinity_np` sur `--cpu-audio` (`:1975-1992`). [VÉRIFIÉ]
- Le thread est créé depuis le thread qui appelle `connect()` — c'est le thread audio/décode (le callback `open()` est appelé depuis `AudioEngine::process()` → `DirettaRenderer.cpp:445`), donc il hérite un instant de l'affinité cœur 3 / FIFO 80 avant de se re-épingler. Sans conséquence. [VÉRIFIÉ]
- Quand `syncWorker()` renvoie `false` (déconnecté / rien à faire, [SUSPECTÉ]), la boucle fait un `nanosleep(100µs)` → ~10 000 réveils/s sur le cœur 2 hors lecture. Sans effet pendant la lecture, mais c'est du bruit inutile entre pistes / en warmup.

### 1.2 Ce que fait `syncWorker()` entre deux `getNewStream()` [SUSPECTÉ, déduit des en-têtes]

1. Lecture d'horloge via `AcquaClockNow` (pointeur de fonction C, `Clock.hpp:5,46`) → très probablement `clock_gettime(CLOCK_MONOTONIC)` vDSO (à vérifier : `current_clocksource` doit être `tsc`, sinon chaque lecture est un vrai syscall HPET).
2. Attente jusqu'à l'échéance du cycle selon le mode de la socket (`EthernetSocket::RCV_MODE`, `Ethernet.hpp:86-97`) : `MODE_SLEEP` (défaut : sommeil/`select` avec timeout), `MODE_BUSY_SHORT` (spin sur les attentes courtes = flag `NOSHORTSLEEP`), `MODE_BUSY_FULL` (spin permanent = `NOSLEEPFORCE`), variantes `_IDLE`/`_ONEIDLE` (spin + `sched_yield` via `ThreadPriority::idle()`, `ThreadPriority.hpp:38-39` = flags `IDLEALL`/`IDLEONE`).
3. Appel(s) `getNewStream()` pour obtenir `Size` octets ; copie interne par `memcpySyncFunc` (variantes Org/AntiPhase/ByteSwap/BitSwap, `Sync.hpp:263-276`) vers le tampon paquet — donc **une seconde copie** de chaque octet après notre `pop()`.
4. Émission : socket raw AF_PACKET « DDS mode 3 » par défaut (`Connection.hpp:117-127`, flag `NORAWSOCKET` pour l'inhiber) ou UDP/IPv6 (`Socket.hpp:75-81`).
5. Réception du feedback cible (non bloquante ou via `Socket::Waiter` = `select`, `Socket.hpp:113-138`), correction du cycle (flags `FEEDBACK*`).
6. Toutes les `infoCycle` (100 ms par défaut, `DirettaSync.h:332`) : un paquet d'information supplémentaire — perturbation périodique de la grille d'envoi à 10 Hz.
7. Gestion du tampon de retransmission (`LIMITRESEND`).

La question centrale non tranchée par le code : le SDK appelle-t-il `getNewStream()` **une fois par paquet** (et envoie nos 264 octets tels quels), ou **N fois pour remplir un paquet de la taille du profil** (`getCycleSize()`) ? DRUP ne journalise jamais `getCycleTime()/getCycleSize()/getCyclePackets()/getLatency()` (grep : aucun appel dans `src/`). Voir §2.4 et l'action P0 en fin de rapport.

### 1.3 Ce que fait `getNewStream()` (`DirettaSync.cpp:1686-1948`) — chemin nominal PCM [VÉRIFIÉ]

| Étape | Ligne | Opération | Coût / partage |
|---|---|---|---|
| a | 1692 | `m_workerActive = true` | store **seq_cst** (`xchg`, barrière complète) |
| b | 1696-1709 | `m_consumerStateGen.load(acquire)` ; rechargement du cache si changement | 1 load ; chemin froid rare |
| c | 1736-1751 | accumulateur de reste 44,1 k (`m_framesPerBufferAccumulator` relaxed load/store) | privé consommateur |
| d | 1755-1757 | `m_streamData.resize()` si la taille change (264↔270 octets à 44,1 k, 1 fois sur 10) | après la 1re croissance, pas de réallocation (capacité conservée) ; 1 alloc heap sur le thread RT **par piste** |
| e | 1761-1762 | `baseStream.Data.P/Size` | – |
| f | 1766-1771 | `RingAccessGuard` : `fetch_add(acq_rel)` + `fetch_sub(release)` sur `m_ringUsers` | **2 RMW verrouillés** sur une ligne partagée avec le producteur (voir §3.2) |
| g | 1777, 1786, 1793, 1804 | loads acquire `m_silenceBuffersRemaining`, `m_stopRequested`, `m_prefillComplete`, `m_postOnlineDelayDone` | loads simples x86 |
| h | 1867 | `m_streamCount.fetch_add(relaxed)` | **1 RMW verrouillé** par appel, purement statistique |
| i | 1868 | `getAvailable()` : loads `writePos_`/`readPos_` | `writePos_` écrit par le producteur ~100×/s → miss occasionnel inévitable |
| j | 1882, 1903 | tests rebuffering / underrun | branches stables |
| k | 1915 | `pop()` (`DirettaRingBuffer.h:1347-1363`) : `getAvailable()` (2 loads) + `memcpy_audio` + `readPos_.store(release)` | copie 264–3816 o |
| l | 1941-1944 | `m_flowMutex.try_lock()/unlock()` + `notify_one()` | ~3 opérations atomiques ; **0 syscall** si aucun waiter (glibc vérifie `__wrefs`) |
| m | 1946 | `m_workerActive = false` | store **seq_cst** |

Total nominal : ~5 RMW verrouillés, ~10 loads, 1 memcpy, **0 syscall, 0 allocation, 0 log** en régime établi (build NOLOG). C'est propre. Les branches silence/stabilisation/underrun remplacent `pop()` par `memset` et sortent plus tôt.

Points particuliers demandés :
- **Silence de stabilisation** (`:1804-1865`) : calcul en double (division) à chaque appel pendant la phase, puis `memset`. Uniquement au démarrage de piste. Sans enjeu.
- **Rebuffering / underrun** (`:1882-1912`) : entrée en rebuffering = `LOG_WARN` (`:1907`), sortie = `LOG_WARN` (`:1891`). `LOG_WARN` n'est **pas** compilé out par NOLOG (`LogLevel.h:13-14, 30-32`) et reste actif en `--quiet`. C'est un `std::cout` → `TimestampedStreambuf::overflow` (`TimestampedLogger.h:59-74` : `stringstream`, `localtime`, allocation) → `write(2)` vers journald **depuis le thread FIFO 80**. Régression par rapport à l'item C6 du rapport « HOT PATH » (« Eliminates blocking I/O from hot path entirely »).
- **DoP** : aucune réécriture de marqueur dans `getNewStream()` ; les marqueurs sont posés au push (`pushDSDToDoP`, `DirettaRingBuffer.h:532-581`). Côté consommateur il n'y a que la parité de trames (`:1739-1743`) et le silence 0x00 (`:1716-1727`). [VÉRIFIÉ]
- **Barrières `beginReconfigure`** (`:2008-2017`) : spin `yield()` côté configurateur, jamais côté worker (le worker « bail-out » et remplit du silence, `:80-110, 1766-1771`). OK.
- **Chemin verbose** (`g_verbose`) : `DIRETTA_LOG_ASYNC` construit un `ostringstream` (allocations heap) sur le thread RT (`:1870-1876`) puis pousse dans `LogRing` ; le DoP verbose fait des `printf` directs (`:1918-1936`). À ne jamais activer pour l'écoute — c'est le cas chez vous (`--quiet`).

### 1.4 Lignes de cache partagées avec les autres threads [VÉRIFIÉ pour l'adjacence, SUSPECTÉ pour les frontières exactes — aucun `alignas` dans `DirettaSync`]

- `m_running(616) / m_stopRequested(617) / m_draining(618) / m_workerActive(619)` : le worker **écrit** `m_workerActive` 2×/appel ; le producteur **lit** `m_draining` et `m_stopRequested` à chaque `sendAudio()` (`:1500-1501`) → faux partage réel : ~100 RFO/s côté worker à des instants imprévisibles.
- `m_openAbortRequested(633) / m_onlineTimeoutOccurred(634) / m_reconfiguring(635) / m_ringUsers(636) / m_flowMutex(641)` : `m_ringUsers` est modifié par **les deux** threads (guard producteur `:1511`, `getBufferLevel` `:1633`, guard consommateur `:1766`) → vrai partage, ~200 transferts de ligne/s. `m_onlineTimeoutOccurred` est lu par le producteur à chaque push (`:1504,1507`).
- `m_streamCount(716)` RMW par cycle côté worker, sur la même ligne que `m_pushCount(717)` (RMW producteur seulement en verbose) et `m_rebuffering(720)`.
- Ring : `writePos_`/`readPos_` sont bien `alignas(64)` (`DirettaRingBuffer.h:1472-1473`) ; `size_/mask_/buffer_` (1469-1471) sont en lecture seule en régime établi. OK.
- `LogRing` (`DirettaSync.h:97-99`) : correctement aligné, mais n'existe qu'en verbose.

### 1.5 Ce qui peut faire varier l'instant d'émission (synthèse)

1. Mode d'attente du SDK (sommeil vs spin) — de loin le premier facteur ; DRUP laisse le défaut « sleep ».
2. Priorité/affinité effectives du worker (voir §3.1 et §5.2 : le drop-in systemd met tout le processus en FIFO **90**, le worker se rétrograde à **80** ; le SDK `CRITICAL`/`OCCUPIED`/`connect(0)` peuvent aussi les modifier).
3. IPI de shootdown TLB provoqués par `munmap`/`brk` des autres threads (FFmpeg, libupnp, `std::vector`) — atteignent le cœur 2 (voir §5.4).
4. IRQ gérées (NVMe, files MSI-X) que `irqaffinity=0` ne couvre pas (voir §5.3).
5. Feedback cible reçu sur le cœur 0 puis réveil du worker par IPI (voir §5.3).
6. Le paquet d'information toutes les 100 ms.
7. Les `LOG_WARN` sur underrun.

---

## 2. Configuration SDK réellement en vigueur

### 2.1 Paramètres passés au SDK (déploiement décrit) [VÉRIFIÉ]

| Paramètre | Valeur effective | Source |
|---|---|---|
| `THRED_MODE` | `1 \| 16 = 17` (CRITICAL + OCCUPIED, ce dernier ajouté automatiquement car `--cpu-audio`) | `DirettaSync.h:329`, `DirettaSync.cpp:200-205` |
| `open(mode, info, ifno, name, id, cpuMain, cpuOther, rngOther, msMode)` | `(17, 100 000 µs, 0, "DirettaRenderer", 0x44525400, 2, 1, 0, MSMODE_AUTO)` | `DirettaSync.cpp:207-210` |
| `setSink(addr, Clock, bool, MTU)` | `(target, cycleTime, false, 3824)` | `DirettaSync.cpp:785` |
| `setSinkConfigure` | après `setSink` (correctif 2.5.15) | `:797` |
| Transfert | `AUTO` → `VAR_MAX` pour PCM (sauf 16 bit ≤48 k accepté en 16 bit par le sink) ; `VAR_AUTO` pour DSD | `:2085-2092` |
| Profil | `targetProfileLimitTime=0` → chemin « SelfProfile » (appels directs sur `Sync`) | `:2095, 2128-2151` |
| `connectPrepare(true)` / `connect(0)` / `connectWait()` | | `:809, 820, 828` |
| Cycle | auto : `round(3821 / octets_par_seconde × 1e6)` borné [100 ; 50 000] µs | `DirettaSync.h:298-308` |

### 2.2 Formule de cycle avec MTU 3824 (efficace 3821) et taille des callbacks

Le débit utilise les bits **négociés avec le sink**, pas ceux de la source : pour 16 bit, `configureSinkPCM` n'essaie le 32 bit que si la source est 32 bit et prend le **24 bit** dès qu'il est supporté (`DirettaSync.cpp:1055-1075`) → CD = 6 o/trame sur le fil (`push16To24`, boucle **scalaire**, `DirettaRingBuffer.h:729-738`).

| Format | Octets/s sur le fil | `cycleTime` passé à `setSink`/`configTransfer*` | Taille `Size` par `getNewStream` (`:1279-1303`, `:1367-1379`) | Appels/s | Ring (pow2, `:149-156`) / préremplissage |
|---|---|---|---|---|---|
| 44,1/16 → 24 bit | 264 600 | **14 441 µs** (21 661 si sink 16 bit) | 264 o (270 tous les 10) — « 1 ms » | 1000 | 262 144 o (0,99 s) / 80 ms |
| 96/24 | 576 000 | **6 634 µs** | 576 o | 1000 | 524 288 o / 80 ms |
| 192/24 | 1 152 000 | **3 317 µs** | 1152 o | 1000 | 1 048 576 o / 80 ms |
| 384/24 | 2 304 000 | 1 658 µs | 2304 o | 1000 | – |
| 768/32 | 6 144 000 | 622 µs | 3816 o (MTU) | ~1610 | – |
| DSD64 | 705 600 | **5 415 µs** | 704 o | 1000 | 1 048 576 o (1,49 s) / 200 ms |
| DSD256 | 2 822 400 | 1 354 µs | 2824 o | 1000 | – |
| DSD512 | 5 644 800 | 677 µs | 3816 o | ~1479 | – |

Pour comparaison en MTU 1500 (1497) : 44,1/24 → 5 658 µs ; 96/24 → 2 599 µs ; 192/24 → 1 300 µs ; DSD64 → 2 122 µs. **Le jumbo 3824 allonge le cycle d'un facteur 2,55.**

Incohérences documentaires : `--cycle-time` annonce 333–10 000 µs (`main.cpp:276`, `diretta-renderer.conf:188-191`) alors que l'auto produit 14 441 µs en CD ; `docs/CONFIGURATION.md:135` parle d'une « base 2620 µs » obsolète ; `SOCKETNOBLOCK=8` est commenté dans le SDK (`Sync.hpp:30`) mais toujours listé par DRUP (`main.cpp:393`, `conf:167`).

### 2.3 Sens de chaque bit de `THRED_MODE` (citations `Sync.hpp:21-51`, doc Doxygen identique) et effet attendu ici

| Bit | Citation SDK | Effet chez vous |
|---|---|---|
| CRITICAL 1 | « Set the priority of the sending thread to critical » | Le SDK applique lui-même `ThreadPriority::setPriority(CRITICAL)` (`ThreadPriority.hpp:18-30`) — valeur inconnue ; **peut écraser** le FIFO 80 posé par DRUP si c'est fait dans `syncWorker()` [SUSPECTÉ]. À vérifier avec `ps -eLo tid,cls,rtprio,psr,comm`. |
| NOSHORTSLEEP 2 | « Do not enter Sleep mode for a short period of time (busy loop) » | `MODE_BUSY_SHORT` : spin sur la fin d'attente au lieu de `nanosleep`. Supprime la latence de réveil (timer + C1 exit + ordonnanceur) — le candidat n°1 pour la régularité sur un cœur isolé. Non exposé autrement que par le bitmask brut. |
| NOSLEEP4CORE 4 | « If fewer than four cores are available, do not perform a busy loop » | Garde-fou ; avec 4 cœurs `nosmt` vous êtes pile à la limite (la condition « <4 » est fausse → spin autorisé). |
| OCCUPIED 16 | « Use the CPU by fixing it to a thread (not exclusively) » | Épinglage SDK de son thread principal sur `cpuMain=2`, des autres threads SDK sur `cpuOther=1`. Le CHANGELOG (:239) note que ce n'est pas fiable partout — d'où le double épinglage DRUP. |
| FEEDBACKOFFSET 32 / masque 0xE0 | « Move the transmission feedback to the moving average » | Champ 3 bits (0–7) : fenêtre de moyenne glissante du feedback cible → **c'est le paramètre qui règle la nervosité de l'asservissement « host-clocked »**. 0 = feedback immédiat (défaut). Jamais documenté ni exposé par DRUP autrement que par le bitmask. |
| NOFASTFEEDBACK 256 | – | Désactive le chemin de correction rapide. Idem. |
| IDLEONE 512 / IDLEALL 1024 | « Run Idle every time / Always run Idle (busy loop) » | Insère `sched_yield` dans le spin. Inutile sur un cœur dédié (rien d'autre à laisser passer), et `IDLEALL` ajoute un syscall par itération. |
| NOSLEEPFORCE 2048 | « force busy loop » | `MODE_BUSY_FULL` : 100 % du cœur 2, réveil déterministe, mais +5–8 W sur un fanless et nécessite §5.1 (throttling RT / fair server). |
| LIMITRESEND 4096 | « Limit the amount of retransmission buffer allocated » | Moins de mémoire verrouillée et d'empreinte cache ; sur lien direct sans perte, sans inconvénient attendu. |
| NOJUMBOFRAME 8192 | « Do not use jumbo frame » | Équivalent à `--mtu 1500` côté SDK. |
| NOFIREWALL 16384 | « Do not send packets to disable the firewall » | Supprime un trafic de contrôle périodique [SUSPECTÉ] ; sur lien direct firewalld off, candidat sans risque. |
| NORAWSOCKET 32768 | « Do not use raw socket (no use DDS mode3) » | À laisser **désactivé** : le mode raw AF_PACKET (`Connection.hpp:117-127`) évite la pile IP/UDP par paquet. |

`MSMODE_AUTO` (« 3 1 0 ») : d'après `ConnectionBuffer(…, MSmode=3)` réservé au constructeur raw (`Connection.hpp:127`) et le libellé « DDS mode3 » de `NORAWSOCKET`, MS3 désigne très probablement le transport raw Ethernet, pas une redondance de flux [SUSPECTÉ]. DRUP ne logge que les capacités (`logSinkCapabilities`, `:445-467`), jamais le mode réellement négocié.

### 2.4 Modes de transfert : ce que disent les en-têtes et ce que DRUP en fait

- `Profile` (`Profile.hpp:17-47`) : `VarSendSize` « if 0 is fix-cycle (variable-size). Specifies the size … for variable-cycle (fix-size) » ; `Mode ∈ {VARIABLE, FIX, RANDOM, TRIANGOLO}`. Donc **VAR = taille de paquet fixe, cycle variable** ; **FIX = cycle fixe, taille variable** (`TypicalFrame`, `sendMultCount`).
- `configTransferVarMax(cycle)` (`Sync.hpp:125-126`) « Adjust to use the maximum network packet size » : paquets pleins MTU, période = 3821/débit → **69 paquets/s en CD, 302/s en 192/24**, si le SDK agrège bien nos callbacks de 1 ms [SUSPECTÉ]. Le `cycleTime` auto de DRUP vaut exactement le temps de remplissage d'un MTU, ce qui autorise le cycle le plus long possible.
- `configTransferFix(cycle, fragments)` / `configTransferVar(cycle, fragments)` / `configTransferAuto(min, target, max)` (`Sync.hpp:110-124`) : **jamais exposés** par DRUP — ce sont pourtant les API « SelfProfile » génériques.
- `configTransferFixAuto/VarAuto` (`Sync.hpp:127-130`) portent le commentaire « Called when Target Profile (Auto) is active », mais DRUP les appelle dans le chemin **SelfProfile** (`DirettaSync.cpp:2130-2136`), y compris pour tout le DSD par défaut. Usage possiblement hors contrat [SUSPECTÉ].
- `configTransferRandom(min, max, 1)` : cycle aléatoire entre `cycleMinTime` (333 µs par défaut) et `cycleTime` — dithering temporel des émissions. `TRIANGOLO` (`Profile.hpp:43`) n'est pas exposé.
- `ProfileMaker` (`Profile.hpp:49-133`) : `configTransferSizeFix(bytes)`, `setForceFragment()`, `setParameter(min,max,pro,…)` = profil annoncé par la **cible** — accessible seulement via `--target-profile-limit >0` (« expérimental »). Holo Red fw 148 annonce donc une plage de cycle que DRUP ignore par défaut.

### 2.5 Réglages jamais exposés ou suboptimaux [VÉRIFIÉ dans le code, effet SUSPECTÉ]

1. **`setSink(addr, cycleTime, …)`** (`:785`) : le 2ᵉ argument est documenté « **Sink buffer time** (if zero use default sink buffer time) » (`Sync.hpp:98`), pas le cycle hôte. DRUP y passe le cycle auto → le tampon cible vaut 14,4 ms en CD mais **3,3 ms en 192/24 et 1,35 ms en DSD256**, c'est-à-dire la marge la plus faible aux débits les plus élevés, et `--cycle-time` modifie les deux à la fois. Essai évident : `ACQUA::Clock::zero()` (défaut du sink) ou une valeur fixe (10–20 ms), puis lire `getLatency()`/`SinkInfo.latencyBuffer` (`Sync.hpp:223-228`).
2. **`connect(0)`** (`:820`) : le paramètre est « CPU number occupied by the send thread (default -1 not set) » (`Sync.hpp:147-150`). DRUP demande donc au SDK d'occuper **le CPU 0 — le cœur de ménage qui reçoit toutes les IRQ**. Si le SDK applique cette affinité dans `syncWorker()` (plausible : le CHANGELOG :239 montre que le SDK gère lui-même de l'affinité), le worker peut être ramené sur le cœur 0 après l'épinglage DRUP. Correctif minimal : `connect(sdkCpuMain)` (ou `-1`). **À vérifier immédiatement avec `ps -T -o tid,psr,rtprio,comm -p $(pidof DirettaRendererUPnP)` pendant la lecture.**
3. `infoCycle` 100 ms : paquet de contrôle à 10 Hz interleavé dans le flux, jamais testé à 500 ms–1 s.
4. Bits `FEEDBACK` (fenêtre de moyenne), `LIMITRESEND`, `NOFIREWALL`, `NOSHORTSLEEP`/`NOSLEEPFORCE` : accessibles seulement via `THREAD_MODE=<entier>`, aucune valeur recommandée, aucune télémétrie pour juger.
5. `rngOther=0`, `cpuOther=1` : les threads SDK secondaires (réception feedback, statut — `Sync.hpp:282-283` `mtx/cv`) tournent sur le cœur 1 ; toute remontée vers le worker traverse un cœur (IPI). L'alternative `cpuOther=2` (« not exclusively ») n'a jamais été essayée.
6. Aucun log de `getCycleTime()/getMinCycleTime()/getCycleSize()/getCyclePackets()/getMode()/getLatency()` après `applyTransferMode()`/`connectWait()` : DRUP ne sait pas ce que le SDK a réellement choisi.
7. MTU 3824 : allonge le cycle ×2,55 par rapport à 1500. Ce n'est ni bien ni mal a priori (moins de réveils vs. paquets plus espacés) mais c'est un axe d'A/B jamais formalisé.
8. `src/sync/DirettaSync.{h,cpp}` est une copie morte non compilée (`Makefile:481-486`) — à ne pas confondre.

---

## 3. Constatations sur le hot path (par impact décroissant)

### 3.1 Priorité et affinité réellement en vigueur — **élevé**, [VÉRIFIÉ pour les scripts, SUSPECTÉ pour le SDK]
- Le drop-in du tuner met **tout le processus** en `CPUSchedulingPolicy=fifo` / `CPUSchedulingPriority=90` (`diretta-renderer-tuner-nosmt.sh:373-374`). Tous les threads (main, UPnP, libupnp, position, drain de logs, préchargement, threads SDK) héritent FIFO 90 ; le worker se met ensuite à FIFO **80** (`:1970`) → **le thread audio est le moins prioritaire du processus**. Sans conséquence directe tant que les cœurs sont disjoints, mais c'est l'inverse de la règle RT, et le `nice -20` devient sans objet pour ces threads.
- Trois acteurs peuvent modifier priorité/affinité du worker après DRUP : `CRITICAL`, `OCCUPIED(cpuMain=2)`, `connect(0)`. Seule une observation `ps -eLo` / `chrt -p` tranche.
- Correctif minimal : retirer `CPUSchedulingPolicy/Priority` du drop-in (laisser DRUP poser FIFO par thread), ou fixer le drop-in à une priorité < `RT_PRIORITY` ; passer `connect(sdkCpuMain)` ; logger `sched_getscheduler`/`sched_getaffinity` du worker après le premier `syncWorker()`.

### 3.2 Partage de lignes de cache entre worker et producteur — **moyen/faible**, [VÉRIFIÉ adjacence]
- `m_workerActive` (2 stores seq_cst/cycle) cohabite avec `m_stopRequested/m_draining` lus par le producteur à chaque push (`DirettaSync.h:616-619`, `DirettaSync.cpp:1500-1501,1692,1946`).
- `m_ringUsers` (RMW des deux côtés) cohabite avec `m_onlineTimeoutOccurred` lu par le producteur (`:633-636`).
- Correctif minimal : regrouper l'état chaud du consommateur dans un bloc `alignas(64)` (accumulateur, cache consommateur, `m_streamCount`, `m_workerActive`), placer `m_ringUsers` et `m_reconfiguring` sur leur propre ligne ; `m_workerActive.store(true, relaxed)` / `store(false, release)` au lieu de l'affectation seq_cst.

### 3.3 `LOG_WARN` sur underrun / fin de rebuffering — **moyen**, [VÉRIFIÉ]
`DirettaSync.cpp:1891,1907` → `TimestampedStreambuf` (allocation + `localtime`) → `write(2)` bloquant vers journald depuis le thread FIFO 80, au moment précis où le flux est déjà en difficulté. Correctif minimal : lever un flag atomique consommé par le thread position (qui tourne à 1 s) ou pousser dans `LogRing` (le créer même hors verbose, il ne coûte que 256 Ko).

### 3.4 Opérations atomiques superflues par cycle — **faible**, [VÉRIFIÉ]
- `m_streamCount.fetch_add` (`:1867`) : RMW verrouillé pour une statistique ; remplacer par un compteur privé au worker publié en relaxed toutes les N itérations, ou incrémenter seulement en verbose.
- `RingAccessGuard` : 2 RMW par cycle pour protéger contre un `resize()` qui n'arrive qu'à l'ouverture de piste, où le worker est de toute façon arrêté/joint (`fullReset`, `joinWorkerWithTimeout`). Le rapport TIMING_VARIANCE (R3) le mentionnait déjà comme « deferred ». Alternative : génération de reconfiguration lue en acquire (1 load) + le producteur/consommateur vérifient `m_reconfiguring` seulement.
- `try_lock/unlock + notify_one` (`:1941-1944`) : 3 atomiques par cycle pour un waiter qui n'existe qu'en DSD **et** ring plein — condition pratiquement impossible avec la régulation à 50 % (§4). Correctif minimal : `if (m_producerWaiting.load(relaxed)) notify_one();`, flag posé/levé par `waitForSpace`.
- Sur x86 les `acquire`/`release` sont gratuits ; seuls les seq_cst de §3.2 coûtent une barrière. L'item C3 du rapport « HOT PATH » (« reduce memory order strength ») n'a rien à gagner hormis ces deux stores.

### 3.5 Copie et prefetch — **faible**, [VÉRIFIÉ]
- `pop()` → `memcpy_audio` → ≤256 o : `memcpy_tiny` ; >256 o : `memcpy_audio_fast` (`FastMemcpy_Audio.h:96-171`, 4 prefetchs initiaux, AVX2 sans stores non temporels — bon, le SDK relit `dest` juste après). La taille est constante par piste → branches prédites.
- Aucun prefetch de la *prochaine* zone du ring dans `pop()` (`prefetch_audio_buffer` ne sert que côté push). Les lignes ont été écrites par le cœur 3 jusqu'à 0,5 s plus tôt (L3 partagé 6 Mo, en concurrence avec FFmpeg + 3×64 Ko de staging). Ajouter `_mm_prefetch(buffer+((rp+len)&mask)+{0,64,128,192})` en fin de `pop()` déplace le miss hors du chemin critique.
- 44,1/16 → 24 bit passe par `convert16To24` **scalaire** (`DirettaRingBuffer.h:729-738`) côté producteur : sans enjeu pour le worker, mais c'est la voie CD la plus courante et elle n'est pas vectorisée (10 ms de CPU par seconde d'audio environ — cœur 3).

### 3.6 Ce qui a été fait et ce qui reste, vis-à-vis des rapports de janvier
- TIMING_VARIANCE : R1/R2 (compteur de génération producteur) ✔ (`:1516-1527`) ; N1 direct write ✔ ; « getNewStream() generation counter » ✔ (C1, `:1694-1709`) ; **R3 RingAccessGuard : non fait** ; N4 (memcpy) inchangé.
- HOT PATH : C0/C1/C4/C7 ✔ ; **C6 (aucune I/O sur underrun) : régressé** (`LOG_WARN` à `:1891,1907`) ; C2 ✔ ; C3 partiellement pertinent (seq_cst de `m_workerActive`) ; C5 sans objet.
- Ni l'un ni l'autre ne traitent la couche SDK (mode d'attente, profil, sink buffer time, feedback), qui domine pourtant la régularité des paquets.

---

## 4. Couplage producteur / consommateur

### 4.1 Mécanique [VÉRIFIÉ]
- Thread audio (`DirettaRenderer.cpp:890-1003`) : épinglé `--cpu-decode` + FIFO 80 (`:896-899`). Boucle : `getBufferLevel()` ; si > 50 % → `sleep_for(10 ms)` ; sinon `process(samplesPerCall)` (+ un second `process` si < 25 %) ; si le décodeur n'a rien → `sleep 5 ms`. Seuils `:907-908`.
- `samplesPerCall` : PCM **2048 trames** (`:938`) ; DSD ≈ 12 ms (`DirettaSync.h:266-286`, borné 8192–131072 échantillons).
- Le callback (`:367-523`) pousse par `sendAudio()` ; PCM : boucle avec micro-sommeil 500 µs si `sendAudio` renvoie 0 (`:500-518`, max 40 essais) ; DSD : attente sur `m_spaceAvailable` 500 µs (`:473-482`, 20 essais). Une réponse partielle en DSD n'est pas rebouclée (`:473-486`) — sans effet en pratique vu la marge de ring, mais à noter.
- `AudioEngine::process()` prend `m_mutex`, décode (`readSamples`, allocations `AudioBuffer::resize` par `delete[]/new` seulement à la croissance, `AudioEngine.cpp:67-73,1319-1322`) et appelle le callback (`:2231-2271`).

### 4.2 Le décodeur se réveille-t-il à chaque cycle SDK ? **Non** [VÉRIFIÉ]
Le producteur est purement **piloté par timer** (10 ms) et par le niveau du ring ; le `notify_one()` du worker ne sert qu'au chemin d'attente DSD, jamais atteint en régime établi. Il n'y a donc pas de couplage 1:1, mais le producteur touche à chaque réveil `m_ringUsers` (RMW) et `writePos_/readPos_` — une interaction de ligne de cache toutes les 10 ms.

### 4.3 Quantification (constantes du code)

| Format | Audio par `process()` | Pushes/s | Réveils timer/s (10 ms) | Appels `getNewStream`/s | Octets par push (sortie ring) |
|---|---|---|---|---|---|
| 44,1/16 (→24) | 46,4 ms | ~22 | ~100 | 1000 | 12 288 |
| 96/24 | 21,3 ms | ~47 | ~100 | 1000 | 12 288 |
| 192/24 | 10,7 ms | ~94 | ~100 (quasi chaque réveil pousse) | 1000 | 12 288 |
| 384/24 | 5,3 ms | ~188 (rafales sans sommeil tant que < 50 %) | <100 | 1000 | 12 288 |
| DSD64 | 12 ms | ~83 | ~100 | 1000 | 8 448 |
| DSD256 | 11,6 ms (borne 131072) | ~86 | ~100 | 1000 | 32 768 |

Le ring est réglé pour osciller autour de 50 % (≈0,5 s en CD, ≈0,45 s en 192/24, ≈0,75 s en DSD64) : marge énorme, aucune pression sur le worker.

### 4.4 Pousser plus gros et plus rarement ? [ANALYSE]
- Le cœur 3 est isolé et dédié : ses réveils ne perturbent le cœur 2 que par (a) les transferts de lignes de §3.2, (b) la bande passante L3/mémoire pendant les rafales de décodage, (c) les IPI de shootdown TLB si le décodage fait des `munmap` (§5.4). (b) et (c) sont proportionnels au **volume**, pas au nombre de pushes ; (a) au nombre.
- Passer de 2048 à 8192 trames par `process()` et le seuil d'attente de 10 ms à 40–50 ms diviserait par 4 les réveils (100 → 25/s) et les touches de lignes partagées, sans risque avec 0,5 s de ring (préremplissage 80 ms inchangé). Gain attendu faible mais gratuit ; à faire après §3.2 sinon l'effet est invisible.
- Ne pas descendre en dessous : la « catch-up » à 25 % et le préremplissage à 80 ms restent dimensionnés pour 2048.

---

## 5. Réglages processus / OS vs. bonnes pratiques PREEMPT_RT

### 5.1 Déjà en place [VÉRIFIÉ scripts + notes de déploiement]
`nosmt isolcpus=1-3 nohz=on nohz_full=1-3 rcu_nocbs=1-3 irqaffinity=0` (`tuner-nosmt.sh:211`), slice `AllowedCPUs=1-3` réconciliée à chaque démarrage (`start-renderer.sh:301-336`), toutes les IRQ non gérées → CPU 0 (`tuner:400-424`), gouverneur performance (`:452-469`), `LimitMEMLOCK=infinity` + `CAP_IPC_LOCK` (`service:27-34`), `LimitRTPRIO=99`, `mlockall(MCL_CURRENT|MCL_FUTURE)` avant création des threads (`main.cpp:553-558` — la pile de chaque thread créé ensuite est donc pré-peuplée : pas de prefault explicite nécessaire), FIFO par thread pour worker et décodeur, `ionice` RT, `tuned latency-performance` (= `cpu_dma_latency` → C1), swap off, `/var` en tmpfs, offloads NIC off, `MINIMAL_UPNP`.

### 5.2 Manques et écarts, par impact décroissant

1. **Priorités inversées par le drop-in** (FIFO 90 processus > FIFO 80 worker) — voir §3.1. [VÉRIFIÉ]
2. **`connect(0)`** — voir §2.5.2. [VÉRIFIÉ code / SUSPECTÉ effet]
3. **IRQ gérées non couvertes** : `irqaffinity=` et l'écriture de `smp_affinity_list` n'affectent pas les vecteurs MSI-X « managed » (NVMe, certaines files NIC) — le script les compte d'ailleurs comme « skipped » (`start-renderer.sh:80-105`). Ils peuvent tomber sur les cœurs 1–3. Ajouter `isolcpus=managed_irq,domain,1-3` et vérifier `/proc/interrupts` (lignes `nvme*`, `eth-*`) colonne CPU2. [VÉRIFIÉ sémantique noyau]
4. **IPI de shootdown TLB vers le cœur 2** : tout `munmap`/`brk` négatif/`madvise(DONTNEED)` d'un autre thread du processus envoie un IPI à chaque CPU exécutant un thread du même `mm` — le worker sur le cœur 2 inclus. Sources : `free()` glibc de blocs > `M_MMAP_THRESHOLD` (128 Ko dynamique → paquets/trames FFmpeg, tampons AVIO, `std::string` XML libupnp), `trim` du heap, `AudioBuffer::resize`, `m_ringBuffer.resize` à chaque piste. Aucun `mallopt` dans le code, aucune variable `MALLOC_*` dans le service. Correctif : `mallopt(M_MMAP_THRESHOLD, 1<<30); mallopt(M_TRIM_THRESHOLD, -1); mallopt(M_TOP_PAD, 64<<20)` juste avant `mlockall`, ou `Environment=MALLOC_MMAP_THRESHOLD_=… MALLOC_TRIM_THRESHOLD_=… MALLOC_ARENA_MAX=2` dans l'unité. Mesure : `/proc/interrupts` ligne `TLB` colonne CPU2 avant/après 10 min de lecture. [VÉRIFIÉ mécanisme / SUSPECTÉ ampleur]
5. **Throttling RT / fair server** : rien dans les scripts (`grep sched_rt` vide). Sans effet tant que le worker dort ; **obligatoire** avant tout essai `NOSHORTSLEEP`/`NOSLEEPFORCE`. Sur le noyau 7.2 vanilla RT, le throttling global est remplacé par le *fair deadline server* (préemption du FIFO seulement si une tâche CFS est prête sur le même cœur) : mettre `/sys/kernel/debug/sched/fair_server/cpu{2,3}/runtime` à 0 ou garder les cœurs vides ; sur noyaux plus anciens `kernel.sched_rt_runtime_us=-1`.
6. **Feedback cible reçu sur le cœur 0** : IRQ `eth-diretta` sur CPU0 → thread IRQ FIFO 50 en concurrence avec tout le ménage → réveil du worker par IPI. Deux essais à faible coût : `chrt -f -p 85 $(pgrep -f 'irq/.*eth-diretta')` (le feedback passe avant le reste du cœur 0) ; `IRQ_CPUS=1` ou `=2` pour supprimer le saut de cœur (2 = compromis isolation/latence, à mesurer).
7. **Horloge** : vérifier `current_clocksource=tsc` ; `AcquaClockNow` est probablement `clock_gettime` (vDSO uniquement si TSC).
8. **NIC eth-diretta** : EEE (`ethtool --show-eee`, à désactiver : réveils de lien de plusieurs µs), coalescence (`ethtool -c`, `rx-usecs 0` pour le feedback), qdisc (`tc qdisc replace dev eth-diretta root pfifo_fast` ou `noqueue` : le raw socket passe encore par le qdisc `fq_codel` par défaut), ASPM PCIe (`pcie_aspm=off`), pilote (r8169 vs igb/igc : latences très différentes).
9. **Cmdline** : le script d'install recommande `skew_tick=1 nosoftlockup` (`Install Fedora…md:229`) mais la cmdline notée pour ce PC ne les contient pas. Ajouter `skew_tick=1 nosoftlockup nowatchdog nmi_watchdog=0 rcu_nocb_poll` ; `default_hugepagesz=1G` du même script est inutile sans `hugepages=N`.
10. **THP** : `transparent_hugepage=never` (ou `madvise` + `khugepaged/defrag=0`) pour éviter compaction/migration et leurs shootdowns ; les hugepages n'apportent rien ici (empreinte du worker < 10 Ko par cycle).
11. **seccomp** : `SystemCallFilter=` (`service:63`) ajoute un filtre BPF à **chaque** syscall du processus, dont les `send*`/`select` du worker (~100–200 ns, variable). Sur une machine dédiée, le retirer est un gain net et mesurable.
12. **Timer slack** : nul automatiquement pour les threads SCHED_FIFO (noyau) ; rien à faire tant que worker et décodeur sont FIFO.
13. **Mémoire verrouillée** : avec `MCL_FUTURE`, chaque `std::thread` verrouille sa pile de 8 Mo (RLIMIT_STACK) → ~150–200 Mo pour une quinzaine de threads (libupnp, préchargement, SDK). Sans effet temporel ; `LimitSTACK=2M` dans l'unité si la RAM compte.
14. **Threads UPnP/position sur le cœur 1** : SSDP, GENA (coupé par `MINIMAL_UPNP`), position à 1 s (`DirettaRenderer.cpp:1005-1013`), drain de logs à 10 ms uniquement en verbose (`main.cpp:181`). Correct.

---

## 6. Plan d'action priorisé

**P0 — mesurer avant de toucher (une soirée)**
1. `tcpdump -i eth-diretta -ttt -n ether proto 0x… or udp` pendant 60 s à 44,1 et 192 : histogramme des inter-arrivées et tailles → tranche entre « 1 paquet de 264 o par ms » et « 1 paquet de 3,8 Ko toutes les 14,4 ms », et donne la vraie référence de gigue.
2. `ps -T -o tid,psr,cls,rtprio,comm -p $(pidof DirettaRendererUPnP)` pendant la lecture → priorité/cœur réels du worker (§3.1, §2.5.2).
3. `/proc/interrupts` (lignes TLB, nvme, eth) colonne CPU2, deltas sur 10 min.
4. `cyclictest -t1 -a2 -p80 -i1000 -m` 5 min pendant la lecture (le plancher du cœur 2 indépendamment de DRUP).
5. Ajouter (patch trivial) le log de `getCycleTime/getMinCycleTime/getCycleSize/getCyclePackets/getMode/getLatency` après `applyTransferMode()` et `connectWait()`.

**P1 — correctifs sans risque**
- `connect(sdkCpuMain)` au lieu de `connect(0)` (`DirettaSync.cpp:820`).
- Retirer `CPUSchedulingPolicy/Priority` du drop-in (ou priorité < 80).
- `LOG_WARN` de `getNewStream` → flag atomique consommé hors RT.
- `mallopt` avant `mlockall` ; `transparent_hugepage=never` ; `isolcpus=managed_irq,domain,1-3` ; `skew_tick=1 nosoftlockup`.
- Retirer `SystemCallFilter=` de l'unité sur la machine dédiée.

**P2 — expérimentations A/B guidées par les mesures P0**
- `setSink(…, Clock::zero(), …)` vs valeur fixe (10–20 ms) — le « sink buffer time ».
- `THREAD_MODE=19` (CRITICAL+NOSHORTSLEEP+OCCUPIED) puis `2065` (+NOSLEEPFORCE) avec §5.2.5 en place ; surveiller température (fanless).
- `--mtu 1500` vs 3824 ; `--cycle-time 2000` ; `--transfer-mode fixauto` ; `--target-profile-limit 1000`.
- `INFO_CYCLE=500000` ; `THREAD_MODE` + `NOFIREWALL` (16384) ; bits `FEEDBACK` 32×{1,2,4}.
- IRQ eth-diretta : priorité 85 ; affinité 1 puis 2.

**P3 — micro-optimisations du hot path (gain faible, à faire ensemble)**
- Bloc `alignas(64)` pour l'état chaud du consommateur ; `m_workerActive` en relaxed/release ; `m_streamCount` privé ; `notify_one` conditionnel ; prefetch de la prochaine zone dans `pop()` ; remplacer `RingAccessGuard` par une génération ; 8192 trames par `process()` + attente 40 ms ; vectoriser `convert16To24`.

---

## 7. Fichiers cités (chemins absolus)

- `le clone v2.5.15\src\DirettaSync.cpp` (getNewStream 1686-1948 ; startSyncWorker 1950-2002 ; open 474-865 ; applyTransferMode 2082-2152 ; configureRingPCM/DSD 1226-1398 ; sendAudio 1499-1630)
- `le clone v2.5.15\src\DirettaSync.h` (cycle 293-313 ; config 325-350 ; membres 579-721)
- `le clone v2.5.15\src\DirettaRingBuffer.h` (pop 1347-1363 ; membres 1462-1474 ; convert16To24 729-738)
- `le clone v2.5.15\src\memcpyfast_audio.h`, `src\FastMemcpy_Audio.h`
- `le clone v2.5.15\src\main.cpp` (mlockall 553-558 ; log drain 169-188)
- `le clone v2.5.15\src\DirettaRenderer.cpp` (callback 367-523 ; audioThreadFunc 890-1003 ; config SDK 226-254)
- `le clone v2.5.15\src\LogLevel.h`, `src\TimestampedLogger.h`
- `le clone v2.5.15\diretta-renderer-tuner-nosmt.sh` (211, 242-257, 367-392, 400-424)
- `le clone v2.5.15\systemd\diretta-renderer.service`, `systemd\start-renderer.sh`
- `<SDK 150>\Host\Sync.hpp` (THRED_MODE 21-51 ; open 76-87 ; setSink 96-101 ; transfert 104-132 ; connect 147-150 ; getNewStream 252-255)
- `<SDK 150>\Host\Profile.hpp`, `Host\Connection.hpp`, `Host\ACQUA\Ethernet.hpp` (RCV_MODE 86-97), `Host\ACQUA\ThreadPriority.hpp`, `Host\ACQUA\Clock.hpp`
- `le clone v2.5.15\docs\2026-01-17-1407-TIMING_VARIANCE_OPTIMIZATION_REPORT.md`, `docs\2026-01-17-1020-HOT PATH SIMPLIFICATION REPORT.md`
