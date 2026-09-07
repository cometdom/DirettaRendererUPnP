# DirettaRendererUPnP 2.5.15 — Synthèse : ce qui peut encore améliorer la qualité sonore, et ce que le SDK 150 permet

*Analyse réalisée le 7 septembre 2026 par Claude (Fable 5.1) à la demande de herisson-88, sur le dépôt `cometdom/DirettaRendererUPnP` à `v2.5.15` (`80ede03`) et les SDK Diretta Host 149_8 / 150_4. Quatre audits de lecture intégrale (14 100 lignes) l'alimentent, archivés à côté : `01-chemin-donnees.md`, `02-chemin-temporel.md`, `03-activite-hote.md`, `04-sdk150-opportunites.md`. Chaque affirmation y est marquée **vérifiée** (lue dans le code ou les en-têtes) ou **suspectée** (déduite, à confirmer par mesure ou auprès de Yu Harada). Limite assumée : la bibliothèque statique du SDK n'a pas été désassemblée ; ce qui se passe à l'intérieur de `syncWorker()` est déduit des en-têtes.*

*Contexte d'écoute : mini-PC fanless i5-8260U sur iFi Power Elite, Fedora 44 PREEMPT_RT vanilla, `nosmt isolcpus=1-3`, DRUP épinglé `--cpu-audio 2 --cpu-decode 3 --cpu-other 1`, FIFO 80, MTU 3824, Holo Audio Red (target 148), JPLAY iOS.*

---

## 1. En une page

**Le chemin des données est bit-parfait par construction** — pas de gain, pas de rééchantillonnage, pas de dither, un seul point de choix de conteneur (`configureSinkPCM`). Ce n'est pas là que se joue la qualité sonore de ce renderer. Elle se joue sur **quand** les paquets partent et sur **ce que fait l'hôte autour** — et sur ces deux plans, l'analyse trouve à la fois des écarts avec la documentation du SDK, des bugs latents, et des leviers jamais actionnés.

Les cinq points qui pèsent le plus, dans l'ordre où je les traiterais :

