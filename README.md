# C2 Network Architecture - Remote Command Executor

Ce projet implémente une architecture réseau minimale de type Command & Control (C2) en langage C. Il permet à un serveur central d'administrer et d'exécuter des commandes système à distance sur plusieurs clients connectés simultanément de manière asynchrone via l'appel système select.

---

## Fonctionnalités

* Gestion Multi-client Concurrente : Utilisation de select() pour gérer jusqu'à 128 clients simultanément sur un seul thread sans blocage.
* Protocole Réseau Structuré : Encapsulation des données via un en-tête personnalisé de 4 octets (Little-Endian) définissant la longueur de la charge utile pour éviter la fragmentation TCP.
* Résilience et Reconnexion Auto :
    * Gestion de mécanismes de Heartbeats (battements de cœur) pour détecter et déconnecter les clients "zombies".
    * Reconnexion automatique infinie du client en cas de perte de liaison avec le serveur.
* Exécution de Commandes : Capture des flux de sortie (stdout/stderr) des commandes exécutées par le client, incluant la gestion native du changement de répertoire (cd).
* Compilation Statique : Configuration du Makefile pour générer des binaires autonomes sans dépendances dynamiques.

---

## Architecture et Protocole

Le protocole réseau utilisé est une couche légère au-dessus de TCP/IP. Chaque message (paquet) est structuré de la manière suivante :

+-----------------------------------+-----------------------------------+
|   Taille du Message (4 octets)    |       Données / Charge utile      |
|         (Little Endian)           |          (Taille variable)        |
+-----------------------------------+-----------------------------------+

* Heartbeat : Un paquet dont la taille est égale à 0 (en-tête uniquement) sert de signal de maintien de vie.
* Commande / Sortie : Le texte brut est encapsulé directement dans la section de données.

---

## Limites actuelles

Avant d'utiliser ou de déployer ce projet, veuillez noter les restrictions techniques suivantes :

1. Absence de Chiffrement : Les communications transitent en texte clair (Cleartext). Aucune couche SSL/TLS ou chiffrement symétrique n'est implémentée, rendant le flux vulnérable aux attaques de type Man-in-the-Middle (MITM).
2. Mode Broadcast Impératif : Le serveur envoie actuellement toutes les commandes saisies à l'ensemble des clients connectés. Il n'est pas possible de cibler un unique client par son identifiant ou son adresse IP.
3. Commandes Interactives Non Supportées : Les commandes nécessitant une interaction utilisateur continue (comme top, nano, sudo, ou les invites de mot de passe) bloqueront ou échoueront car le flux stdin du client n'est pas lié à la commande lancée.

---

## Guide de Compilation

Le projet utilise un fichier Makefile pour automatiser et standardiser le processus de construction des exécutables. La compilation produit des binaires statiques (grâce aux drapeaux -static), ce qui signifie qu'ils intègrent toutes les bibliothèques nécessaires et peuvent être exécutés sur n'importe quel système Linux compatible sans dépendances externes.

### Prérequis
* Un compilateur GCC (GNU Compiler Collection).
* Un environnement de type UNIX (Linux, WSL, ou macOS avec les outils de développement).
* La bibliothèque standard statique (le paquet libc-static peut être requis sur certaines distributions comme Alpine ou CentOS/Fedora).

### Instructions de compilation

Pour compiler le serveur et le client simultanément, placez-vous dans le répertoire du projet et exécutez :
```bash
make
```
Le binaire client obtenu par la compilation est dynamique. Cela signifie qu'il s'appuie sur les bibliothèques partagées standard du système hôte pour s'exécuter, ce qui permet d'obtenir un fichier léger et facile à transférer.
 ### Exécution du binaire
 * Serveur :
```bash
./serveur [port]
```
* Client :
```bash
./client [port] [ip_serveur]
```
