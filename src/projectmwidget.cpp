#include "projectmwidget.h"
#include <libprojectM/ProjectM.hpp>
#include <GL/glew.h>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QtMath>
#include <QDebug>
#include <QMessageBox>
#include <vector>

ProjectMWidget::ProjectMWidget(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_initialized(false)
    , m_sampleCount(0)
    , m_phase(0.0f)
    , m_frameCount(0)
    , m_currentPresetIndex(-1)
{
    m_spectrumData.fill(0.0f, NUM_BANDS);
    m_pcmDataLeft.resize(PCM_BUFFER_SIZE);
    m_pcmDataRight.resize(PCM_BUFFER_SIZE);
    m_pcmDataLeft.fill(0.0f);
    m_pcmDataRight.fill(0.0f);

    setMinimumSize(400, 300);

    // Render timer for smooth animation
    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
}

ProjectMWidget::~ProjectMWidget()
{
    // Stop the render timer first to prevent any updates during cleanup
    if (m_renderTimer) {
        m_renderTimer->stop();
    }

    makeCurrent();
    m_projectM.reset();
    doneCurrent();
}

void ProjectMWidget::initializeGL()
{
    emit debugLog("[ProjectM] initializeGL() called");

    // Initialize GLEW first (required for OpenGL extensions)
    GLenum glewError = glewInit();
    if (glewError != GLEW_OK) {
        QString errorMsg = QString("Failed to initialize GLEW: %1")
            .arg(reinterpret_cast<const char*>(glewGetErrorString(glewError)));
        emit debugLog(QString("[ProjectM] ") + errorMsg);
        QMessageBox::critical(nullptr, "ProjectM Error", errorMsg);
        m_initialized = false;
        return;
    }

    emit debugLog("[ProjectM] GLEW initialized successfully");
    emit debugLog(QString("[ProjectM] OpenGL Version: %1").arg(reinterpret_cast<const char*>(glGetString(GL_VERSION))));
    emit debugLog(QString("[ProjectM] GLSL Version: %1").arg(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION))));

    // Now initialize projectM
    initializeProjectM();
}

void ProjectMWidget::initializeProjectM()
{
    try {
        emit debugLog("[ProjectM] Creating ProjectM instance...");
        emit debugLog(QString("[ProjectM] Window size: %1x%2").arg(width()).arg(height()));

        // Create projectM instance with default settings
        m_projectM = std::make_unique<libprojectM::ProjectM>();
        emit debugLog("[ProjectM] Instance created successfully");

        // Configure settings via setter methods
        emit debugLog("[ProjectM] Configuring settings...");
        m_projectM->SetWindowSize(width(), height());
        m_projectM->SetMeshSize(48, 36);
        m_projectM->SetTargetFramesPerSecond(60);
        m_projectM->SetAspectCorrection(true);
        m_projectM->SetEasterEgg(1.0f);
        m_projectM->SetPresetDuration(999999.0);  // Effectively disable auto-change (manual only)
        m_projectM->SetSoftCutDuration(0.0);  // No transition - instant switch
        m_projectM->SetHardCutEnabled(false); // Disable beat-based cuts
        m_projectM->SetHardCutDuration(0.0);
        m_projectM->SetHardCutSensitivity(0.0f);
        m_projectM->SetBeatSensitivity(1.0f);
        emit debugLog("[ProjectM] Settings configured successfully");

        // Configure texture paths
        QString exeDir = QCoreApplication::applicationDirPath();
        QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

        QStringList textureSearchPaths = {
            exeDir + "/textures",                    // .\textures relative to exe
            exeDir,                                  // exe directory itself
            appDataPath + "/textures",
            "C:/Program Files/projectM/textures",
            "C:/projectM/textures",
            "./textures",
            "../textures"
        };

        emit debugLog("[ProjectM] Searching for textures...");
        std::vector<std::string> texturePaths;
        for (const QString& path : textureSearchPaths) {
            QDir dir(path);
            if (dir.exists()) {
                texturePaths.push_back(path.toStdString());
                emit debugLog(QString("[ProjectM] Found texture directory: %1").arg(path));
            }
        }

        if (!texturePaths.empty()) {
            m_projectM->SetTexturePaths(texturePaths);
            emit debugLog(QString("[ProjectM] Set %1 texture path(s)").arg(texturePaths.size()));
        } else {
            emit debugLog(QString("[ProjectM] Warning: No texture directories found. Place textures in: %1/textures").arg(exeDir));
        }

        // Try common preset locations (check exe dir first)
        QStringList presetSearchPaths = {
            exeDir + "/presets",                    // .\presets relative to exe
            appDataPath + "/presets",
            "C:/Program Files/projectM/presets",
            "C:/projectM/presets",
            "./presets",
            "../presets"
        };

        emit debugLog("[ProjectM] Searching for presets...");
        for (const QString& path : presetSearchPaths) {
            QDir dir(path);
            if (dir.exists()) {
                // Look for .milk and .prjm files recursively in subdirectories
                QDirIterator it(path, QStringList() << "*.milk" << "*.prjm",
                               QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    m_presetFiles << it.next();
                }

                if (!m_presetFiles.isEmpty()) {
                    emit debugLog(QString("[ProjectM] Found %1 preset(s) in: %2").arg(m_presetFiles.size()).arg(path));
                    break;  // Stop after finding first directory with presets
                }
            }
        }

        if (!m_presetFiles.isEmpty()) {
            // Load a random preset to avoid "transition to black" presets
            m_currentPresetIndex = QRandomGenerator::global()->bounded(m_presetFiles.size());
            m_currentPreset = m_presetFiles[m_currentPresetIndex];
            emit debugLog(QString("[ProjectM] Loading preset %1 of %2: %3")
                         .arg(m_currentPresetIndex + 1)
                         .arg(m_presetFiles.size())
                         .arg(m_currentPreset));
            m_projectM->LoadPresetFile(m_currentPreset.toStdString(), false);
            emit debugLog("[ProjectM] Preset loaded successfully");
        } else {
            emit debugLog(QString("[ProjectM] Warning: No presets found. Place .milk files in: %1").arg(exeDir + "/presets"));
            emit debugLog("[ProjectM] Visualizer will work but may show default/blank output");
        }

        m_initialized = true;
        // Don't start timer here - will be started when audio begins playing
        emit debugLog("[ProjectM] Initialization complete!");

    } catch (const std::exception& e) {
        QString errorMsg = QString("Failed to initialize projectM: %1").arg(e.what());
        emit debugLog(QString("[ProjectM] ") + errorMsg);
        QMessageBox::critical(nullptr, "ProjectM Error", errorMsg);
        m_initialized = false;
    } catch (...) {
        QString errorMsg = "Failed to initialize projectM: Unknown error";
        emit debugLog(QString("[ProjectM] ") + errorMsg);
        QMessageBox::critical(nullptr, "ProjectM Error", errorMsg);
        m_initialized = false;
    }
}

