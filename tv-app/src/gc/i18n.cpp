// Game Compressor TV app - Interface languages (English and French).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gc/i18n.hpp"

#include <unordered_map>

namespace gc::i18n
{

namespace
{

struct Entry
{
    const char *en;
    const char *fr;
};

constexpr Entry kTable[] = {
    // Header and states
    {"Connected", "Connecté"},
    {"Starting the payload...", "Démarrage du payload..."},
    {"Payload not running", "Payload arrêté"},
    {"Update available - press Options", "Mise à jour disponible - appuyez sur Options"},
    {"Update installed - reopen the app", "Mise à jour installée - rouvrez l'application"},
    {"Loading games...", "Chargement des jeux..."},
    {"The Game Compressor payload is not running.", "Le payload Game Compressor n'est pas lancé."},
    {"Make sure the jailbreak and its ELF loader (port 9021) are active, then retry.",
     "Vérifiez que le jailbreak et son ELF loader (port 9021) sont actifs, puis réessayez."},
    {"Starting Game Compressor...", "Démarrage de Game Compressor..."},
    {"No game found.", "Aucun jeu trouvé."},
    {"Press the touchpad to refresh.", "Appuyez sur le pavé tactile pour actualiser."},
    {"Couldn't open the system browser.", "Impossible d'ouvrir le navigateur système."},
    {"Measuring...", "Mesure en cours..."},
    {"Working", "En cours"},
    {"Unavailable", "Indisponible"},
    {"Image", "Image"},
    // Detail panel
    {"Title ID", "ID du titre"},
    {"Type", "Type"},
    {"Storage", "Stockage"},
    {"Size", "Taille"},
    {"Compressed", "Compressé"},
    {"Saved", "Économisé"},
    {"Validation", "Validation"},
    {"validated", "validé"},
    {"not-validated", "non validé"},
    {"unknown", "inconnu"},
    {"failed", "échec"},
    {"n/a", "s/o"},
    {"internal", "interne"},
    // Hints
    {"Actions", "Actions"},
    {"Web interface", "Interface web"},
    {"Settings", "Paramètres"},
    {"Refresh", "Actualiser"},
    {"Cancel job", "Annuler le job"},
    {"Select", "Choisir"},
    {"Back", "Retour"},
    {"Change / Start", "Changer / Lancer"},
    // Game menu
    {"Compress...", "Compresser..."},
    {"Make image...", "Créer une image..."},
    {"Validate and repair", "Valider et réparer"},
    {"Validate only", "Valider seulement"},
    {"Uncompress...", "Décompresser..."},
    {"Set read-only", "Mettre en lecture seule"},
    {"Extract to folder", "Extraire vers un dossier"},
    {"Read speed test", "Test de vitesse de lecture"},
    {"Update AMPR to {version}", "Mettre à jour AMPR vers {version}"},
    {"Choose AMPR version...", "Choisir la version AMPR..."},
    {"Restore original AMPR", "Restaurer l'AMPR d'origine"},
    {"Build AMPR index", "Construire l'index AMPR"},
    {"Copy to...", "Copier vers..."},
    {"Move to...", "Déplacer vers..."},
    {"Delete game data", "Supprimer les données du jeu"},
    {"Copy to", "Copier vers"},
    {"Move to", "Déplacer vers"},
    {"Keep the game", "Garder le jeu"},
    {"Yes, delete game data", "Oui, supprimer les données"},
    {"Delete {name}?", "Supprimer {name} ?"},
    {"AMPR version", "Version AMPR"},
    {"{version}  (installed)", "{version}  (installée)"},
    {"{version}  (latest)", "{version}  (dernière)"},
    {"No AMPR build cached yet (open the web interface once)",
     "Aucune version AMPR en cache (ouvrez une fois l'interface web)"},
    {"Internal storage", "Stockage interne"},
    {"{name}  ({free} free)", "{name}  ({free} libres)"},
    {"This entry can't be processed from here.", "Cette entrée ne peut pas être traitée ici."},
    {"Another job is running. Wait or cancel it with Square.",
     "Un autre job est en cours. Attendez ou annulez-le avec Carré."},
    // Forms
    {"Compress", "Compresser"},
    {"Make image", "Créer une image"},
    {"Uncompress", "Décompresser"},
    {"Format", "Format"},
    {"Original data", "Données d'origine"},
    {"Delete after", "Supprimer après"},
    {"Keep", "Conserver"},
    {"Delete while working (risky)", "Supprimer pendant le travail (risqué)"},
    {"Result goes to", "Résultat envoyé vers"},
    {"Same place", "Même emplacement"},
    {"Output", "Sortie"},
    {"App folder", "Dossier du jeu"},
    {"Destination", "Destination"},
    {"In place", "Sur place"},
    {"Compressed file", "Fichier compressé"},
    {"Start", "Lancer"},
    // Settings
    {"Check for updates", "Rechercher les mises à jour"},
    {"Checking for updates...", "Recherche de mises à jour..."},
    {"Payload not reachable", "Payload injoignable"},
    {"Payload outdated", "Payload obsolète"},
    {"restarting it from this app's copy...", "redémarrage depuis la copie de l'application..."},
    {"Install update", "Installer la mise à jour"},
    {"build {latest} available (installed: {current})",
     "build {latest} disponible (installé : {current})"},
    {"Downloading {file}", "Téléchargement de {file}"},
    {"Verifying...", "Vérification..."},
    {"Installing...", "Installation..."},
    {"Update installed", "Mise à jour installée"},
    {"app folder copy: update by hand", "copie en dossier : à mettre à jour à la main"},
    {"reopen the app", "rouvrez l'application"},
    {"Up to date (build {build})", "À jour (build {build})"},
    {"build {build}", "build {build}"},
    {"Automatic updates", "Mises à jour automatiques"},
    {"On", "Activées"},
    {"Off", "Désactivées"},
    {"Language", "Langue"},
    {"Theme", "Thème"},
    {"Automatic", "Automatique"},
    {"Restart the Game Compressor payload", "Redémarrer le payload Game Compressor"},
    {"Refresh the game list", "Actualiser la liste des jeux"},
    {"Open the web interface", "Ouvrir l'interface web"},
    {"system browser", "navigateur système"},
    {"A job is running: wait for it to finish first.",
     "Un job est en cours : attendez qu'il se termine."},
    {"Restarting the payload...", "Redémarrage du payload..."},
    {"TV app {app}   -   payload {payload} (build {build})",
     "Application TV {app}   -   payload {payload} (build {build})"},
    {"Latest release: {version} (build {build})", "Dernière version : {version} (build {build})"},
    {"Updates come from the project's GitHub releases. The app image is replaced in place; reopen the app afterwards.",
     "Les mises à jour viennent des releases GitHub du projet. L'image de l'application est remplacée sur place ; rouvrez l'application ensuite."},
    // Messages from the store
    {"{action} started: {name}", "{action} lancé : {name}"},
    {"{action} started", "{action} lancé"},
    {"{action} refused: {reason}", "{action} refusé : {reason}"},
    {"{action} refused", "{action} refusé"},
    {"Cancel", "Annuler"},
    {"Updating Game Compressor to build {build}...",
     "Mise à jour de Game Compressor vers le build {build}..."},
    {"Updated to build {build}. The app folder copy was not changed: update it by hand.",
     "Mise à jour vers le build {build}. La copie en dossier n'a pas été modifiée : mettez-la à jour à la main."},
    {"Updated to build {build} in {path}. Close and reopen the app to use it.",
     "Mise à jour vers le build {build} dans {path}. Fermez et rouvrez l'application."},
    {"Updated to build {build}. Close and reopen the app to use the new interface.",
     "Mise à jour vers le build {build}. Fermez et rouvrez l'application pour utiliser la nouvelle interface."},
    {"Restarted the payload with the version bundled in this app.",
     "Payload redémarré avec la version embarquée dans cette application."},
    {"The running payload is outdated: {detail}", "Le payload en cours est obsolète : {detail}"},
    // Payload starter
    {"Could not read {path}.", "Impossible de lire {path}."},
    {"Could not open a connection on this console.", "Impossible d'ouvrir une connexion sur cette console."},
    {"No ELF loader is listening on port {port}.", "Aucun ELF loader n'écoute sur le port {port}."},
    {"Could not reach the ELF loader on port {port}.", "Impossible de joindre l'ELF loader sur le port {port}."},
    {"The ELF loader stopped taking Game Compressor part way.",
     "L'ELF loader a cessé de recevoir Game Compressor en cours de route."},
    {"No copy of Game Compressor was found to start.",
     "Aucune copie de Game Compressor à démarrer."},
    {"Sent Game Compressor {version} to the ELF loader.",
     "Game Compressor {version} envoyé à l'ELF loader."},
    {"(version unknown)", "(version inconnue)"},
};

Language g_language = Language::automatic;
int g_system_language = 1;

const std::unordered_map<std::string_view, const char *> &french_table()
{
    static const std::unordered_map<std::string_view, const char *> table = []
    {
        std::unordered_map<std::string_view, const char *> map;
        for (const Entry &entry : kTable)
            map.emplace(entry.en, entry.fr);
        return map;
    }();
    return table;
}

} // namespace

void set_system_language(int system_language)
{
    g_system_language = system_language;
}

void set_language(Language language)
{
    g_language = language;
}

Language language()
{
    return g_language;
}

bool french()
{
    if (g_language == Language::french)
        return true;
    if (g_language == Language::english)
        return false;
    return g_system_language == 2 || g_system_language == 22; // French, French (Canada)
}

std::string tr(std::string_view english, Args args)
{
    std::string text(english);
    if (french())
    {
        const auto &table = french_table();
        const auto it = table.find(english);
        if (it != table.end())
            text = it->second;
    }
    if (args.size() == 0)
        return text;
    for (const auto &arg : args)
    {
        const std::string key = std::string("{") + arg.first + "}";
        for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at))
        {
            text.replace(at, key.size(), arg.second);
            at += arg.second.size();
        }
    }
    return text;
}

} // namespace gc::i18n
