#pragma once
#include "launcher_game.h"
#include <iterator>
#include <string>
#include <utility>

namespace gsr_collection {

// Flag order: English, Spanish, Italian, French, German. SDL message boxes
// take UTF-8 directly. Each dialog chooses its own game's language, so two
// games with different flags never share a global UI-language setting.
struct CollectionPopupText {
    const char* build_title;
    const char* build_question;
    const char* build_both;
    const char* build_one;
    const char* build_retry;
    const char* save_title;
    const char* save_question;
    const char* save_convert;
    const char* save_skip;
    const char* build_single_question;
    const char* build_start;
    const char* build_cancel;
    const char* build_less_resources;
    const char* build_resource_notice;
};

inline constexpr CollectionPopupText kCollectionPopupText[] = {
    {
        "Game build",
        "{game} needs to be built before it can start.\n{other} needs building too.\n\nBuild both now?",
        "Build both", "Only this game", "The build did not finish. Please try again.",
        "Convert English save?",
        "An English save for {game} was found.\n\nWould you like to convert it to English?\n\n"
        "Only in-game saves are converted.\n"
        "F-key saves (F2, etc.) do NOT carry over.\n"
        "They stay available in their original language.\n\n"
        "Your characters will be moved to the nearest sanctuary.\n\n"
        "Your English save will be kept.\nSaved progress will be separate for each language.",
        "Convert save", "Start without it",
        "Build or rebuild {game}?", "Build game", "Cancel",
        "Use fewer system resources",
        "Limits the build to 50% of CPU and RAM.\nBuilding may take longer.\nOther apps use additional resources."
    },
    {
        "Preparar el juego",
        "Hay que preparar {game} antes de iniciarlo.\n{other} también necesita prepararse.\n\n¿Quieres preparar ambos ahora?",
        "Ambos", "Solo este juego", "La preparación no ha terminado. Inténtalo de nuevo.",
        "¿Convertir la partida en inglés?",
        "Se ha encontrado una partida en inglés de {game}.\n\n¿Quieres convertirla al español?\n\n"
        "Solo se convierten las partidas guardadas dentro del juego.\n"
        "Las partidas rápidas guardadas con las teclas F\n"
        "(F2, etc.) NO se transfieren.\n"
        "Se conservan para su idioma original.\n\n"
        "Tus personajes se trasladarán al santuario más cercano.\n\n"
        "Se conservará tu partida en inglés.\nCada idioma tendrá su propio progreso guardado.",
        "Convertir", "Iniciar sin ella",
        "¿Preparar o volver a preparar {game}?", "Preparar", "Cancelar",
        "Usar menos recursos del sistema",
        "Limita la preparación al 50% de CPU y RAM.\nPuede tardar más.\nLas otras aplicaciones usan recursos adicionales."
    },
    {
        "Prepara il gioco",
        "Devi preparare {game} prima di avviarlo.\nAnche {other} deve essere preparato.\n\nVuoi prepararli entrambi ora?",
        "Entrambi", "Solo questo", "La preparazione non è terminata. Riprova.",
        "Convertire il salvataggio inglese?",
        "È stato trovato un salvataggio inglese di {game}.\n\nVuoi convertirlo in italiano?\n\n"
        "Si convertono solo i salvataggi effettuati nel gioco.\n"
        "I salvataggi rapidi con i tasti F (F2, ecc.)\n"
        "NON vengono trasferiti. Restano nella lingua originale.\n\n"
        "I personaggi saranno spostati al santuario più vicino.\n\n"
        "Il salvataggio inglese sarà conservato.\nOgni lingua avrà i propri progressi salvati.",
        "Converti", "Avvia senza",
        "Preparare o ricreare {game}?", "Prepara", "Annulla",
        "Usa meno risorse di sistema",
        "Limita la preparazione al 50% di CPU e RAM.\nPotrebbe richiedere più tempo.\nLe altre applicazioni usano risorse aggiuntive."
    },
    {
        "Préparer le jeu",
        "Il faut préparer {game} avant de le lancer.\n{other} doit aussi être préparé.\n\nPréparer les deux maintenant ?",
        "Les deux", "Ce jeu", "La préparation n'a pas abouti. Réessayez.",
        "Convertir la sauvegarde anglaise ?",
        "Une sauvegarde anglaise de {game} a été trouvée.\n\nVoulez-vous la convertir en français ?\n\n"
        "Seules les sauvegardes faites dans le jeu sont converties.\n"
        "Les sauvegardes rapides avec les touches F (F2, etc.)\n"
        "NE sont PAS transférées.\n"
        "Elles restent disponibles dans leur langue d'origine.\n\n"
        "Vos personnages seront déplacés au sanctuaire le plus proche.\n\n"
        "Votre sauvegarde anglaise sera conservée.\nLa progression sera sauvegardée séparément pour chaque langue.",
        "Convertir", "Lancer sans",
        "Préparer ou recréer {game} ?", "Préparer", "Annuler",
        "Utiliser moins de ressources",
        "Limite la préparation à 50% du CPU et de la RAM.\nElle peut prendre plus de temps.\nLes autres applications utilisent aussi des ressources."
    },
    {
        "Spiel erstellen",
        "{game} muss vor dem Start erstellt werden.\nAuch {other} muss erstellt werden.\n\nJetzt beide erstellen?",
        "Beide erstellen", "Nur dieses", "Das Spiel wurde nicht fertig erstellt. Bitte versuche es erneut.",
        "Englischen Spielstand umwandeln?",
        "Ein englischer Spielstand für {game} wurde gefunden.\n\nMöchtest du ihn ins Deutsche umwandeln?\n\n"
        "Nur Spielstände aus dem Spielmenü werden umgewandelt.\n"
        "Schnellspeicherstände per F-Taste (F2 usw.) werden\n"
        "NICHT übertragen. Sie bleiben in der ursprünglichen Sprache.\n\n"
        "Deine Charaktere werden zum nächsten Heiligtum gebracht.\n\n"
        "Dein englischer Spielstand bleibt erhalten.\nDer Fortschritt wird für jede Sprache getrennt gespeichert.",
        "Umwandeln", "Ohne starten",
        "{game} erstellen oder neu erstellen?", "Erstellen", "Abbrechen",
        "Weniger Systemressourcen nutzen",
        "Begrenzt den Build auf 50% CPU und RAM.\nDas Erstellen kann länger dauern.\nAndere Programme nutzen zusätzliche Ressourcen."
    }
};

inline const CollectionPopupText& collection_popup_text(int language) {
    return kCollectionPopupText[language >= 0 && language < int(std::size(kCollectionPopupText)) ? language : 0];
}

inline std::string popup_game_names(std::string message, int game) {
    for (const auto& token : {std::pair{"{game}", game}, std::pair{"{other}", 1 - game}}) {
        const auto at = message.find(token.first);
        if (at != std::string::npos)
            message.replace(at, std::char_traits<char>::length(token.first), gsr_launcher::kGames[token.second].stem);
    }
    return message;
}

inline std::string build_games_message(int game, int language) {
    return popup_game_names(collection_popup_text(language).build_question, game);
}

}  // namespace gsr_collection