void ProjectMWidget::resizeGL(int w, int h)
{
    if (m_projectM) {
        m_projectM->SetWindowSize(w, h);
    }
}

void ProjectMWidget::paintGL()
{
    if (!m_initialized || !m_projectM) {
        // Draw error message
        glClearColor(0.1f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Debug: Log every 60 frames
        static int errorFrameCounter = 0;
        if (++errorFrameCounter >= 60) {
            emit debugLog(QString("[ProjectM] paintGL: Not rendering - initialized: %1, projectM valid: %2").arg(m_initialized).arg(m_projectM != nullptr));
            errorFrameCounter = 0;
        }
        return;
    }

    try {
        // Render projectM visualization to Qt's framebuffer
        // QOpenGLWidget uses its own FBO, not the default framebuffer (0)
        m_projectM->RenderFrame(defaultFramebufferObject());

        m_frameCount++;

    } catch (const std::exception& e) {
        emit debugLog(QString("[ProjectM] Render error: %1").arg(e.what()));
        // Draw error screen
        glClearColor(0.2f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void ProjectMWidget::setSpectrumData(const QVector<float>& magnitudes)
{
    QMutexLocker locker(&m_dataMutex);

    if (magnitudes.size() != NUM_BANDS) return;

    // Apply smoothing
    for (int i = 0; i < NUM_BANDS; ++i) {
        m_spectrumData[i] = m_spectrumData[i] * 0.7f + magnitudes[i] * 0.3f;
    }

}

void ProjectMWidget::setPCMData(const std::vector<float>& leftChannel, const std::vector<float>& rightChannel)
{
    if (!m_initialized || !m_projectM) {
        return;
    }

    static int callCount = 0;
    callCount++;

    int count = qMin(leftChannel.size(), rightChannel.size());
    
    // Update m_pcmData arrays for waveform visualization
    QMutexLocker locker(&m_dataMutex);
    for (int i = 0; i < count && i < PCM_BUFFER_SIZE; ++i) {
        m_pcmDataLeft[i] = leftChannel[i];
        m_pcmDataRight[i] = rightChannel[i];
    }

    // Feed PCM to projectM
    std::vector<float> interleaved(count * 2);
    for (int i = 0; i < count; ++i) {
        interleaved[i * 2] = leftChannel[i];
        interleaved[i * 2 + 1] = rightChannel[i];
    }
    m_projectM->PCM().Add(interleaved.data(), 2, count);

}

void ProjectMWidget::generatePCMData()
{
    // Convert spectrum data (frequency domain) back to PCM (time domain)
    // This is an approximation using inverse FFT-like synthesis

    QMutexLocker locker(&m_dataMutex);

    for (int i = 0; i < PCM_BUFFER_SIZE; ++i) {
        float sample = 0.0f;

        // Synthesize waveform from frequency bands
        for (int band = 0; band < NUM_BANDS; ++band) {
            float frequency = (band + 1) * 100.0f;  // Frequency in Hz
            float amplitude = m_spectrumData[band];
            float phase = m_phase + (i * frequency * 2.0f * M_PI / 44100.0f);

            sample += amplitude * qSin(phase) / NUM_BANDS;
        }

        // Clamp to valid range
        sample = qBound(-1.0f, sample, 1.0f);

        m_pcmDataLeft[i] = sample;
        m_pcmDataRight[i] = sample;
    }

    // Update phase for continuity
    m_phase += (PCM_BUFFER_SIZE * 2.0f * M_PI / 44100.0f);
    if (m_phase > M_PI * 2) {
        m_phase -= M_PI * 2;
    }
}

void ProjectMWidget::nextPreset()
{
    if (m_presetFiles.isEmpty() || !m_projectM) {
        emit debugLog("[ProjectM] No presets available");
        return;
    }

    m_currentPresetIndex = (m_currentPresetIndex + 1) % m_presetFiles.size();
    loadPreset(m_presetFiles[m_currentPresetIndex]);
    emit debugLog(QString("[ProjectM] Next preset: %1/%2").arg(m_currentPresetIndex + 1).arg(m_presetFiles.size()));
}

void ProjectMWidget::previousPreset()
{
    if (m_presetFiles.isEmpty() || !m_projectM) {
        emit debugLog("[ProjectM] No presets available");
        return;
    }

    m_currentPresetIndex--;
    if (m_currentPresetIndex < 0) {
        m_currentPresetIndex = m_presetFiles.size() - 1;
    }
    loadPreset(m_presetFiles[m_currentPresetIndex]);
    emit debugLog(QString("[ProjectM] Previous preset: %1/%2").arg(m_currentPresetIndex + 1).arg(m_presetFiles.size()));
}

void ProjectMWidget::randomPreset()
{
    if (m_presetFiles.isEmpty() || !m_projectM) {
        emit debugLog("[ProjectM] No presets available");
        return;
    }

    // Pick a random preset (different from current)
    if (m_presetFiles.size() > 1) {
        int newIndex;
        do {
            newIndex = QRandomGenerator::global()->bounded(m_presetFiles.size());
        } while (newIndex == m_currentPresetIndex);
        m_currentPresetIndex = newIndex;
    }

    loadPreset(m_presetFiles[m_currentPresetIndex]);
    emit debugLog(QString("[ProjectM] Random preset: %1/%2").arg(m_currentPresetIndex + 1).arg(m_presetFiles.size()));
}

void ProjectMWidget::loadPreset(const QString& presetPath)
{
    if (!m_projectM || !m_initialized) {
        emit debugLog("[ProjectM] Cannot load preset - not initialized");
        return;
    }

    try {
        // Make OpenGL context current before calling projectM methods
        makeCurrent();

        // Use smooth transition (true = blend from previous preset)
        m_projectM->LoadPresetFile(presetPath.toStdString(), true);
        m_currentPreset = presetPath;

        // Update index if this preset is in our list
        int index = m_presetFiles.indexOf(presetPath);
        if (index >= 0) {
            m_currentPresetIndex = index;
        }

        doneCurrent();

        QFileInfo info(presetPath);
        emit debugLog(QString("[ProjectM] Loaded preset: %1").arg(info.fileName()));
    } catch (const std::exception& e) {
        doneCurrent();
        emit debugLog(QString("[ProjectM] Failed to load preset: %1").arg(e.what()));
        QMessageBox::warning(nullptr, "Preset Error",
            QString("Failed to load preset: %1").arg(e.what()));
    }
}

QString ProjectMWidget::currentPresetName() const
{
    if (!m_currentPreset.isEmpty()) {
        QFileInfo info(m_currentPreset);
        return info.baseName();
    }
    return "No preset loaded";
}

void ProjectMWidget::pauseRendering()
{
    if (m_renderTimer && m_renderTimer->isActive()) {
        m_renderTimer->stop();
        emit debugLog("[ProjectM] Rendering paused");
    }
}

void ProjectMWidget::resumeRendering()
{
    if (m_renderTimer && !m_renderTimer->isActive() && m_initialized) {
        m_renderTimer->start(16);  // ~60 FPS
        emit debugLog("[ProjectM] Rendering resumed");
    }
}
