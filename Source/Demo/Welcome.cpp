#include "Welcome.h"
#include "BinaryData.h"

namespace lathe::welcome
{
String songName()   { return "Welcome to Lathe"; }

Result write (const File& projectsDir, File& projectFile)
{
    const MemoryBlock song (BinaryData::Welcome_lathe, (size_t) BinaryData::Welcome_latheSize);
    auto folder = projectsDir.getChildFile (songName());
    auto file = folder.getChildFile (songName() + kProjectExtension);

    // Opened before: the copy there is theirs now (they may have changed it), so open that.
    if (file.existsAsFile())
    {
        projectFile = file;
        return Result::ok();
    }
    if (folder.exists())
    {
        folder = folder.getNonexistentSibling (true);
        file = folder.getChildFile (folder.getFileName() + kProjectExtension);
    }
    if (! folder.getChildFile ("Audio").createDirectory())
        return Result::fail ("Couldn't make " + folder.getFullPathName());
    if (! file.replaceWithData (song.getData(), song.getSize()))
        return Result::fail ("Couldn't write " + file.getFullPathName());
    projectFile = file;
    return Result::ok();
}
} // namespace lathe::welcome
