#ifndef PROJECTMWIDGET_H
#define PROJECTMWIDGET_H

// IMPORTANT: GLEW must be included BEFORE any OpenGL headers (including Qt's)
#include <GL/glew.h>

#include <QOpenGLWidget>
#include <QTimer>
#include <QVector>
#include <QMutex>
#include <memory>

// Forward declare projectM to avoid header dependency
namespace libprojectM {
    class ProjectM;
}

class ProjectMWidget : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit ProjectMWidget(QWidget* parent = nullptr);
    ~ProjectMWidget();

    void setSpectrumData(const QVector<float>& magnitudes);
    void setPCMData(const std::vector<float>& leftChannel, const std::vector<float>& rightChannel);
    void nextPreset();
    void previousPreset();
    void randomPreset();
    void loadPreset(const QString& presetPath);
    QString currentPresetName() const;
    QStringList presetFiles() const { return m_presetFiles; }
    void pauseRendering();
    void resumeRendering();

    bool isInitialized() const { return m_initialized; }

signals:
    void debugLog(const QString& message);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void initializeProjectM();
    void generatePCMData();

    std::unique_ptr<libprojectM::ProjectM> m_projectM;
    QTimer* m_renderTimer;

    // Audio data
    QVector<float> m_spectrumData;
    QVector<float> m_pcmDataLeft;
    QVector<float> m_pcmDataRight;
    QMutex m_dataMutex;

    bool m_initialized;
    int m_sampleCount;
    float m_phase;
    uint32_t m_frameCount;

    // Preset management
    QStringList m_presetFiles;      // All available preset file paths
    int m_currentPresetIndex;       // Current preset index in the list
    QString m_currentPreset;        // Current preset file path

    static constexpr int NUM_BANDS = 32;
    static constexpr int PCM_BUFFER_SIZE = 512;
};

#endif // PROJECTMWIDGET_H
