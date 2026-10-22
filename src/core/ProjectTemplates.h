// ProjectTemplates.h
// Project templates and versioning system for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>
#include <map>

// Project template structure
struct ProjectTemplate
{
    juce::String name;
    juce::String description;
    juce::String category;
    juce::File templateFile;
    juce::Image thumbnail;
};

// Project version structure
struct ProjectVersion
{
    juce::String versionName;
    juce::String author;
    juce::String date;
    juce::String notes;
    juce::File projectFile;
};

// Project templates and versioning manager
class ProjectTemplates
{
public:
    ProjectTemplates();
    ~ProjectTemplates();

    // Load built-in templates
    void loadBuiltInTemplates();

    // Load user templates from directory
    void loadUserTemplates(const juce::File& directory);

    // Save current project as template
    bool saveAsTemplate(const juce::String& name, const juce::String& description,
                        const juce::String& category, const juce::File& outputFile);

    // Create new project from template
    bool createFromTemplate(const juce::String& templateName, const juce::File& outputFile);

    // Save project version
    bool saveVersion(const juce::String& versionName, const juce::String& notes);

    // Load project version
    bool loadVersion(const juce::String& versionName);

    // Get available templates
    const std::vector<ProjectTemplate>& getTemplates() const { return templates; }

    // Get project versions
    const std::vector<ProjectVersion>& getVersions() const { return versions; }

    // Set current project file
    void setCurrentProjectFile(const juce::File& file);

    // Get current project file
    juce::File getCurrentProjectFile() const { return currentProjectFile; }

    // Set auto-save interval (seconds)
    void setAutoSaveInterval(int seconds);

    // Start auto-save timer
    void startAutoSave();

    // Stop auto-save timer
    void stopAutoSave();

private:
    std::vector<ProjectTemplate> templates;
    std::vector<ProjectVersion> versions;
    juce::File currentProjectFile;
    juce::File versionsDirectory;
    juce::File templatesDirectory;
    int autoSaveInterval;
    juce::Timer autoSaveTimer;

    // Internal methods
    void autoSaveTimerCallback();
    bool saveProjectToFile(const juce::File& file);
    bool loadProjectFromFile(const juce::File& file);
};
