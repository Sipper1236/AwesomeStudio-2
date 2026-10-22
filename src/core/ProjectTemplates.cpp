// ProjectTemplates.cpp
// Project templates and versioning implementation for Awesome Studio DAW

#include "ProjectTemplates.h"
#include <JuceHeader.h>
#include <fstream>
#include <sstream>

//==============================================================================
ProjectTemplates::ProjectTemplates()
    : currentProjectFile(), versionsDirectory("./versions"), templatesDirectory("./templates"),
      autoSaveInterval(60), autoSaveTimer()
{
    // Create directories if they don't exist
    if (!versionsDirectory.exists()) versionsDirectory.createDirectory();
    if (!templatesDirectory.exists()) templatesDirectory.createDirectory();

    // Load built-in templates
    loadBuiltInTemplates();
}

//==============================================================================
ProjectTemplates::~ProjectTemplates()
{
    stopAutoSave();
}

//==============================================================================
void ProjectTemplates::loadBuiltInTemplates()
{
    // Clear existing templates
    templates.clear();

    // Add some built-in templates
    templates.push_back({
        "Blank Project",
        "A blank project template for new compositions",
        "Composition",
        templatesDirectory.getChildFile("BlankProject.aws"),
        juce::ImageCache::getSingleton().loadImage("blank_template_thumb.png")
    });

    templates.push_back({
        "Drum Project",
        "Template for drum programming",
        "Drum",
        templatesDirectory.getChildFile("DrumProject.aws"),
        juce::ImageCache::getSingleton().loadImage("drum_template_thumb.png")
    });

    templates.push_back({
        "Vocal Project",
        "Template for vocal recording",
        "Vocal",
        templatesDirectory.getChildFile("VocalProject.aws"),
        juce::ImageCache::getSingleton().loadImage("vocal_template_thumb.png")
    });

    // Load any user templates
    loadUserTemplates(templatesDirectory);
}

//==============================================================================
void ProjectTemplates::loadUserTemplates(const juce::File& directory)
{
    // Find all template files
    juce::Array<juce::File> templateFiles;
    directory.findChildFiles(templateFiles, juce::File::findFiles, false, "*.aws");

    // Load each template
    for (const auto& file : templateFiles) {
        if (file.getFileExtension().equalsIgnoreCase("aws")) {
            templates.push_back({
                file.getFileNameWithoutExtension(),
                "User template",
                "User",
                file,
                juce::ImageCache::getSingleton().loadImage("user_template_thumb.png")
            });
        }
    }
}

//==============================================================================
bool ProjectTemplates::saveAsTemplate(const juce::String& name, const juce::String& description,
                                      const juce::String& category, const juce::File& outputFile)
{
    // Check if template already exists
    for (const auto& templateItem : templates) {
        if (templateItem.name.equalsIgnoreCase(name)) {
            return false; // Template already exists
        }
    }

    // Create template file
    if (!outputFile.existsAsFile()) {
        if (!outputFile.create()) return false;
    }

    // Copy current project to template
    if (!saveProjectToFile(outputFile)) return false;

    // Add to templates list
    templates.push_back({
        name,
        description,
        category,
        outputFile,
        juce::ImageCache::getSingleton().loadImage("user_template_thumb.png")
    });

    return true;
}

//==============================================================================
bool ProjectTemplates::createFromTemplate(const juce::String& templateName, const juce::File& outputFile)
{
    // Find template
    for (const auto& templateItem : templates) {
        if (templateItem.name.equalsIgnoreCase(templateName)) {
            // Copy template to output file
            if (!templateItem.templateFile.copyTo(outputFile)) {
                return false;
            }

            // Set current project
            setCurrentProjectFile(outputFile);
            return true;
        }
    }

    return false; // Template not found
}

//==============================================================================
bool ProjectTemplates::saveVersion(const juce::String& versionName, const juce::String& notes)
{
    // Create version directory if needed
    juce::File versionDir = versionsDirectory.getChildFile(currentProjectFile.getFileNameWithoutExtension());
    if (!versionDir.exists()) {
        versionDir.createDirectory();
    }

    // Create version file
    juce::File versionFile = versionDir.getChildFile(
        currentProjectFile.getFileNameWithoutExtension() + "_" + versionName + ".aws");

    // Save current project
    if (!saveProjectToFile(versionFile)) {
        return false;
    }

    // Add to versions list
    versions.push_back({
        versionName,
        "Awesome Studio",
        juce::Time::getCurrentTime().toString("yyyy-MM-dd HH:mm:ss"),
        notes,
        versionFile
    });

    return true;
}

