#pragma once

#include "../Common.h"

/** File > Import Folder as New Project: what's in a folder of stems, and what to call each track. */
namespace lathe::stemfolder
{
inline const char* audioExtensions() { return "wav;aif;aiff;flac;mp3;ogg"; }
inline const char* midiExtensions()  { return "mid;midi"; }

struct Found
{
    Array<File> audio, midi;   // in name order ("2 Bass" before "10 Keys")
    int other = 0;             // files Lathe can't import (a PDF of the lyrics, say)
};

/** The audio and MIDI files directly in `folder` (not in folders inside it). Hidden files are
    left out, and so are macOS's "._" copies that a drive formatted for Windows leaves next to
    each file. */
inline Found find (const File& folder)
{
    Found out;
    for (auto& f : folder.findChildFiles (File::findFiles, false))
    {
        const auto name = f.getFileName();
        if (name.startsWithChar ('.') || f.isHidden())
            continue;
        if (f.hasFileExtension (audioExtensions()))
            out.audio.add (f);
        else if (f.hasFileExtension (midiExtensions()))
            out.midi.add (f);
        else
            ++out.other;
    }
    auto byName = [] (Array<File>& files)
    {
        std::sort (files.begin(), files.end(), [] (const File& a, const File& b)
        {
            return a.getFileName().compareNatural (b.getFileName()) < 0;
        });
    };
    byName (out.audio);
    byName (out.midi);
    return out;
}

/** Track names from file names: the file's name without its extension, and without what every
    file's name starts with ("Song_Kick", "Song_Snare": "Kick", "Snare") or a number in front for
    the order ("01 Kick": "Kick"). A name is kept whole if trimming would leave nothing. */
inline StringArray trackNames (const Array<File>& files)
{
    StringArray names;
    for (auto& f : files)
        names.add (f.getFileNameWithoutExtension().trim());

    auto isSeparator = [] (juce_wchar c) { return c == ' ' || c == '_' || c == '-' || c == '.'; };

    // What they all start with, cut back to the last separator in it (so "Kick" and "Keys"
    // don't lose their "K").
    if (names.size() > 1)
    {
        int common = names[0].length();
        for (auto& n : names)
        {
            int i = 0;
            while (i < common && i < n.length() && n[i] == names[0][i])
                ++i;
            common = i;
        }
        while (common > 0 && ! isSeparator (names[0][common - 1]))
            --common;
        bool allLeft = common > 0;
        for (auto& n : names)
            if (n.substring (common).trimCharactersAtStart (" _-.").isEmpty())
                allLeft = false;
        if (allLeft)
            for (auto& n : names)
                n = n.substring (common).trimCharactersAtStart (" _-.");
    }

    for (auto& n : names)
    {
        // "01 Kick", "02_Snare", "3 - Bass", "4. Keys"
        int i = 0;
        while (i < n.length() && i < 3 && CharacterFunctions::isDigit (n[i]))
            ++i;
        if (i > 0 && i < n.length() && isSeparator (n[i]))
        {
            const auto rest = n.substring (i).trimCharactersAtStart (" _-.");
            if (rest.isNotEmpty())
                n = rest;
        }
    }
    return names;
}
} // namespace lathe::stemfolder
