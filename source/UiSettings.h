#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace asampler
{
    // Per-machine window preferences (window size, the sample browser's
    // folder), kept apart from the project: they belong to the person at this
    // computer, not to the song. Stored in <app data>/ASampler/ui.settings.
    // Shared by every ASampler window in the process.
    class UiSettings
    {
    public:
        UiSettings()
        {
            juce::PropertiesFile::Options options;
            options.applicationName     = "ui";
            options.filenameSuffix      = "settings";
            options.folderName          = "ASampler";
            options.osxLibrarySubFolder = "Application Support";
            options.storageFormat       = juce::PropertiesFile::storeAsXML;
            options.millisecondsBeforeSaving = 500;
            file = std::make_unique<juce::PropertiesFile> (options);
        }

        juce::PropertiesFile& get() noexcept { return *file; }

    private:
        std::unique_ptr<juce::PropertiesFile> file;
    };
}
