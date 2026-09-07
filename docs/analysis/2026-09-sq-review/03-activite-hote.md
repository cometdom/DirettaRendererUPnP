# Audit DirettaRendererUPnP 2.5.15 — activité de l'hôte pendant la lecture

*Audit réalisé le 2026-09-07 (agent d'analyse, rapport brut archivé sans retouche).*

Périmètre : arbre `le clone v2.5.15` (lecture seule), en-têtes SDK `<SDK 150>\Host`. Configuration de référence = celle de l'hôte de référence : `--cpu-audio 2 --cpu-decode 3 --cpu-other 1 --rt-priority 80 --quiet --minimal-upnp`, build `NOLOG=1`, tuner `nosmt` appliqué (`isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3 irqaffinity=0`, slice `AllowedCPUs=1-3`), `IRQ_CPUS=0`, sources LMS/slim2UPnP et JPLAY iOS.

Légende : **[V]** vérifié dans le code ; **[S]** suspecté / déduit (à confirmer par mesure) ; **[E]** externe au dépôt (libupnp, FFmpeg, SDK), non vérifiable ici.

---

## 1. Inventaire des threads et des activités périodiques

### 1.1 Threads du processus pendant qu'une piste joue

| # | Thread (origine) | Cœur (config ci-dessus) | Ordonnancement | Cadence de réveil / cause | Statut |
|---|---|---|---|---|---|
| 1 | **main** (`main.cpp:428`) | 1 (`main.cpp:561-564`) | hérité du service (voir §1.3 : FIFO 90 si drop-in tuner) | **1 Hz** : `while (isRunning()) sleep_for(1s)` (`main.cpp:615-617`). Ne fait rien. | [V] |
| 2 | **« UPnP Thread »** (`DirettaRenderer::upnpThreadFunc`, `DirettaRenderer.cpp:877-888`) | 1 | hérité | **1 Hz** : `while (m_running) sleep_for(1s)`. **Corps vide** — thread inutile. | [V] |
| 3 | **« Audio Thread »** = décodage (`audioThreadFunc`, `DirettaRenderer.cpp:890-1003`) | 3 (`:896-899`) | SCHED_FIFO `g_rtPriority` (80) | **100 Hz** en lecture : si ring > 50 % → `sleep_for(10ms)` (`:958-960`) ; sinon `process(2048)` immédiatement (`:963`) puis re-test sans dormir. Quantum de remplissage fixe = **2048 échantillons** (`:938`), quelle que soit la taille du ring. À l'arrêt : idem 100 Hz (`:997`). | [V] |
| 4 | **Position Thread** (`positionThreadFunc`, `:1005-1054`) | 1 | hérité | 1 Hz ; 3 prises de `m_stateMutex` par tour. **Absent avec `--minimal-upnp`** (`:821-825`). | [V] |
| 5 | **Worker Diretta** (lambda `DirettaSync::startSyncWorker`, `DirettaSync.cpp:1967-1999`) | 2 (`:1975-1992`) | SCHED_FIFO 80 (`:1970`) | Boucle `syncWorker()` du SDK ; cadence = cycle Diretta (cf. §2.4). Si `syncWorker()` rend `false` : sleep 100 µs (`:1996`). Chaque `getNewStream()` = memset ou `pop` de `bytesPerBuffer` (264 o à 44,1 kHz/24 bit, « 1 ms », `:1279-1293`). | [V] boucle ; [E] sémantique bloquante de `syncWorker()` |
| 6 | **Thread(s) interne(s) du SDK** | hint `cpuOther` = 1er cœur de `--cpu-other` → 1, hint `cpuMain` → 2, flag `OCCUPIED` ajouté (`DirettaSync.cpp:191-210`) | interne SDK | Les en-têtes ne montrent qu'un `std::thread thread_sync_node` (`Sync.hpp:281`) et `thread_buffer_node` dans `SyncBuffer` (non utilisé par DRUP). Comme DRUP remplace `startSyncWorker()`, il est probable que le SDK ne crée pas de thread d'envoi propre ; l'existence d'un thread « other » (réception des retours cible, info packets à `infoCycle` = 100 ms, `DirettaSync.h:332`) n'est pas vérifiable sans les sources. **À confirmer avec `ps -T -o tid,psr,cls,rtprio,comm -p $(pidof DirettaRendererUPnP)`.** | [E]/[S] |
| 7 | **Threads libupnp** (créés dans `UpnpInit2`, `UPnPDevice.cpp:90`, appelé depuis main déjà épinglé) | 1 (héritage d'affinité) | hérité | MiniServer (`select()` bloquant, sans timeout périodique), TimerThread (réveil à l'échéance suivante : ré-annonce SSDP), 3 pools (send/recv/miniserver) avec 2 threads minimum chacun, en attente sur condition variable. **Aucun réveil périodique propre en régime établi** hormis le timer SSDP et le trafic entrant (M-SEARCH, SOAP, SUBSCRIBE). | [E] libupnp 1.14 (défauts MIN_THREADS=2, MAX_THREADS=12) |
| 8 | **Thread de préchargement** (`AudioEngine::m_preloadThread`, créé dans `process()` `AudioEngine.cpp:2188-2198` ou `play()` `:2019-2026`) | **hérite du créateur** : créé depuis `process()` → **cœur 3, SCHED_FIFO 80** (sémantique `PTHREAD_INHERIT_SCHED` par défaut) | FIFO 80 | Transitoire, **au début de chaque piste** (dès `SetNextAVTransportURI`) : `avformat_open_input` + `find_stream_info` sur l'URL suivante = connexion TCP, lecture d'en-tête (+ jusqu'à 5 Mo de `probesize` pour les URL non locales, §2.2), parsing CPU **au niveau RT sur le cœur de décodage**. | [V] création ; [S] héritage FIFO/affinité (non mesuré) |
| 9 | **Thread détaché par `Play`** (`UPnPDevice.cpp:554-565`) | 1 (hérite du thread libupnp) | hérité | Transitoire, à chaque action Play : `onPlay` (ouverture piste, `DirettaSync::open`, sleep 100 ms de stabilisation `DirettaRenderer.cpp:690-695`). | [V] |
| 10 | **Log drain thread** (`main.cpp:169-188`) | 1 | — | 100 Hz (`sleep_for(10ms)`) — **uniquement en `--verbose`** (`main.cpp:571-574`). Absent en `--quiet`. | [V] |

Points notables :
- **Deux threads « ne font rien » mais se réveillent** (main + « UPnP Thread », 2 réveils/s sur le cœur 1, cœur `nohz_full` — chaque réveil = hrtimer + retour au tick).
- Le thread de décodage se réveille **100 fois/s en permanence**, même à l'arrêt.
- Tous les `std::thread` héritent politique + affinité du créateur : le préchargement HTTP tourne en RT sur le cœur audio-décodage ; les threads libupnp/SDK/main tournent sur le cœur 1 avec la politique du service.
- `mlockall(MCL_CURRENT|MCL_FUTURE)` (`main.cpp:553`) : **chaque création de thread (8 Mo de pile par défaut) est pré-fautée et verrouillée** → threads 8 et 9 coûtent un `mmap`+zeroing de 8 Mo par piste / par Play, sauf réutilisation du cache de piles glibc (≤ 40 Mo). [S]

### 1.2 Activités du processus en régime établi (--quiet --minimal-upnp)

| Activité | Cadence | Cœur | Réf. |
|---|---|---|---|
| Vérification niveau ring + décision | 100 Hz | 3 | `DirettaRenderer.cpp:953-963` |
| Décodage 2048 échantillons + `sendAudio` | à 44,1 kHz : ~toutes les 46 ms (2048/44100) ; à 192 kHz : ~toutes les 10,7 ms (≈ chaque réveil) | 3 | `:938, :963` ; `AudioEngine.cpp:2232, :2265` |
| `recv()` TCP 32 Ko (remplissage tampon avio FFmpeg) | 44,1/16 FLAC : ~3,3/s ; 192/24 FLAC : ~21/s (§2) | 3 | `AudioEngine.cpp:1364` (via libavformat) |
| `getNewStream()` (memcpy 264 o à 44,1 k, ou ≈ MTU au-delà de 96 k) | ~1000/s (buffers « 1 ms » ≤ 96 kHz) ; regroupés par cycle Diretta | 2 | `DirettaSync.cpp:1686-1948` |
| Paquets Diretta UDP sortants | 177 pps (44,1/24, MTU 1500) … 770 pps (192/24, MTU 1500) ; 29 / 128 pps en MTU 9000 | 2 (xmit) + IRQ TX sur CPU 0 | `DirettaSync.h:293-313` (calcul) |
| Paquets « info » Diretta | 10 Hz (`infoCycle` 100 ms) | SDK | `DirettaSync.h:332`, `DirettaSync.cpp:189` |
| `fetch_add/fetch_sub` sur `m_ringUsers` (RingAccessGuard) | à chaque `sendAudio`, `getBufferLevel` **et** `getNewStream` → **ligne de cache partagée entre cœur 2 (RT) et cœur 3** à ~1 kHz + 100 Hz | 2 ↔ 3 | `DirettaSync.cpp:80-110, :1511, :1633, :1766` |
| Réponses SOAP `GetPositionInfo`/`GetTransportInfo` | **pilotées par le point de contrôle** (Audirvana/Bubble/mconnect ~1 Hz ; JPLAY : non ; LMS/slim2UPnP : à vérifier). **Toujours servies en `--minimal-upnp`** | 1 | `UPnPDevice.cpp:687-725` |
| Réponses SSDP aux M-SEARCH du LAN | subies (chaque appareil UPnP du LAN) | 1 | libupnp [E] |
| NOTIFY SSDP `ssdp:alive` | toutes les ~14,5 min (Exp/2 − 30 s avec Exp = 1800, `UPnPDevice.cpp:170`) | 1 | [E] libupnp `AUTO_ADVERTISEMENT_TIME` |

### 1.3 Activités hors processus laissées par le déploiement

| Élément | Comportement | Périodique ? | Réf. |
|---|---|---|---|
| `diretta-renderer.slice` | `AllowedCPUs=1-3`, `CPUQuota=100%` | non | `diretta-renderer-tuner.sh:311-325` |
| Drop-in `10-isolation.conf` | **`CPUSchedulingPolicy=fifo` + `CPUSchedulingPriority=90`, `Nice=-19`** appliqués au processus entier → `start-renderer.sh` puis le binaire démarrent en **FIFO 90** ; tous les threads qui ne re-fixent pas leur priorité (main, « UPnP Thread », position, libupnp, thread SDK, thread détaché Play) tournent en **FIFO 90, au-dessus du worker audio (80)**. `NICE_LEVEL=-20` du wrapper est sans effet sous FIFO. | non, mais structurel | `tuner.sh:336-361` ; `start-renderer.sh:41-44, :358` |
| `set-irq-affinity-diretta.service` | oneshot au boot : toutes les IRQ → CPU 0 ; log `/var/log/irq-affinity-diretta.log` | non | `tuner.sh:366-410` |
| `start-renderer.sh` IRQ | à chaque démarrage : IRQ de `IRQ_INTERFACE` → `IRQ_CPUS` (0) | non | `start-renderer.sh:86-105` |
| `cpu-performance-diretta.service` | oneshot au boot (gouverneur performance cœurs 1-3) | non | `tuner.sh:414-440` |
| `distribute-diretta-threads.sh` | script généré mais **non branché** (commentaire explicite) | non | `tuner.sh:357-360, :442-534` |
| `diretta-renderer-webui.service` (Python `HTTPServer.serve_forever`) | bloqué en `accept()` ; **aucun sondage du renderer, aucun timer, aucun JS de rafraîchissement** (template statique, vérifié par grep). Ne lit la conf qu'au chargement de page. Hors slice → tourne sur CPU 0. | non | `webui/diretta_webui.py:468-475`, `webui/diretta-renderer-webui.service:8-10` |
| journald | reçoit chaque ligne `std::cout`/`cerr` (`StandardOutput=journal`) ; écrit sur disque si journal persistant (Pi5 en mode dev), en RAM si `systemd.volatile=state` (x86) | par ligne de log | `systemd/diretta-renderer.service:19-21` |
| Pile TCP/NAPI de la NIC LAN (flux HTTP) et de la NIC Diretta | softirq RX sur CPU 0 (`irqaffinity=0`) ; l'émission Diretta se fait dans le contexte de `sendto()` sur le cœur 2 | par paquet | noyau [E] |

---

## 2. Profil réseau de la lecture HTTP

### 2.1 Comment le corps HTTP est lu [V]

- Pas de lecteur maison : `avformat_open_input(url)` → protocole `http` de libavformat sur `tcp` (`AudioEngine.cpp:308`). Exceptions : DFF (parseur maison sur `avio_open2`, `:839-984`, lectures de 32 Ko `:1068, :1116`) et flux PCM Audirvana (contexte HTTP interne enveloppé dans un `AVIOContext` custom de 32 Ko, `:242-282`).
- Le décodeur ne lit **que ce que le ring accepte** : la boucle audio ne décode que sous 50 % de ring (`DirettaRenderer.cpp:958-963`) ; `sendAudio` refuse ce qui ne rentre pas et le callback attend par micro-sommeils de 500 µs (`:494-518`). **Le fichier n'est donc pas téléchargé d'un bloc** : la lecture est cadencée par la consommation.
- Profondeur d'anticipation (read-ahead) empilée :
  1. ring `DirettaSync` : 0,5 s local PCM, 3,0 s « remote », 2,0 s > 192 kHz, 0,8 s DSD (`DirettaSync.h:203-206`) — arrondi à la puissance de 2 supérieure (`DirettaRingBuffer.h:150`) : 44,1/24 → 262 144 o (~1 s), 192/24 → 1 048 576 o (~0,9 s) ;
  2. FIFO décodeur ≤ 8192 échantillons (`AudioEngine.cpp:1691-1694, :1772-1776`) ;
  3. tampon avio FFmpeg : **32 Ko** (`IO_BUFFER_SIZE`) — voir remarque ci-dessous sur `buffer_size` ;
  4. lookahead du parseur FLAC (~10 trames ≈ 100-150 Ko) [E] ;
  5. tampon de réception TCP du noyau (`rcvbuf` auto-ajusté ; défaut initial 128 Ko, croît seulement si l'application lit vite) [E].
  → total ≈ 2-3 s d'avance à 44,1/16 FLAC, ≈ 1 s à 192/24.
- Options HTTP posées (`:201-230`) : `timeout` 10 s (30 s local), `user_agent`, et pour les URL non locales `reconnect`/`reconnect_streamed`/`reconnect_delay_max=5`/`multiple_requests`/`ignore_eof`. **`buffer_size` (256 Ko local / 512 Ko remote) est très probablement sans effet** : ce n'est une option ni de `http` ni de `tcp` (qui expose `recv_buffer_size`/`send_buffer_size`) ; le dictionnaire résiduel est libéré sans contrôle (`:325`). Les commentaires « to absorb LAN jitter » décrivent donc un tampon qui n'existe pas. [S — à vérifier avec `ffmpeg -h protocol=tcp` de la version installée]
- Distinction local / remote (`:145-154`) : `isStreamingProxy` = URL contenant « qobuz »/« tidal » (même en 192.168.x.x) → traité comme **remote** : `probesize` par défaut **5 Mo** (le plafond 32 Ko n'est appliqué qu'aux locaux, `:330-333`), ring 3 s, prefill 500 ms. Le commentaire `:327-329` reconnaît les « massive concurrent reads during anticipated preload » que cela provoque.
- Timeouts/relances : interrupt callback à 5 s sur `av_read_frame` (`:101-108, :1360-1365`) ; reconnexion jusqu'à 3× sur flux live (`:2312-2353`).

### 2.2 Forme du trafic résultante

Régime établi : chaque vidage du tampon avio déclenche un `recv()` de 32 Ko ; la fenêtre TCP se rouvre de 32 Ko et le serveur envoie **un train de ~22 trames de 1500 o à la vitesse du lien** (0,27 ms à 1 GbE), puis silence jusqu'à la lecture suivante. Le « paquebot » est en fait une noria de barges de 32 Ko dont la cadence est dictée par la consommation, **pas** par la fenêtre TCP — sauf aux deux moments suivants où c'est bien la fenêtre/`probesize` qui commande : **à l'ouverture** (en-tête + lookahead + remplissage du rcvbuf : quelques centaines de Ko en quelques ms) et **au préchargement anticipé de la piste suivante, dès le début de la piste courante** (`AudioEngine.cpp:2185-2198`) : nouvelle connexion, en-tête, `probesize` (32 Ko local, **5 Mo** via proxy Qobuz/Tidal), puis la connexion reste ouverte et inactive avec ~128-256 Ko non lus dans le noyau jusqu'à la transition.

| | 44,1/16 FLAC (~850 kbit/s) | 192/24 FLAC (~5,5 Mbit/s) |
|---|---|---|
| Débit moyen HTTP | ~106 Ko/s | ~690 Ko/s |
| Quantum de lecture | 32 Ko | 32 Ko |
| Rafales | ~3,3/s, chacune ~22 trames en ~0,27 ms (1 GbE) | ~21/s, idem |
| Rapport cyclique du lien | ~0,1 % | ~0,6 % |
| Trou entre rafales | ~300 ms | ~47 ms |
| Ouverture de piste | ~0,3-0,4 Mo en rafale | ~0,3-0,4 Mo |
| Préchargement (début de piste) | +1 connexion ; 5 Mo en rafale si URL « qobuz/tidal » | idem |
| Côté Diretta (après négociation 24 bit, `DirettaSync.cpp:1045-1088` : une source 16 bit part en 24 bit) | 264,6 Ko/s = 2,1 Mbit/s **continus** ; MTU 1500 : cycle 5,66 ms → 177 pps ; MTU 9000 : ~34 ms → 29 pps | 1,152 Mo/s = 9,2 Mbit/s ; MTU 1500 : 1,3 ms → 770 pps ; MTU 9000 : 7,8 ms → 128 pps |

Les cycles Diretta sont calculés par `DirettaCycleCalculator` (`DirettaSync.h:300-308`) ; en mode `VarMax`/`VarAuto` le SDK peut les ajuster [E]. Le contraste avec l'image du TGV évoquée dans la demande est donc exact mais déplacé : la double rame de TGV (Diretta) roule en continu ; en face, le HTTP n'est pas un paquebot en régime établi mais **des rafales courtes de 32 Ko à pleine vitesse du lien**, et le paquebot n'apparaît qu'aux ouvertures/préchargements.

Chemin des octets audio (mémoire) : DMA NIC → skb noyau (CPU 0) → `copy_to_user` dans le tampon avio (cœur 3) → paquet FFmpeg (malloc par trame FLAC ~10 Ko) → trame décodée (pool) → `swr_convert` vers `m_resampleBuffer` (256 Ko) → `memcpy_audio` vers `m_buffer` → `push16To24`/`push` dans le ring → `pop` de 264 o par le worker (cœur 2) → paquet SDK → `sendto` → skb → DMA. Six à sept copies.

### 2.3 Rendre le flux « filet d'eau » — propositions

1. **Thread de prefetch dédié sur le cœur 1, file bornée, lecture par callback `AVIOContext`** (modèle qbz2diretta : 64 Ko / 4 Mo). Le motif existe déjà dans le code (enveloppe Audirvana `AudioEngine.cpp:242-282`) : ouvrir le HTTP avec `avio_open2`, le lire depuis le thread de prefetch, servir le décodeur par un `AVIOContext` custom lisant la file. Gains : plus aucun `recv()`/`copy_to_user` sur le cœur 3 ; anticipation profonde en RAM (4 Mo ≈ 40 s à 44,1 FLAC) ; le thread doit être créé avec **affinité et politique explicites** (cœur 1, SCHED_OTHER) pour ne pas hériter FIFO 80/cœur 3. Le seek passe par le callback seek (Range HTTP sur le contexte interne, vidage de la file).
2. **Lissage réseau** dans ce même thread : (a) lectures cadencées (toutes les ~100 ms, taille = consommation, ex. 10 Ko à 44,1 FLAC) une fois la file pleine ; (b) borner le rcvbuf via l'option `tcp` **`recv_buffer_size`** (ex. 64 Ko) pour que le serveur ne puisse jamais émettre plus de 64 Ko d'un coup. Le noyau devient l'élément de régulation avec de petits quanta réguliers : ~7 trames par 100 ms au lieu de 22 par 300 ms.
3. À défaut, à coût nul : **`probesize=32768` + `max_analyze_duration=0` pour toutes les URL HTTP** (pas seulement locales) → supprime la rafale de 5 Mo au préchargement des flux Qobuz/Tidal via proxy.
4. Hystérésis du ring (voir §5) pour rendre le décodage plus rare et plus gros ; sans elle, agrandir `--pcm-buffer-seconds` **ne change pas** le quantum de 2048 échantillons.

---

## 3. Activité UPnP pendant la lecture

### 3.1 Sans `--minimal-upnp` [V]
- Position thread 1 Hz (`DirettaRenderer.cpp:1005-1054`) : `getPosition()` + `setCurrentPosition`/`setTrackDuration`/`notifyPositionChange` — **aucun événement réseau** (la position n'est jamais eventée, conformément à la spec AVTransport, `UPnPDevice.cpp:1001-1002`). C'est donc du CPU cœur 1 + 3 verrous, rien sur le fil.
- Événements GENA `LastChange` (`UpnpNotify`, `:1051-1056`) uniquement sur changement d'état : Play/Pause/Stop, transition gapless (`notifyGaplessTransition :2009-2026`), fin de playlist. Chaque NOTIFY = 1 requête HTTP par abonné (thread pool libupnp, cœur 1).
- SOAP servis à la demande du point de contrôle.

### 3.2 Avec `--minimal-upnp` [V]
Ce que le flag coupe : le position thread (`:821-825`) et **l'émission** des événements AVTransport/RenderingControl (`UPnPDevice.cpp:1005, :1068`). Ce qui **reste** :
- **SUBSCRIBE acceptés** (`handleSubscriptionRequest :349-442` n'est pas gaté) → événement initial envoyé par `UpnpAcceptSubscription` (obligatoire par la spec), puis **renouvellements** du point de contrôle (typiquement toutes les 15 min pour un timeout de 1800 s, plus souvent pour des CP demandant 300 s) — tous répondus HTTP 200 par libupnp.
- **`GetPositionInfo`/`GetTransportInfo` toujours servis** : le libellé « no position polling » de l'aide (`main.cpp:259`) désigne le thread interne, pas le sondage des CP. Chaque sondage = connexion/requête HTTP, parsing ixml, réponse construite par `createActionResponse`/`addResponseArg` (`:957-978`), appel `m_positionCallback` (`:693-694`) qui lit `m_samplesPlayed` écrit par le cœur 3 (lecture non atomique, bénigne). Coût ~1 ms CPU cœur 1 par sondage.
- SSDP : `ssdp:alive` toutes les ~14,5 min (max-age 1800, `:170`), réponses M-SEARCH, service de `description.xml`/SCPD depuis `/tmp/upnp_scpd` (tmpfs, `PrivateTmp`).
- Conséquence fonctionnelle : à la fin d'une playlist non gapless, `notifyStateChange("STOPPED")` ne part plus ; seuls les CP qui **sondent** `GetTransportInfo` enchaînent (commentaire `DirettaRenderer.cpp:600-604`). JPLAY/LMS utilisent `SetNextAVTransportURI` → OK.

### 3.3 Ce qui est requis / réductible
| Élément | Requis par | Réductible ? |
|---|---|---|
| ssdp:alive périodique | UPnP DA (max-age ≥ 1800 s) | **Oui** : `UpnpSendAdvertisement(h, 86400)` → 1 NOTIFY/12 h. Les CP redécouvrent par M-SEARCH. Risque faible. |
| Réponses M-SEARCH | découverte | Non (subi). |
| Événement initial + renouvellements GENA | spec GENA ; Audirvana/Bubble/UAPP en dépendent | Refuser SUBSCRIBE en mode « ultra-minimal » supprimerait les renouvellements, mais casserait ces CP. Sans objet pour JPLAY. |
| `GetPositionInfo` | CP qui sondent | Non côté renderer (c'est le CP qui décide). Le coût interne est déjà minimal. |
| Position thread / événements | UI des CP | Déjà coupés. |

---

## 4. Journalisation, allocations, disque

### 4.1 Ce qui logue encore en `--quiet` + `NOLOG` [V]
- `NOLOG` ne compile hors que `DIRETTA_LOG`/`DIRETTA_LOG_ASYNC` (`DirettaSync.h:117-137`) ; `LOG_INFO`/`LOG_DEBUG` sont gatés par `g_logLevel` (`LogLevel.h:26-40`, coût = 1 lecture globale).
- Mais **des dizaines de `std::cout` bruts ne sont pas gatés** et sortent même en `--quiet` : à chaque piste `[AudioDecoder] Opening…` (`AudioEngine.cpp:111`), `Opened successfully` (`:786`), `[AudioEngine] Opening track / Track opened` (`:2449-2470`), `Next URI queued` (`:1977`), `Pending next URI applied`, `Anticipated preload started` (`:2164, :2195`), `Bytes read from stream` / `Samples decoded` en fin de piste (`:1371, :1379`), `Transitioning to next track` (`:2367`), bannières `DirettaSync::open` (`DirettaSync.cpp:479-482, :510-518, :559, :863`), `[UPnPDevice] Play` (`UPnPDevice.cpp:546`), `[DirettaRenderer] Play` (`:660`), etc. En DSD : `printf` inconditionnels des 3-5 premiers paquets **de chaque piste** (`AudioEngine.cpp:1165-1170, :1241-1252, :1274-1282`). Underrun/rebuffering : `LOG_WARN` (visibles, `DirettaSync.cpp:1891, :1907`) et `cerr` en fin de session (`:1430-1433`).
- Volume : ~10-20 lignes par changement de piste, **zéro en régime établi**. Chaque ligne : `TimestampedStreambuf::overflow` **appelé caractère par caractère** (pas de zone de mise en tampon, `TimestampedLogger.h:59-74`), un `stringstream` + `localtime()` par ligne (`:41-51`), puis `endl` → `write(2)` vers le socket journald → réveil de journald (hors slice) → écriture disque si journal persistant.
- Le hot path est propre : dans `sendAudio`/`getNewStream`, tout est sous `g_verbose` (`DirettaSync.cpp:1618-1626, :1870-1876, :1918`).

### 4.2 Allocations
- **Par paquet/trame** [V] : un `av_buffer` par paquet lu par `av_read_frame` (≈10/s à 44,1 k FLAC, ≈47/s à 192 k) ; trames décodées via pool FFmpeg (réutilisées) ; `m_packet`/`m_frame` réutilisés (`:1345-1351`) ; `swr_convert` vers tampon membre préalloué 256 Ko (`:1789-1794`) ; FIFO `av_audio_fifo` bornée ; ring : aucune allocation (`DirettaRingBuffer.h:309-333, :1347-1361`). **Zéro allocation dans `getNewStream`** sauf redimensionnement de `m_streamData` au changement de format (`DirettaSync.cpp:1755-1757`).
- **Par piste** [V] : `AudioDecoder` + contextes FFmpeg + swr + FIFO + 256 Ko ; copies multiples de l'URI et des métadonnées DIDL (plusieurs Ko, ~10 copies entre UPnPDevice/DirettaRenderer/AudioEngine) ; `m_ringBuffer.resize()` à chaque `open()` complet ou changement de format (pas en « quick resume ») avec **`memset` de tout le ring** (`DirettaRingBuffer.h:149-156, :191-193` — jusqu'à 1 Mo à 192/24, allocation `posix_memalign` `:78`) ; création d'un thread (pile 8 Mo verrouillée par `MCL_FUTURE`).
- **Contention inter-cœurs** [V] : `m_ringUsers` (RingAccessGuard) incrémenté/décrémenté par les cœurs 2 et 3 à ~1 kHz (`DirettaSync.cpp:80-110`) ; `readPos_` (écrit par le cœur 2) lu à 100 Hz par `getBufferLevel` sur le cœur 3 ; `writePos_` lu par le cœur 2 à chaque pop. `writePos_`/`readPos_` sont bien isolés (`alignas(64)`, `DirettaRingBuffer.h:1472-1473`), `m_ringUsers` ne l'est pas.
- **Croissance mémoire observée** [S] : la note Pi5 mesure **13,8 Go verrouillés après 24 h** ≈ **160 Ko/s** — ordre de grandeur du débit PCM 16 bit (176 Ko/s) ou du débit Diretta 24 bit (265 Ko/s) sur un cycle d'utilisation < 100 %. Rien dans le code lu ne retient le flux ; le suspect le plus cohérent est le **tampon de retransmission du SDK**, que le flag `LIMITRESEND=4096` « Limit the amount of retransmission buffer allocated » (`Sync.hpp:43-44`) suggère non borné par défaut. Si c'est confirmé, l'activité associée pendant la lecture est réelle : ~40 fautes de page + zeroing noyau par seconde, en continu, mémoire verrouillée. Test : `THREAD_MODE=4097` (CRITICAL|LIMITRESEND ; `OCCUPIED` est ajouté automatiquement) et suivre `grep VmLck /proc/$(pidof DirettaRendererUPnP)/status` sur une heure ; sinon `smaps` pour localiser la région qui grossit (heap vs anon).

### 4.3 Disque et relectures de configuration [V]
- Au démarrage seulement : 3 `system("mkdir -p …")` + écriture des SCPD dans `/tmp/upnp_scpd` (`UPnPDevice.cpp:119-140`), lecture `/sys/devices/system/cpu/online` (`main.cpp:102-132`), `EnvironmentFile` par systemd. **Aucune relecture de configuration en cours de lecture.**
- Pendant la lecture : uniquement journald (voir 4.1). Divergence de doc à noter : `diretta-renderer.conf:289-291` et l'aide `main.cpp:410-413` annoncent remote 1,0 s / prefill 150 ms, le code applique **3,0 s / 500 ms** (`DirettaSync.h:205, :211`).

---

## 5. Changements proposés, par priorité

| # | Changement | Effet attendu | Risque | Effort |
|---|---|---|---|---|
| 1 | **Hystérésis du thread de décodage** : réveil quand ring < 30 %, remplir jusqu'à 70 %, sommeil 20-50 ms (ou CV notifiée par `getNewStream` sous seuil — le mécanisme `m_spaceAvailable` existe `DirettaSync.cpp:1941-1944`). Aujourd'hui : 100 Hz + quantum 2048. | Réveils cœur 3 de 100 Hz → quelques Hz ; décodage et `recv()` regroupés en rafales rares (à 44,1 k : ~1 rafale/200 ms au lieu de toutes les 46 ms). | Faible (ring 0,5 s = 150 ms de marge à 30 % ; vérifier > 192 kHz où le ring est 2 s). La variante CV ajoute un `futex_wake` occasionnel côté RT. | 1-2 h |
| 2 | **Thread de prefetch persistant** (cœur 1, SCHED_OTHER explicite, file 4 Mo, lecture 64 Ko, `AVIOContext` custom) + **lissage** (`recv_buffer_size` 64 Ko et/ou lectures cadencées). | Plus de syscalls réseau ni de `copy_to_user` sur le cœur de décodage ; rafales réseau bornées à 64 Ko et régulières ; anticipation profonde ; le préchargement cesse de tourner en FIFO 80 sur le cœur 3. | Moyen (seek, flux live/chunked, reconnexion remote à réimplémenter au-dessus de la file). | 1-2 j |
| 3 | **`probesize=32768`, `max_analyze_duration=0` pour toutes les URL HTTP** (`AudioEngine.cpp:330-333`). | Supprime la rafale de 5 Mo au début de chaque piste via proxy Qobuz/Tidal. | Faible (en-têtes FLAC/WAV/AIFF/DSF < 32 Ko ; les blocs PICTURE FLAC sont lus par le demuxer indépendamment). | 15 min |
| 4 | **Diagnostiquer la croissance de mémoire verrouillée** (test `THREAD_MODE=4097`, suivi `VmLck`). | Si confirmé : suppression de ~40 fautes de page/s continues et d'une empreinte qui finit par épuiser la RAM. | Nul pour le test ; le flag est documenté par le SDK. | 1 h |
| 5 | **Supprimer « UPnP Thread » et la boucle 1 Hz de main** (attente sur CV/`sigwait`). | −2 réveils/s sur le cœur 1 (nohz_full). | Nul. | 30 min |
| 6 | **Rendre `--quiet` réellement silencieux** : passer les `std::cout`/`printf` inconditionnels des chemins de piste en `LOG_INFO`/`LOG_DEBUG` ; donner une zone de mise en tampon à `TimestampedStreambuf`. | Plus d'écritures journald ni de réveil de journald aux changements de piste (surtout DSD : `printf` hexadécimaux). | Nul. | 1-2 h |
| 7 | **Ne plus créer de threads pendant la lecture** : préchargement (`:2191`) et `Play` détaché (`UPnPDevice.cpp:562`) via un thread de contrôle persistant, affinité/politique fixées explicitement (cœur 1, SCHED_OTHER). | Évite `mmap` + pré-faute de 8 Mo verrouillés par piste/Play ; évite qu'un thread FIFO 80 concurrence le décodage sur le cœur 3. | Faible. | 2 h (inclus dans #2 si fait) |
| 8 | **Cohérence des priorités avec le tuner** : soit retirer `CPUSchedulingPolicy/Priority` du drop-in (`tuner.sh:342-343`), DRUP fixant lui-même ses priorités, soit forcer SCHED_OTHER sur main/libupnp/position. | Les threads de contrôle (SOAP, SSDP, Play) cessent de tourner en FIFO 90 au-dessus du worker audio ; `NICE_LEVEL` redevient effectif. | Faible ; à confirmer d'abord par `ps -T -o tid,psr,cls,rtprio,comm`. | 15 min |
| 9 | **SSDP max-age 1800 → 86400** (`UPnPDevice.cpp:170`). | ~100 rafales NOTIFY/jour → 2. | Faible (spec : ≥ 1800 autorisé). | 5 min |
| 10 | **`alignas(64)` sur `m_ringUsers`** (ou remplacer RingAccessGuard par un seqlock/génération sans compteur partagé) (`DirettaSync.h:636`). | Supprime un ping-pong de ligne de cache 2↔3 à ~1 kHz. | Faible. | 30 min |
| 11 | Option « ultra-minimal » : refuser SUBSCRIBE (`handleSubscriptionRequest`) pour JPLAY/LMS uniquement. | Plus aucun renouvellement GENA. | Moyen (casse Audirvana/Bubble/UAPP) — à ne pas activer par défaut. | 30 min |
| 12 | `--info-cycle` plus long (ex. 500 000 µs). | −8 paquets info/s côté Diretta. | Moyen/inconnu (régulation SDK) — à tester à l'oreille et sur les underruns. | 5 min |

### Mesures à faire avant/après
- `perf stat -e context-switches,cpu-migrations -C 1,2,3 -- sleep 60` et `top -H -p $(pidof DirettaRendererUPnP)` (réveils par thread).
- `ss -tmi` pendant la lecture (taille réelle du rcvbuf → tranche la question `buffer_size`), `tcpdump -i eth-lan -w` + analyse des rafales (quantum 32 Ko attendu).
- `strace -f -e trace=recvfrom,sendto,write -T -c` 60 s (nombre de `recv` 32 Ko, `write` journald).
- `grep VmLck /proc/<pid>/status` toutes les heures ; `/proc/interrupts` (deltas CPU 0).

---

**En résumé** : le code fait déjà l'essentiel (lecture cadencée par la consommation, hot path sans allocation ni log, position/événements coupés). L'activité résiduelle pendant la lecture vient de (1) le thread de décodage à 100 Hz avec un quantum fixe de 2048 échantillons, (2) des `recv()` de 32 Ko qui produisent des rafales à pleine vitesse de lien sur le cœur 3, (3) un préchargement en RT sur le cœur audio avec, pour les proxies Qobuz/Tidal, une rafale de 5 Mo, (4) deux threads inutiles à 1 Hz, (5) des logs non gatés par `--quiet` aux changements de piste, et — à confirmer — (6) une croissance continue de mémoire verrouillée qui est en soi une activité hôte permanente. Le trio #1 + #2 + #3 transforme le « paquebot » en filet régulier et vide le cœur 3 de tout ce qui n'est pas décodage.

---

*Complément (agent, après remise) : le CHANGELOG de DRUP (l.159, entrée `--cpu-decode`) décrit lui-même le thread audio comme faisant « HTTP receive + FFmpeg decode » sur son cœur dédié — confirmation, par la documentation du projet, que toutes les lectures socket tournent aujourd'hui sur le cœur 3 (§2.1, §5-#2) : un thread de préchargement sur le cœur `--cpu-other` est bien le changement qui retire les appels système réseau du cœur de décodage.*
