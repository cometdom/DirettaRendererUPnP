# Guide : Installer DirettaRendererUPnP sur Fedora 43 Minimal
Auteur : SwissBearMountains

## Introduction

Ce guide vous accompagne dans la mise en place d'un lecteur audio haute-fidélité sur une machine sans écran (headless) utilisant Fedora 43 Minimal. Nous prenons en charge les architectures x86_64 (Intel/AMD) et ARM64 (Raspberry Pi 4/5).

**Durée estimée :** 45-60 minutes

**Ce dont vous avez besoin :**
- Un ordinateur : x86_64 (Intel NUC, mini PC) ou ARM64 (Raspberry Pi 4/5)
- Une clé USB (8 Go minimum) pour l'installation
- Un adaptateur USB-Ethernet avec chipset RTL8156 (recommandé pour l'audio Diretta)
- Une connexion réseau (Ethernet recommandé pour l'audio)
- Un autre ordinateur pour préparer la clé USB et transférer les fichiers
- Temporairement : un écran, un clavier et une souris pour la configuration initiale

---

# PARTIE A : Devant la machine

*Vous aurez besoin d'un écran, d'un clavier et d'une souris pour cette partie.*

---

## Étape 1 : Télécharger et créer le support d'installation

Sur votre ordinateur principal, préparez la clé USB bootable.

### 1.1 Télécharger Fedora 43 Minimal

**Pour x86_64 (Intel/AMD) :**
- Allez sur : https://fedoraproject.org/server/download
- Sélectionnez **Network Install** (netinst) pour x86_64
- Fichier : `Fedora-Server-netinst-x86_64-43-*.iso`

**Pour ARM64 (aarch64) :**
- Allez sur : https://fedoraproject.org/server/download
- Sélectionnez **Network Install** (netinst) pour aarch64
- Fichier : `Fedora-Server-netinst-aarch64-43-*.iso`

### 1.2 Créer la clé USB bootable avec balenaEtcher

1. Téléchargez [balenaEtcher](https://etcher.balena.io/) pour votre système
2. Installez et lancez balenaEtcher
3. Cliquez sur **Flash from file** → sélectionnez l'ISO Fedora
4. Cliquez sur **Select target** → choisissez votre clé USB
5. Cliquez sur **Flash!**
6. Attendez la fin et la vérification

---

## Étape 2 : Installer Fedora 43 Minimal

### 2.1 Démarrer depuis la clé USB

1. Insérez la clé USB dans votre PC audio
2. Connectez l'écran, le clavier, la souris et le câble Ethernet
3. Allumez et entrez dans le BIOS/UEFI (généralement F2, F12, Suppr ou Échap)
4. Définissez la clé USB comme premier périphérique de démarrage
5. Enregistrez et redémarrez

### 2.2 Étapes d'installation

Lorsque l'installateur démarre :

1. **Sélection de la langue** → Français (ou votre préférence) → Continuer

2. **Destination de l'installation**
   - Sélectionnez votre disque cible
   - Choisissez le partitionnement "Automatique"
   - Cliquez sur Terminé

3. **Sélection des logiciels** → **Installation minimale**
   - C'est crucial - sélectionnez uniquement "Installation minimale"
   - N'ajoutez AUCUN groupe de logiciels supplémentaire
   - Cliquez sur Terminé

4. **Réseau et nom d'hôte**
   - Activez votre interface réseau
   - Définissez un nom d'hôte (ex : `diretta-renderer`)
   - Cliquez sur Terminé

5. **Mot de passe root**
   - Définissez un mot de passe root robuste
   - Cochez "Autoriser la connexion SSH root avec mot de passe"

6. **Création d'utilisateur**
   - Créez un compte utilisateur (ex : `audiophile`)
   - Faites de cet utilisateur un administrateur
   - Définissez un mot de passe

7. Cliquez sur **Commencer l'installation**

8. Attendez la fin de l'installation, puis **Redémarrez**

---

## Étape 3 : Activer SSH et noter l'adresse IP

Après le redémarrage, retirez la clé USB et connectez-vous avec votre compte utilisateur.

### 3.1 Installer et activer SSH

```bash
sudo dnf install -y openssh-server
sudo systemctl enable sshd
sudo systemctl start sshd
```

### 3.2 Noter votre adresse IP

```bash
ip addr show
```

Cherchez une adresse comme `192.168.1.100` — notez-la !

---

## Étape 4 : Connecter l'adaptateur USB-Ethernet (RTL8156)

Branchez votre adaptateur USB-Ethernet sur un port USB du PC audio. Le chipset RTL8156 est recommandé pour un streaming audio Diretta optimal.

Connectez le câble Ethernet de votre réseau audio à cet adaptateur.

### 4.1 Vérifier la détection

```bash
lsusb | grep -i realtek
```

Vous devriez voir quelque chose comme : `Realtek Semiconductor Corp. RTL8156`

### 4.2 Vérifier l'interface réseau

```bash
ip link
```

Vous devriez voir une nouvelle interface nommée `eth1` ou `enxXXXXXXXXXXXX` (où X est l'adresse MAC).

---

## Étape 5 : Déconnecter l'écran, le clavier et la souris

Vous avez terminé devant la machine. Débranchez l'écran, le clavier et la souris.

Votre PC audio est maintenant sans écran et prêt pour la configuration à distance.

---

# PARTIE B : Depuis le canapé

*Tout ce qui suit se fait à distance depuis votre ordinateur principal via SSH.*

---

## Étape 6 : Se connecter via SSH

Depuis votre ordinateur principal (Terminal sur Mac/Linux, PowerShell sur Windows) :

```bash
ssh audiophile@192.168.1.100
```

Remplacez `192.168.1.100` par l'adresse IP que vous avez notée précédemment.

---

## Étape 7 : Exécuter le script d'optimisation

### 7.1 Créer le script

```bash
nano ~/optimize-fedora-audio.sh
```

Choisissez et collez le script approprié pour votre architecture :

---

#### SCRIPT POUR x86_64 (Intel/AMD) - avec noyau RT CachyOS

```bash
#!/bin/bash
# Script d'optimisation audio Fedora 43 - x86_64 avec noyau RT

set -e
echo "=== Optimisation DirettaRendererUPnP pour x86_64 ==="

echo "=== Installation des paquets requis ==="
sudo dnf install -y kernel-devel make dwarves tar zstd rsync curl wget unzip htop

echo "=== Désactivation des services inutiles ==="

# Désactiver le démon d'audit
sudo systemctl disable auditd 2>/dev/null || true
sudo systemctl stop auditd 2>/dev/null || true

# Supprimer le pare-feu (inutile pour l'audio dédié)
sudo systemctl stop firewalld 2>/dev/null || true
sudo dnf remove -y firewalld 2>/dev/null || true

# Supprimer SELinux (simplifie la configuration audio)
sudo dnf remove -y selinux-policy 2>/dev/null || true

# Désactiver journald (réduit les écritures disque)
sudo systemctl disable systemd-journald 2>/dev/null || true
sudo systemctl stop systemd-journald 2>/dev/null || true

# Désactiver le démon OOM
sudo systemctl disable systemd-oomd 2>/dev/null || true
sudo systemctl stop systemd-oomd 2>/dev/null || true

# Désactiver le démon home
sudo systemctl disable systemd-homed 2>/dev/null || true
sudo systemctl stop systemd-homed 2>/dev/null || true

# Supprimer PolicyKit
sudo systemctl stop polkitd 2>/dev/null || true
sudo dnf remove -y polkit 2>/dev/null || true

echo "=== Installation du noyau temps réel CachyOS ==="
sudo dnf copr enable -y bieszczaders/kernel-cachyos
sudo dnf install -y kernel-cachyos-rt kernel-cachyos-rt-devel-matched

echo "=== Configuration des paramètres de démarrage du noyau ==="
sudo grubby --update-kernel=ALL --args="audit=0 zswap.enabled=0 skew_tick=1 nosoftlockup default_hugepagesz=1G intel_pstate=enable"

echo ""
echo "=== Optimisation terminée ! ==="
echo "Le système va redémarrer dans 10 secondes..."
sleep 10
sudo reboot
```

---

#### SCRIPT POUR ARM64 (aarch64) - sans noyau RT

```bash
#!/bin/bash
# Script d'optimisation audio Fedora 43 - ARM64

set -e
echo "=== Optimisation DirettaRendererUPnP pour ARM64 ==="

echo "=== Installation des paquets requis ==="
sudo dnf install -y kernel-devel make dwarves tar zstd rsync curl wget unzip htop

echo "=== Désactivation des services inutiles ==="

# Désactiver le démon d'audit
sudo systemctl disable auditd 2>/dev/null || true
sudo systemctl stop auditd 2>/dev/null || true

# Supprimer le pare-feu (inutile pour l'audio dédié)
sudo systemctl stop firewalld 2>/dev/null || true
sudo dnf remove -y firewalld 2>/dev/null || true

# Supprimer SELinux (simplifie la configuration audio)
sudo dnf remove -y selinux-policy 2>/dev/null || true

# Désactiver journald (réduit les écritures disque)
sudo systemctl disable systemd-journald 2>/dev/null || true
sudo systemctl stop systemd-journald 2>/dev/null || true

# Désactiver le démon OOM
sudo systemctl disable systemd-oomd 2>/dev/null || true
sudo systemctl stop systemd-oomd 2>/dev/null || true

# Désactiver le démon home
sudo systemctl disable systemd-homed 2>/dev/null || true
sudo systemctl stop systemd-homed 2>/dev/null || true

# Supprimer PolicyKit
sudo systemctl stop polkitd 2>/dev/null || true
sudo dnf remove -y polkit 2>/dev/null || true

echo "=== Configuration des paramètres de démarrage du noyau ==="
sudo grubby --update-kernel=ALL --args="audit=0 zswap.enabled=0 skew_tick=1 nosoftlockup"

echo ""
echo "=== Optimisation terminée ! ==="
echo "Le système va redémarrer dans 10 secondes..."
sleep 10
sudo reboot
```

---

Enregistrez et quittez : **Ctrl+O**, **Entrée**, **Ctrl+X**

### 7.2 Exécuter le script

```bash
chmod +x ~/optimize-fedora-audio.sh
sudo ~/optimize-fedora-audio.sh
```

Le système redémarrera automatiquement. Attendez 1-2 minutes, puis reconnectez-vous :

```bash
ssh audiophile@192.168.1.100
```

### 7.3 Vérifier le noyau RT (x86_64 uniquement)

```bash
uname -r
# Devrait afficher quelque chose comme : 6.x.x-cachyos-rt
```

---

## Étape 8 : Transférer le SDK Diretta

Le SDK Diretta Host est nécessaire pour la compilation. Téléchargez-le sur votre ordinateur principal :

1. Allez sur [diretta.link](https://www.diretta.link/hostsdk.html)
2. Téléchargez **DirettaHostSDK_148** (ou la dernière version)
3. Fichier attendu : `DirettaHostSDK_148_8.tar.zst` (ou similaire)

### 8.1 Transférer via SCP

Depuis votre ordinateur principal (Terminal sur Mac/Linux, PowerShell sur Windows) :

```bash
scp DirettaHostSDK_148_8.tar.zst audiophile@192.168.1.100:~/
```

> **Depuis Windows (PowerShell) :**
> ```powershell
> scp C:\Users\VotreNom\Downloads\DirettaHostSDK_148_8.tar.zst audiophile@192.168.1.100:~/
> ```

---

## Étape 9 : Installer DirettaRendererUPnP

Reconnectez-vous en SSH à la machine Fedora :

```bash
ssh audiophile@192.168.1.100
```

### 9.1 Installer les dépendances de compilation

```bash
sudo dnf install -y git gcc-c++ make ffmpeg-free-devel libupnp-devel
```

### 9.2 Extraire le SDK Diretta

```bash
cd ~
tar --zstd -xf DirettaHostSDK_148_8.tar.zst
```

Le SDK sera extrait dans `~/DirettaHostSDK_148/` (ou similaire).

### 9.3 Cloner le dépôt et installer

```bash
cd ~
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP
chmod +x install.sh
./install.sh
```

Le script d'installation propose un menu interactif avec les options suivantes :
- Compilation de l'application (détection automatique de l'architecture et du SDK)
- Installation en tant que service systemd
- Configuration du démarrage automatique
- Configuration de la cible Diretta

---

## Étape 10 : Vérifier et profiter

### 10.1 Lister les cibles Diretta disponibles

```bash
sudo ./bin/DirettaRendererUPnP --list-targets
```

### 10.2 Lancer le lecteur

```bash
# Lancement avec la cible numéro 1
sudo ./bin/DirettaRendererUPnP --target 1

# Lancement avec les logs détaillés (pour le dépannage)
sudo ./bin/DirettaRendererUPnP --target 1 --verbose
```

### 10.3 Tester avec votre contrôleur UPnP

Sur votre réseau, utilisez un point de contrôle UPnP :
- **Audirvana** (macOS/Windows) — DSD natif, gapless nativement supporté (désactivez "Universal Gapless" dans Audirvana)
- **JPlay iOS** (iOS) — Support complet
- **BubbleUPnP** (Android) — Très configurable
- **mConnect** (iOS/Android) — Interface épurée
- **foobar2000** (Windows) — avec plugin UPnP

Le lecteur devrait apparaître sous le nom "Diretta Renderer" dans la liste des appareils.

### 10.4 Mettre à jour ultérieurement

Pour mettre à jour vers une nouvelle version :

```bash
cd ~/DirettaRendererUPnP
git pull
make clean && make
```

---

## Aide-mémoire

```bash
# === PARTIE A : Devant la machine ===
# Installez Fedora, puis :
sudo dnf install -y openssh-server
sudo systemctl enable sshd
sudo systemctl start sshd
ip addr show   # Notez l'adresse IP !

# === PARTIE B : Depuis le canapé ===
ssh audiophile@<IP>

# Exécuter le script d'optimisation
nano ~/optimize-fedora-audio.sh
chmod +x ~/optimize-fedora-audio.sh
sudo ~/optimize-fedora-audio.sh

# Transférer le SDK (depuis l'ordinateur principal)
scp DirettaHostSDK_148_8.tar.zst audiophile@<IP>:~/

# Installer les dépendances
sudo dnf install -y git gcc-c++ make ffmpeg-free-devel libupnp-devel

# Extraire le SDK
tar --zstd -xf DirettaHostSDK_148_8.tar.zst

# Cloner et installer DirettaRendererUPnP
git clone https://github.com/cometdom/DirettaRendererUPnP.git
cd DirettaRendererUPnP
chmod +x install.sh
./install.sh

# Lancer le lecteur
sudo ./bin/DirettaRendererUPnP --list-targets
sudo ./bin/DirettaRendererUPnP --target 1
```

---

## Dépannage

### Impossible de se connecter en SSH après le redémarrage
- Attendez 2-3 minutes que le système démarre complètement
- Essayez : `ping diretta-renderer.local`
- Vérifiez la page d'administration de votre routeur pour trouver l'appareil

### Adaptateur USB-Ethernet non détecté
```bash
# Vérifier les périphériques USB
lsusb

# Vérifier les messages du noyau
dmesg | tail -30

# Lister les interfaces réseau
ip link
```

Si l'adaptateur est détecté mais n'a pas d'IP, vérifiez votre serveur DHCP ou configurez-le manuellement.

### tar ne peut pas extraire .tar.zst
```bash
sudo dnf install -y zstd
tar --zstd -xvf fichier.tar.zst
```

### Le lecteur n'apparaît pas sur le réseau
```bash
sudo systemctl status diretta-renderer
sudo systemctl restart diretta-renderer
```

---

## Support

Pour toute question ou problème, veuillez ouvrir une issue sur le dépôt GitHub.

Bonne écoute !
