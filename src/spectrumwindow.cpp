#include "spectrumwindow.h"
#include <QVBoxLayout>
#include <QSettings>

SpectrumWindow::SpectrumWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_audioEngine(nullptr)
{
    setWindowTitle("Spectrum Visualizer - Deezer Client");
    resize(600, 400);

    // Don't prevent application from quitting when this window is still open
    setAttribute(Qt::WA_QuitOnClose, false);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_spectrumWidget = new SpectrumWidget(this);
    layout->addWidget(m_spectrumWidget);

    // Restore window geometry if saved
    QSettings settings;
    QByteArray geometry = settings.value("SpectrumWindow/geometry").toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

SpectrumWindow::~SpectrumWindow()
{
}

void SpectrumWindow::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;

    if (m_audioEngine) {
        connect(m_audioEngine, &AudioEngine::spectrumDataReady,
                m_spectrumWidget, &SpectrumWidget::setSpectrumData);
    }
}

void SpectrumWindow::closeEvent(QCloseEvent* event)
{
    // Save geometry before closing
    QSettings settings;
    settings.setValue("SpectrumWindow/geometry", saveGeometry());

    emit closed();
    QWidget::closeEvent(event);
}