//==============================================================================
bool ProjectTemplates::loadVersion(const juce::String& versionName)
{
    // Find version directory
    juce::File versionDir = versionsDirectory.getChildFile(currentProjectFile.getFileNameWithoutExtension());
    if (!versionDir.exists()) {
        return false;
    }

    // Find version file
    juce::File versionFile;
    for (const auto& child : versionDir.getChildren()) {
        if (child.getFileName().contains("_") && child.getFileName().endsWith(".aws")) {
            juce::String versionNameStr = child.getFileName().substring(
                child.getFileName().lastOccurrenceOf("_") + 1,
                child.getFileName().length() - 4);

            if (versionNameStr.equalsIgnoreCase(versionName)) {
                versionFile = child;
                break;
            }
        }
    }

    if (!versionFile.exists()) {
        return false;
    }

    // Load version
    if (!loadProjectFromFile(versionFile)) {
        return false;
    }

    return true;
}

//==============================================================================
void ProjectTemplates::setCurrentProjectFile(const juce::File& file)
{
    currentProjectFile = file;

    // Update templates and versions
    loadBuiltInTemplates();
    loadUserTemplates(templatesDirectory);

    // Clear existing versions
    versions.clear();

    // Load versions for current project
    juce::File versionDir = versionsDirectory.getChildFile(file.getFileNameWithoutExtension());
    if (versionDir.exists()) {
        juce::Array<juce::File> versionFiles;
        versionDir.findChildFiles(versionFiles, juce::File::findFiles, false, "*.aws");

        for (const auto& versionFile : versionFiles) {
            juce::String versionName = versionFile.getFileName().substring(
                versionFile.getFileName().lastOccurrenceOf("_") + 1,
                versionFile.getFileName().length() - 4);

            versions.push_back({
                versionName,
                "Awesome Studio",
                juce::Time::getCurrentTime().toString("yyyy-MM-dd HH:mm:ss"),
                "",
                versionFile
            });
        }
    }
}

//==============================================================================
void ProjectTemplates::setAutoSaveInterval(int seconds)
{
    autoSaveInterval = seconds > 0 ? seconds : 60;
}

//==============================================================================
void ProjectTemplates::startAutoSave()
{
    stopAutoSave();
    autoSaveTimer.startTimer(secondsToMilliseconds(autoSaveInterval));
}

//==============================================================================
void ProjectTemplates::stopAutoSave()
{
    autoSaveTimer.stopTimer();
}

//==============================================================================
void ProjectTemplates::autoSaveTimerCallback()
{
    if (currentProjectFile.existsAsFile()) {
        if (saveProjectToFile(currentProjectFile)) {
            // Save successful - add to versions
            juce::String versionName = "AutoSave_";
            versionName += juce::Time::getCurrentTime().toString("yyyyMMdd_HHmmss");

            saveVersion(versionName, "Auto-saved version");
        }
    }
}

//==============================================================================
bool ProjectTemplates::saveProjectToFile(const juce::File& file)
{
    // Create XML document
    juce::XmlElement* doc = new juce::XmlElement("AwesomeStudioProject");
    doc->setAttribute("version", "1.0");

    // Add project metadata
    doc->addAttribute("author", "Awesome Studio Team");
    doc->addAttribute("date", juce::Time::getCurrentTime().toString("yyyy-MM-dd"));

    // Add project settings
    doc->addChildElement("Mixer");
    doc->getChildElement("Mixer")->addAttribute("channels", "8");
    doc->getChildElement("Mixer")->addAttribute("buses", "4");

    doc->addChildElement("Transport");
    doc->getChildElement("Transport")->addAttribute("playbackRate", "1.0");
    doc->getChildElement("Transport")->addAttribute("position", "0");

    // Add audio files
    doc->addChildElement("AudioFiles");
    doc->getChildElement("AudioFiles")->addAttribute("count", "0");

    // Add MIDI files
    doc->addChildElement("MIDIFiles");
    doc->getChildElement("MIDIFiles")->addAttribute("count", "0");

    // Add plugin settings
    doc->addChildElement("Plugins");
    doc->getChildElement("Plugins")->addAttribute("count", "0");

    // Add automation
    doc->addChildElement("Automation");
    doc->getChildElement("Automation")->addAttribute("count", "0");

    // Add mixer settings
    doc->addChildElement("MixerSettings");
    doc->getChildElement("MixerSettings")->addAttribute("count", "0");

    // Save to file
    juce::FileOutputStream fos(file);
    if (!fos.openedSuccessfully()) {
        delete doc;
        return false;
    }

    doc->writeToStream(fos);
    delete doc;

    return true;
}

//==============================================================================
bool ProjectTemplates::loadProjectFromFile(const juce::File& file)
{
    // Load XML document
    juce::XmlDocument doc;
    if (!doc.loadFromFile(file)) {
        return false;
    }

    // Parse project metadata
    juce::XmlElement* root = doc.getDocumentElement();
    if (!root) return false;

    // Set current project file
    setCurrentProjectFile(file);

    return true;
}