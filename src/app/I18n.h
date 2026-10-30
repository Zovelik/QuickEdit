#pragma once

#include <string>
#include <vector>


// Translations of the interface. The English texts of the code are the keys
// of catalogs lang/<code>.json next to the executable: {"_language": "Français",
// "Save Project": "Enregistrer le projet", ...}.
//
// tr() keeps the parts of a label that are not text: the icons at its start
// (Material Design glyphs) and an ImGui ID after "##" or "###". Windows and
// popups whose names are looked up give a stable ID: tr("Timeline###Timeline").
namespace i18n
{
    struct Language
    {
        std::string code;       // "fr"
        std::string name;       // In the language itself: "Français"
    };

    // English, then the catalogs found.
    std::vector<Language> languages();

    // Loads the catalog of the language ("en": none). False when not found.
    bool setLanguage(const std::string& code);
    const std::string& language();

    // The language of the system when a catalog has it, else English.
    std::string systemLanguage();

    // The text in the current language; the pointer stays valid until the
    // language changes.
    const char* tr(const char* text);
    std::string tr(const std::string& text);
}

using i18n::tr;