1. **`connect(0)` demande au SDK d'occuper le cœur 0** — le cœur de ménage qui reçoit toutes les IRQ — alors que tout le reste est calé sur le cœur 2. La doc 150 rend enfin le paramètre explicite (« CPU number occupied by the send thread, default −1 »). Une ligne. À vérifier d'abord par `ps -T` pendant la lecture : si le thread d'envoi est bien sur le cœur 0, c'est le plus gros gain disponible pour zéro effort.
2. **`setSink(addr, cycleTime, …)` passe le temps de cycle là où le SDK attend le « Sink buffer time »** (tampon demandé à la cible ; l'échantillon de Yu passe 100 ms). Conséquence : la marge côté cible est de 14 ms en CD mais **3,3 ms en 192/24 et 1,35 ms en DSD256** — la plus faible aux débits où on en a le plus besoin, et `--cycle-time` change les deux à la fois sans le dire.
3. **Le tuner met tout le processus en FIFO 90, le thread audio se rétrograde à FIFO 80** : le thread le plus critique est le moins prioritaire du processus (SOAP, SSDP, préchargement, threads SDK passent avant). Et le préchargement HTTP de la piste suivante tourne **en FIFO 80 sur le cœur de décodage**, avec pour les URL Qobuz/Tidal via proxy une rafale de **5 Mo** (`probesize` par défaut) au début de chaque piste.
4. **Le mode d'attente du SDK est le défaut « sommeil »** : le thread d'envoi dort entre deux paquets et se réveille par timer (latence timer + sortie de C1 + ordonnanceur à chaque paquet). Les bits `NOSHORTSLEEP` / `NOSLEEPFORCE` (busy-wait sur un cœur isolé) et `FEEDBACKOFFSET` (fenêtre de lissage de l'asservissement hôte↔cible) sont les leviers les plus directs sur la régularité d'émission — jamais exposés autrement qu'en bitmask brut, jamais documentés, jamais mesurés. Sur un fanless, le busy-wait a un coût thermique ; il faut le régler avec le fair server RT.
5. **Trois bugs latents à effet audible** : l'indice d'alignement S24 est **effacé à la reprise après pause** et le défaut est LSB (bruit blanc pleine échelle si la reprise tombe sur un passage doux — masqué sur ARM par un `#if`, exposé sur x86) ; un `LOG_WARN` avec allocation et `write(2)` vers journald **depuis le thread FIFO 80** exactement au moment d'un underrun ; et un ordre de bits DoP identique pour DSF et DFF (un DSF en DoP part probablement inversé bit à bit — le DAC « fonctionne » avec le bruit de mise en forme replié).

Ce que le **SDK 150** apporte de neuf est petit mais ciblé : `connect(cpu, rapidStart)`, `configTransferSizeFix(bytes, noRemainder)` (paquets tous de même taille, la cadence la plus régulière possible), `statusUpdate(REQ_STATE)` (inaccessible : privé), et `ClockDiff` qui permet enfin de **mesurer** la gigue d'appel côté hôte. Le reste des opportunités existait déjà en 149 et n'a jamais été pris.

Enfin : DRUP **ne sait pas ce que le SDK a réellement négocié** (`getCycleTime`, `getCycleSize`, `getLatency`, `is_MSmode`, `Info.latencyMax/reqMTU` ne sont jamais lus). Avant tout réglage, la première chose à faire est de le journaliser — c'est le point P0 du protocole (§6).

---

## 2. Écarts avec la documentation du SDK

| # | Écart | Où | Statut | Correctif |
|---|---|---|---|---|
| E1 | `setSink` 2ᵉ argument = **Sink buffer time**, DRUP passe le cycle | `DirettaSync.cpp:785` ; `Sync.hpp:98` (« if zero use default sink buffer time ») ; `SinHost.cpp` passe `Clock::MilliSeconds(100)` | vérifié | `Clock::zero()` (défaut du sink) ou nouveau `--sink-buffer-ms` (10–100), puis lire `getLatency()` / `Info.latencyBuffer` |
| E2 | `connect(0)` = CPU 0 « occupé par le thread d'envoi » | `DirettaSync.cpp:820` ; `Sync.hpp:147-150` | vérifié (code) / suspecté (effet) | `connect(sdkCpuMain)` ou `connect(-1)` ; vérifier `ps -T -o tid,psr,cls,rtprio,comm` |
| E3 | `configTransferFixAuto/VarAuto` documentés « Called when Target Profile (Auto) is active », utilisés dans le chemin **Self**Profile ; `inquiryParameter()` (profil de la cible) jamais appelé ; `configTransferAuto(min, target, max)` — le mode de l'auteur (`200 µs, 0, 100 ms`) — jamais utilisé | `DirettaSync.cpp:2082-2152` ; `Sync.hpp:110-132, 187-188` | vérifié | Ajouter `--transfer-mode auto-sdk` ; appeler `inquiryParameter` avant `getProfileMaker` |
| E4 | `statusUpdate() override {}` (corrigé en 2.5.15) ; `setSinkConfigure` avant `setSink` (corrigé en 2.5.15) | — | corrigé | Garder l'appel de base |
| E5 | `is_MSmode()` public jamais lu (DRUP « devine » le mode) ; `getCycleTime/MinCycleTime/CycleSize/CyclePackets/Mode/Latency` jamais journalisés ; `Info.{latencyBuffer,latencyMax,latencyHw,maxSize,reqMTU,maxMTU}` jamais lus | `DirettaSync.cpp:438-467` | vérifié | Journaliser après `applyTransferMode()` et `connectWait()` (ce que `SinHost` affiche l.172-178) |
| E6 | `SOCKETNOBLOCK=8` listé par DRUP mais **commenté dans le SDK** (no-op) ; « base 2620 µs » de `CONFIGURATION.md` obsolète ; `--cycle-time` documenté 333–10 000 µs alors que l'auto produit 14 441 µs en CD | `docs/CONFIGURATION.md:113,135` ; `main.cpp:276,393` | vérifié | Documentation |
| E7 | `src/sync/DirettaSync.{cpp,h}` : copie morte non compilée | `Makefile:481-486` | vérifié | Supprimer (risque de confusion) |

## 3. Bugs à effet potentiel sur le son

| # | Bug | Gravité | Où | Correctif |
|---|---|---|---|---|
| B1 | **Alignement S24 : défaut LSB jamais correct ici (FFmpeg justifie toujours à gauche), et indice effacé par `clear()` à la reprise après pause sans être reposé** → bruit blanc pleine échelle si la reprise tombe sur > ~0,5 s de passage < −48 dBFS ; masqué sur ARM64 par `#if __aarch64__` (dont le commentaire « x86 produit du LSB » est faux) | bug latent, x86 exposé | `DirettaRingBuffer.h:367-397, 179-189` ; `DirettaSync.cpp:1480` ; `DirettaRenderer.cpp:676-679` | défaut `MsbAligned` (l.380, 390) ; ne plus effacer `m_s24Hint` dans `clear()` ; supprimer le `#if` |
| B2 | **`LOG_WARN` sur underrun / fin de rebuffering depuis le thread FIFO 80** : `stringstream` + `localtime` + `write(2)` journald au moment où le flux souffre déjà (régression de l'item C6 du rapport « HOT PATH ») | moyen | `DirettaSync.cpp:1891, 1907` ; `TimestampedLogger.h:41-74` | flag atomique consommé hors RT, ou `LogRing` hors verbose |
| B3 | **DoP : ordre de bits indépendant du format source** (`dopBitReverse = g_dopMsb`) ; spec DoP = bit le plus ancien en MSB (convention DFF) → un DSF part inversé bit à bit ; le DAC « marche » (somme par octet conservée) avec le bruit de mise en forme replié = plancher de bruit plus haut. Cohérent avec l'existence de `--dop-msb` « try if --dop produces noise » | risque qualité, à confirmer | `DirettaSync.cpp:1547` ; `AudioEngine.cpp:1284-1286` | `dopBitReverse = (dsdFormat == DSF) ^ g_dopMsb` ; test : même DSF en `--dop` / `--dop-msb`, mesurer le souffle |
| B4 | **Pushes partiels à ring plein non alignés sur la trame** (PCM : `free ≡ −1 mod trame`, sur-lecture de quelques octets injectés dans le ring → échange de canaux/bruit jusqu'au prochain `clear()`) ; **DSD : plans L/R décalés** (`srcR` calculé sur la taille tronquée). Latents grâce au throttle 50 % | latent | `DirettaRingBuffer.h:309-313, 480-487, 921-927` ; `DirettaRenderer.cpp:469-486, 504-506` | pushes atomiques (`return 0` si `free < total`) ou arrondi à la trame |
| B5 | Source 24 bits sur sink « 32 sans 24 » ou 16 seulement → copie directe d'octets mal typés (`inputBps 4 > direttaBps 2`, aucun flag) | latent (sinks exotiques) | `DirettaSync.cpp:1052-1087, 762-765` | tenter `FMT_PCM_SIGNED_32` pour du 24 ; refuser toute combinaison sans conversion |
| B6 | `throw std::runtime_error` non rattrapé sur le thread audio (`terminate`) quand aucun format n'est accepté ; `ProtocolInfo` codé en dur (liste Holo) au lieu de dérivé de `getSinkInfo()` → un CP peut envoyer du DSD à un target PCM-only | latent | `DirettaSync.cpp:1087, 1219` ; `UPnPDevice.cpp:56-57` | `return false` ; filtrer `ProtocolInfo` sur `checkSinkSupportDSD()` |
| B7 | Fin de piste : décodeur non vidangé (codecs à délai : MP3/AAC) ; fin de playlist : jusqu'à ~1 % du ring jeté | cosmétique | `AudioEngine.cpp:1374-1376` ; `DirettaRenderer.cpp:575-589` | `avcodec_send_packet(NULL)` + drain |

## 4. L'hôte pendant la lecture : ce qui bouge encore

Le code fait déjà l'essentiel (lecture cadencée par la consommation, hot path sans allocation ni log, position/événements coupés par `--minimal-upnp`). Le résiduel :

| # | Activité | Cadence / effet | Où | Proposition |
|---|---|---|---|---|
| H1 | **Thread de décodage réveillé 100 fois/s** (timer 10 ms) avec un quantum fixe de **2048 trames**, même à l'arrêt ; agrandir le ring ne change pas le quantum | 100 réveils/s sur le cœur 3 ; à 192/24 quasi chaque réveil décode | `DirettaRenderer.cpp:938, 953-963` | hystérésis 30 %→70 %, 8192 trames, sommeil 40–50 ms (ou CV `m_spaceAvailable` sous seuil) : 100 → ~5 réveils/s |
| H2 | **`recv()` TCP de 32 Ko** (tampon avio) → le serveur envoie ~22 trames à pleine vitesse du lien, puis silence ; le « paquebot » n'est pas un flux continu mais une noria de rafales sur le **cœur 3** (`copy_to_user` en RT) | 3,3 rafales/s en CD, 21/s en 192/24 ; 6–7 copies mémoire de bout en bout | `AudioEngine.cpp:308, 1364` | thread de préchargement persistant sur le cœur 1 (SCHED_OTHER explicite), file bornée 4 Mo, lecture 64 Ko, `AVIOContext` custom (le motif existe déjà l.242-282) ; borner `recv_buffer_size` à 64 Ko ; lectures cadencées |
| H3 | **Préchargement de la piste suivante en RT sur le cœur 3** (héritage `PTHREAD_INHERIT_SCHED`), avec **`probesize` 5 Mo** pour toute URL contenant « qobuz »/« tidal » | rafale de 5 Mo + parsing en FIFO 80 au début de chaque piste | `AudioEngine.cpp:145-154, 330-333, 2188-2198` | `probesize=32768` + `max_analyze_duration=0` pour toutes les URL HTTP (15 min) ; créer le thread avec affinité/politique explicites |
| H4 | **Deux threads qui ne font rien mais se réveillent** (main et « UPnP Thread », `sleep_for(1s)`) sur le cœur 1 `nohz_full` | 2 réveils/s | `main.cpp:615-617` ; `DirettaRenderer.cpp:877-888` | attente sur CV / `sigwait` |
| H5 | **`--quiet` n'est pas silencieux** : dizaines de `std::cout` bruts par changement de piste (bannières `OPEN`, `Opening…`, `Next URI queued`…), `printf` hexadécimaux des premiers paquets DSD ; `TimestampedStreambuf::overflow` appelé **caractère par caractère** | ~10–20 lignes/piste → `write(2)` journald, réveil de journald (hors slice) | `AudioEngine.cpp:111, 786, 1165-1170, 2449-2470` ; `TimestampedLogger.h:59-74` | passer en `LOG_INFO/DEBUG` ; zone de mise en tampon |
| H6 | `m_ringUsers` (RingAccessGuard) : RMW verrouillé par **les deux** cœurs 2 et 3 à ~1 kHz, non aligné ; `m_workerActive` (2 stores seq_cst/cycle) sur la même ligne que `m_stopRequested/m_draining` lus par le producteur | ping-pong de lignes de cache 2↔3 | `DirettaSync.h:616-619, 633-636` ; `DirettaSync.cpp:1692, 1766, 1946` | bloc `alignas(64)` pour l'état chaud du consommateur ; remplacer le guard par une génération ; stores relaxed/release |
| H7 | `notify_one` + `try_lock` **par cycle** pour un waiter qui n'existe qu'en DSD à ring plein (jamais en pratique) ; `m_streamCount.fetch_add` par cycle pour une statistique | 3–4 atomiques/cycle inutiles | `DirettaSync.cpp:1867, 1941-1944` | `notify` conditionnel sur un flag `m_producerWaiting` ; compteur privé |
| H8 | **Croissance de mémoire verrouillée** (13,8 Go après 24 h sur le Pi ≈ 160 Ko/s, l'ordre de grandeur du débit audio) ; rien dans le code lu ne retient le flux → suspect : **tampon de retransmission du SDK** non borné (`LIMITRESEND=4096` « Limit the amount of retransmission buffer allocated ») | ~40 fautes de page + zeroing noyau par seconde, en continu | `Sync.hpp:43-44` | test `THREAD_MODE=4097` (CRITICAL+LIMITRESEND) et suivi de `VmLck` sur une heure ; sinon `smaps` |
| H9 | SSDP `ssdp:alive` toutes les ~14,5 min ; SUBSCRIBE/renouvellements GENA toujours acceptés en minimal | faible | `UPnPDevice.cpp:170, 349-442` | max-age 86 400 ; option « ultra-minimal » refusant SUBSCRIBE pour JPLAY/LMS uniquement (casserait Audirvana/Bubble) |
| H10 | `convert16To24` **scalaire** (la voie CD la plus courante) ; `swresample` instancié pour une identité sur tout FLAC (bypass refusé sur la croyance fausse « FLAC = planaire ») ; double copie ; 256 Ko alloués pour rien | charge cœur 3 inutile | `DirettaRingBuffer.h:729-738` ; `AudioEngine.cpp:1738-1828` | vectoriser ; lever la garde `isCompressed` |

## 5. Ce que le SDK 150 (et le 149 inexploité) permet

**Nouveautés 150** (diff des en-têtes ; `memo_host.txt` inchangé ; le Doxygen 150 n'est que la mise en forme des commentaires `///` — il n'existe aucune sémantique documentée au-delà) :

- `Sync::connect(int cpu = −1, bool rapidStart = false)` — le cœur d'envoi explicite (→ E2) et un **Rapid Start** dont la seule doc est « Rapid Start (defalt play mode) ». Sémantique à demander à Yu ; à exposer `--rapid-start`, off par défaut, mesurer le temps `connect → online` et le comportement au premier paquet.
- `ProfileMaker::configTransferSizeFix(size_t bytes, bool noRemainder = false)` — **taille de transfert fixe sans paquet « reste »** : tous les paquets d'un cycle ont la même taille. C'est le mode le plus proche de « paquet constant à cadence constante ». Nécessite le chemin TargetProfile (`inquiryParameter` → `getProfileMaker` → `setConfigTransfer`).
- `statusUpdate(REQ_STATE)` — privé, non surchargeable : le seul point d'accroche reste le `virtual statusUpdate()` qu'il faut chaîner (2.5.15).
- `ACQUA::ClockDiff` — chronomètre delta : `static ClockDiff d; auto dt = d.update();` dans `getNewStream` + min/max/écart-type dans `dumpStats()` = **la première mesure de gigue côté hôte**, coût nul.
- `SyncBuffer::setupBuffer(…, firstCallbackImmediately)` et `setStream() → bool` — classe *push*, DRUP n'en dérive pas.

**Leviers 149 = 150 jamais actionnés** (`THRED_MODE`, `Sync.hpp:21-51`) :

| Bit | Doc SDK | Intérêt ici |
|---|---|---|
| `NOSHORTSLEEP` 2 | « Do not enter Sleep mode for a short period of time (busy loop) » | spin sur la fin d'attente au lieu de `nanosleep` : supprime timer + sortie C1 + ordonnanceur à chaque paquet — **candidat n°1** sur cœur isolé |
| `NOSLEEPFORCE` 2048 | « force busy loop » | 100 % du cœur 2, réveil déterministe ; +5–8 W sur un fanless ; exige le réglage du fair server RT |
| `FEEDBACKOFFSET` 32…224 (3 bits) | « Move the transmission feedback to the moving average » | **fenêtre de lissage de l'asservissement hôte↔cible** — règle la nervosité des corrections de cadence ; 0 = immédiat (défaut) |
| `NOFASTFEEDBACK` 256 | — | désactive la correction rapide |
| `LIMITRESEND` 4096 | « Limit the amount of retransmission buffer allocated » | sur lien direct sans perte : sans inconvénient ; suspect n°1 de la croissance mémoire (H8) |
| `NOFIREWALL` 16384 | « Do not send packets to disable the firewall » | supprime un trafic de contrôle périodique (suspecté) ; lien direct, pare-feu off : sans risque |
| `IDLEONE/IDLEALL` | spin + `sched_yield` | inutile sur cœur dédié |
| `NORAWSOCKET` | désactive le mode raw AF_PACKET (« DDS mode 3 ») | **à ne pas activer** : le raw évite la pile IP/UDP par paquet |

Et : `changeWorkMode(THRED_MODE)` change le mode **à chaud** sans reconnexion (A/B pendant la lecture, ex. via SIGUSR2 ou la web UI) ; `currentWorkMode()` pour journaliser l'état réel ; `rngOther` d'`open()` pour répartir les threads SDK secondaires ; `configTransferRandom`/`TRIANGOLO` (dithering temporel des émissions, non documenté).

**Ce que DRUP fait aujourd'hui** : `THRED_MODE = 1|16` (CRITICAL + OCCUPIED), attente « sommeil », `infoCycle` 100 ms (paquet de contrôle à 10 Hz interleavé), transfert `VAR_MAX` (paquets pleins MTU, période = 3821 / débit : **69 paquets/s en CD, 302/s en 192/24** si le SDK agrège bien les callbacks de 1 ms — non journalisé, donc non su), profil Self. Le jumbo 3824 allonge le cycle ×2,55 par rapport à 1500 — un axe d'A/B jamais formalisé.

## 6. Protocole : mesurer d'abord, régler ensuite

**P0 — une soirée de mesures, sans modifier le son**
1. `ps -T -o tid,psr,cls,rtprio,comm -p $(pidof DirettaRendererUPnP)` pendant la lecture : sur quel cœur et à quelle priorité tourne **réellement** le thread d'envoi (E2, §3 rapport temporel) — tranche `connect(0)` et le FIFO 90 du drop-in.
2. Patch trivial : journaliser `getCycleTime/getMinCycleTime/getCycleSize/getCyclePackets/getMode/getLatency/is_MSmode` et `Info.{latencyBuffer,latencyMax,reqMTU,maxMTU}` après `applyTransferMode()` et `connectWait()` — on saura enfin ce qui est négocié.
3. `tcpdump -i eth-diretta -ttt -n` 60 s en 44,1 et 192 : histogramme des inter-arrivées et des tailles → « 1 paquet de 264 o par ms » ou « 1 paquet de 3,8 Ko toutes les 14,4 ms » ? C'est la référence de gigue avant/après.
4. `/proc/interrupts` (lignes `TLB`, `nvme*`, `eth-*`), colonne CPU2, deltas sur 10 min de lecture : les IPI de shootdown TLB et les IRQ gérées atteignent-elles le cœur audio ?
5. `cyclictest -t1 -a2 -p80 -i1000 -m` 5 min pendant la lecture : le plancher de latence du cœur 2 indépendamment de DRUP.
6. `grep VmLck /proc/<pid>/status` toutes les heures (H8).

**P1 — correctifs sans risque** (une demi-journée) : E2 (`connect(sdkCpuMain)`), drop-in sans `CPUSchedulingPolicy/Priority` (ou < 80), B1 (S24), B2 (`LOG_WARN` hors RT), H3 (`probesize` 32 Ko partout), H4 (threads morts), H5 (`--quiet` réel), `mallopt(M_MMAP_THRESHOLD, 1<<30); mallopt(M_TRIM_THRESHOLD, −1)` avant `mlockall` (supprime les `munmap` → IPI TLB vers le cœur 2), `transparent_hugepage=never`, `isolcpus=managed_irq,domain,1-3`, `skew_tick=1 nosoftlockup nowatchdog`, retirer `SystemCallFilter=` de l'unité (filtre BPF sur chaque `sendto` du worker), vérifier `current_clocksource=tsc`, EEE off et qdisc `noqueue` sur `eth-diretta`.

**P2 — A/B d'écoute, un paramètre à la fois, guidés par P0** : `setSink` buffer time (0 / 20 / 50 / 100 ms — E1) ; `THREAD_MODE=19` (+NOSHORTSLEEP) puis 2065 (+NOSLEEPFORCE) avec le fair server réglé et la température surveillée ; `FEEDBACKOFFSET` 32/64/128 ; `INFO_CYCLE=500000` ; `NOFIREWALL` ; `--mtu 1500` vs 3824 ; `--transfer-mode auto-sdk` (nouveau) et `sizefix` (nouveau, SDK 150) ; `--rapid-start` ; IRQ `eth-diretta` en priorité 85, puis affinité 1 ou 2 ; H1 (hystérésis du décodeur) ; **format A/B** : 24 bits packé vs conteneur S32 pour le hi-res (audio identique, comportement hôte/cible différent), 16 bits natif vs 16→24.

**P3 — refonte utile mais plus lourde** (1–2 jours) : H2, le thread de préchargement persistant + lissage réseau ; H6/H7, l'hygiène du hot path ; H10, vectorisation et bypass FLAC ; B3–B6.

## 7. Questions pour Yu Harada

1. Sémantique exacte de **Rapid Start** (`connect(…, true)`).
2. Signification des deux `uint32_t` d'`inquiryParameter(addr, u32, u32)` (probablement `NoBaseMin` / `FrameMax`).
3. Effet de `FEEDBACKOFFSET` / `NOFASTFEEDBACK` sur la boucle d'asservissement ; plage recommandée.
4. `connect(cpu)` s'applique-t-il au thread créé par un `startSyncWorker()` surchargé, ou à un thread interne ?
5. Ce que fait `TRIANGOLO` ; ce que couvre exactement `MSMODE_AUTO` (« 3 1 0 ») et si MS3 = transport raw.
6. Le tampon de retransmission est-il non borné par défaut (H8) ?
7. `getNewStream` est-il appelé une fois par paquet ou N fois par cycle de profil ?

## 8. Ce qui n'a pas été fait, et pourquoi

- Pas de désassemblage de `libDirettaHost` : tout ce qui concerne l'intérieur de `syncWorker()` (attente, feedback, retransmission) est **suspecté**, et c'est précisément pour cela que le protocole commence par des mesures.
- Pas d'écoute : ce document propose des A/B, il ne prétend pas savoir ce qui sonne mieux. Les seuls points présentés comme *corrections* (et non comme préférences) sont E1–E3 et B1–B4.
- Ces rapports sont archivés dans `docs/analysis/2026-09-sq-review/` ; les correctifs qui en découlent sont dans la branche `pm/sq-improvements`.
